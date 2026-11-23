/*
 * Bad Apple player: 1-bpp frames streamed from /BADAPPLE.BIN on the TF
 * card and expanded to RGB565 straight into the ILI9341.
 *
 * File layout (little-endian, 32-byte header):
 *   0   "BADAPPL1"
 *   8   u16 width       (multiple of 8, <= 240)
 *  10   u16 height      (<= 320)
 *  12   u16 x           (screen offset)
 *  14   u16 y
 *  16   u16 fps
 *  18   u16 flags       (0 = raw frames, 1 = length-prefixed RLE)
 *  20   u32 frame_count
 *  24   u32 frame_bytes ((width / 8) * height, uncompressed)
 *  28   u32 reserved
 *
 * Each frame is row-major, one bit per pixel, MSB first; 1 = white.
 * With flags bit 0 clear every frame is exactly frame_bytes of raw 1-bpp.
 * With bit 0 set each frame starts with a u16 payload length; bit 15 of
 * that length means "raw", the low 15 bits are the byte count.  An RLE
 * payload is a LEB128 run-length stream starting with black; a zero-length
 * first run encodes a white first pixel.  See ba_rle.c.
 * The converter is tools/bad_apple.py.
 */
#include <string.h>

#include "board.h"
#include "badapple.h"
#include "ba_rle.h"
#include "key.h"
#include "lcd.h"
#include "sd_fs.h"

#define BA_NAME         "BADAPPLEBIN"
#define BA_MAGIC        "BADAPPL1"
#define BA_HDR_SIZE     32u
#define BA_MAX_FRAME    (LCD_W / 8u * LCD_H)
#define BA_TICKS_HZ     500u                    /* configTICK_RATE_HZ */
#define BA_FLAG_RLE     1u
#define BA_FRAME_RAW    0x8000u

static u8 g_frame[BA_MAX_FRAME] __attribute__((aligned(4)));
static u8 g_comp[BA_MAX_FRAME] __attribute__((aligned(4)));
static u32 g_pix[256][4] __attribute__((aligned(4)));

static u16 rd16(const u8 *p)
{
    return (u16)(p[0] | ((u16)p[1] << 8));
}

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) |
           ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void pix_init(void)
{
    u32 v;
    int b;

    for (v = 0; v < 256u; v++) {
        u8 *d = (u8 *)g_pix[v];

        for (b = 7; b >= 0; b--) {
            u16 c = (v & (1u << b)) ? 0xFFFFu : 0x0000u;

            *d++ = (u8)(c >> 8);
            *d++ = (u8)c;
        }
    }
}

