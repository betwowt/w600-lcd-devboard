#include "ba_rle.h"

/* Fill `run` pixels starting at bit position `pos` (0 = MSB of dst[0]). */
static void fill_run(unsigned char *dst, unsigned int pos,
                     unsigned int run, unsigned int color)
{
    unsigned int end = pos + run;
    unsigned char full = color ? 0xFFu : 0x00u;

    while (pos < end && (pos & 7u)) {
        unsigned int mask = 0x80u >> (pos & 7u);

        if (color)
            dst[pos >> 3] |= (unsigned char)mask;
        else
            dst[pos >> 3] &= (unsigned char)~mask;
        pos++;
    }
    while (pos + 8u <= end) {
        dst[pos >> 3] = full;
        pos += 8u;
    }
    while (pos < end) {
        unsigned int mask = 0x80u >> (pos & 7u);

        if (color)
            dst[pos >> 3] |= (unsigned char)mask;
        else
            dst[pos >> 3] &= (unsigned char)~mask;
        pos++;
    }
}

int ba_rle_decode(const unsigned char *src, unsigned int len,
                  unsigned char *dst, unsigned int pixels)
{
    unsigned int i = 0;
    unsigned int out = 0;
    unsigned int color = 0;

    if (!src || !dst)
        return -1;

    while (out < pixels) {
        unsigned int run = 0;
        unsigned int shift = 0;
        unsigned char b;

        do {
            if (i >= len || shift >= 32u)
                return -1;
            b = src[i++];
            run |= (unsigned int)(b & 0x7Fu) << shift;
            shift += 7u;
        } while (b & 0x80u);

        if (run > pixels - out)
            return -1;
        if (run)
            fill_run(dst, out, run, color);
        out += run;
        color ^= 1u;
    }
    return 0;
}
