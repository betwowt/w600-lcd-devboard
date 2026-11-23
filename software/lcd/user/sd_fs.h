/*
 * Minimal SD card + FAT16/FAT32 reader for the shared SPI bus.
 *
 * The W600 hardware SPI controller misreads the card's R1, so the whole
 * SD path (commands and data) runs on a GPIO bit-bang.  Files are opened
 * by their padded 8.3 name ("LOGO    BIN"), always in the root directory,
 * and read as a forward stream.
 */
#ifndef SD_FS_H
#define SD_FS_H

#include "board.h"

#define SD_NAME_LEN     11
#define SD_BLOCK_SIZE   512u

/* 0 = mounted, -1 = card/init failed, -2 = no FAT volume. Idempotent. */
int sd_mount(void);

/* Capacity of the mounted volume in KiB. */
u32 sd_card_kb(void);

/* Board-check interface: fills msg with "FAT32 150000KB" etc. */
int sd_card_probe(char *msg, u32 msg_len);

/* Open a root-directory file by its 11-byte padded 8.3 name. */
int sd_file_open(const char name11[SD_NAME_LEN], u32 *size);
int sd_file_rewind(void);
int sd_file_read(void *buf, u32 len);
void sd_file_close(void);

#endif /* SD_FS_H */