int badapple_run(void)
{
    u8 hdr[BA_HDR_SIZE];
    u32 file_size;
    u32 frame_count;
    u32 frame_bytes;
    u16 w;
    u16 h;
    u16 x;
    u16 y;
    u16 fps;
    u16 flags;
    u32 frame_ticks;
    u32 next_tick;
    u32 frame;
    u32 stat_frames = 0;
    u32 sd_ticks = 0;
    u32 lcd_ticks = 0;
    u32 stat_tick;
    u16 loop = 0;

    if (sd_mount() != 0) {
        printf("BA: no FAT volume\r\n");
        return -1;
    }
    if (sd_file_open(BA_NAME, &file_size) != 0) {
        printf("BA: /BADAPPLE.BIN not found\r\n");
        return -1;
    }
    if (file_size < BA_HDR_SIZE || sd_file_read(hdr, BA_HDR_SIZE) != 0 ||
        memcmp(hdr, BA_MAGIC, 8) != 0) {
        printf("BA: bad header\r\n");
        sd_file_close();
        return -1;
    }

    w = rd16(hdr + 8);
    h = rd16(hdr + 10);
    x = rd16(hdr + 12);
    y = rd16(hdr + 14);
    fps = rd16(hdr + 16);
    flags = rd16(hdr + 18);
    frame_count = rd32(hdr + 20);
    frame_bytes = rd32(hdr + 24);

    if (w == 0 || h == 0 || (w & 7u) || w > LCD_W || h > LCD_H ||
        (u32)x + w > LCD_W || (u32)y + h > LCD_H ||
        frame_bytes != (u32)(w / 8u) * h ||
        frame_bytes > BA_MAX_FRAME ||
        (flags & ~(u16)BA_FLAG_RLE) != 0 ||
        frame_count == 0 ||
        fps == 0 || fps > 60 ||
        (!(flags & BA_FLAG_RLE) &&
         (file_size - BA_HDR_SIZE) / frame_bytes < frame_count)) {
        printf("BA: invalid geometry %ux%u @%u, %u frames of %u bytes\r\n",
               w, h, fps, (unsigned)frame_count, (unsigned)frame_bytes);
        sd_file_close();
        return -1;
    }

    {
        u32 avg = (flags & BA_FLAG_RLE)
                      ? (file_size - BA_HDR_SIZE) / frame_count
                      : frame_bytes;

        printf("BA: %ux%u at (%u,%u), %u fps, %u frames, %s, %u.%u KB/s\r\n",
               w, h, x, y, fps, (unsigned)frame_count,
               (flags & BA_FLAG_RLE) ? "RLE" : "raw",
               (unsigned)(avg * fps / 1024u),
               (unsigned)((avg * fps % 1024u) * 10u / 1024u));
    }

    pix_init();
    key_init();

    /* Black background outside the letterbox. */
    lcd_fill(0, 0, LCD_W - 1, LCD_H - 1, LCD_BLACK);

    frame_ticks = BA_TICKS_HZ / fps;
    if (frame_ticks == 0)
        frame_ticks = 1;

    for (;;) {
        if (sd_file_rewind() != 0 ||
            sd_file_read(hdr, BA_HDR_SIZE) != 0 ||
            memcmp(hdr, BA_MAGIC, 8) != 0) {
            printf("BA: rewind failed\r\n");
            break;
        }

        next_tick = tls_os_get_time();
        stat_tick = next_tick;

        for (frame = 0; frame < frame_count; frame++) {
            u32 t0;
            u32 t1;
            u32 t2;
            u32 now;

            t0 = tls_os_get_time();
            if (flags & BA_FLAG_RLE) {
                u8 lenbuf[2];
                u16 flen;
                int raw;

                if (sd_file_read(lenbuf, 2u) != 0) {
                    printf("BA: frame %u length failed\r\n",
                           (unsigned)frame);
                    goto out;
                }
                flen = rd16(lenbuf);
                raw = (flen & BA_FRAME_RAW) != 0;
                flen &= (u16)~BA_FRAME_RAW;
                if (flen == 0 || flen > sizeof(g_comp) ||
                    sd_file_read(g_comp, flen) != 0) {
                    printf("BA: frame %u payload failed (%u)\r\n",
                           (unsigned)frame, (unsigned)flen);
                    goto out;
                }
                t1 = tls_os_get_time();
                if (raw) {
                    if (flen != frame_bytes) {
                        printf("BA: frame %u raw size %u != %u\r\n",
                               (unsigned)frame, (unsigned)flen,
                               (unsigned)frame_bytes);
                        goto out;
                    }
                    memcpy(g_frame, g_comp, frame_bytes);
                } else if (ba_rle_decode(g_comp, flen, g_frame,
                                         (u32)w * h) != 0) {
                    printf("BA: frame %u RLE decode failed (%u)\r\n",
                           (unsigned)frame, (unsigned)flen);
                    goto out;
                }
            } else {
                if (sd_file_read(g_frame, frame_bytes) != 0) {
                    printf("BA: frame %u read failed\r\n", (unsigned)frame);
                    goto out;
                }
                t1 = tls_os_get_time();
            }

            lcd_image_1bpp(x, y, w, h, (u16)(w / 8u), g_frame,
                           (const u32 *)g_pix);
            t2 = tls_os_get_time();

            sd_ticks += t1 - t0;
            lcd_ticks += t2 - t1;
            stat_frames++;

            next_tick += frame_ticks;
            now = tls_os_get_time();
            if ((int)(next_tick - now) > 0) {
                tls_os_time_delay(next_tick - now);
            } else if ((u32)(now - next_tick) > BA_TICKS_HZ) {
                next_tick = now;             /* fell behind, resync */
            }

            if ((frame & 3u) == 0 && key_scan() != 0) {
                printf("BA: key pressed, stopping\r\n");
                goto out;
            }

            now = tls_os_get_time();
            if ((now - stat_tick) >= BA_TICKS_HZ * 10u && stat_frames) {
                u32 sd_x10 = sd_ticks * 20u / stat_frames;
                u32 lcd_x10 = lcd_ticks * 20u / stat_frames;

                printf("BA: loop %u frame %u, %u fps, SD %u.%u ms, "
                       "LCD %u.%u ms\r\n",
                       (unsigned)loop, (unsigned)frame,
                       (unsigned)(stat_frames * BA_TICKS_HZ /
                                  (now - stat_tick + 1u)),
                       (unsigned)(sd_x10 / 10u),
                       (unsigned)(sd_x10 % 10u),
                       (unsigned)(lcd_x10 / 10u),
                       (unsigned)(lcd_x10 % 10u));
                stat_tick = tls_os_get_time();
                stat_frames = 0;
                sd_ticks = 0;
                lcd_ticks = 0;
            }
        }

        loop++;
        printf("BA: loop %u done\r\n", (unsigned)loop);
    }

out:
    sd_file_close();
    return 0;
}
