#ifndef LCD_LOGO_H
#define LCD_LOGO_H

/*
 * Boot logo storage.
 *
 * The SD card is only an update source.  If /logo.bin exists it is copied
 * to the external W25Q64 at 0x700000; the display always reads that Flash
 * copy, including when no card is inserted.
 *
 * File format (little-endian header, big-endian RGB565 pixels):
 *   0  "LOGO"
 *   4  u16 width
 *   6  u16 height
 *   8  u16 format (1 = RGB565 big-endian)
 *  10  u16 reserved
 *  12  u32 data_size
 *  16  u32 crc32 (unused by the display)
 *  20  pixel data
 *
 * Returns 0 on success, -1 otherwise.
 */
int lcd_logo_load(void);
int lcd_logo_display(void);

#endif /* LCD_LOGO_H */
