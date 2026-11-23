/*
 * W25Q64 (8 MB) SPI NOR flash on the W600 module dev board.
 *
 *   CS   PB15   (shared SPI flash chip select, active low)
 *   CLK  PA01
 *   MOSI PB18
 *   MISO PB17
 *
 * CLK/MOSI/MISO are shared with the LCD and the TF card socket.  This
 * board's W600 host-SPI receive path returns FF for every Flash mode, while
 * GPIO bit-bang reads the W25Q64JVSIQ ID correctly; the driver therefore
 * uses GPIO bit-bang.  lcd_spi_resume() restores the LCD's host-SPI mode-0
 * setup after every flash transaction.
 */
#ifndef W25QXX_H
#define W25QXX_H

#include "board.h"

#define W25QXX_PAGE_SIZE    256u
#define W25QXX_SECTOR_SIZE  4096u
#define W25QXX_CAPACITY     0x800000u   /* 8 MB */
#define W25QXX_JEDEC_ID     0xEF4017u   /* Winbond W25Q64 */

/* Probe the flash and print the JEDEC ID.  0 = W25Q64/W25Q128 found. */
int w25qxx_init(void);

/* Last JEDEC ID read, 0xRRGGBB. */
u32 w25qxx_id(void);

/* Raw JEDEC ID read (3 bytes).  0 on success. */
int w25qxx_read_id(u8 id[3]);

/* Read len bytes.  0 on success, -1 on bad range / bus error. */
int w25qxx_read(u32 addr, void *buf, u32 len);

/*
 * Erase every 4 KB sector touched by [addr, addr+len), then program the
 * data page by page.  Destructive: bytes outside the range in the first and
 * last sectors are erased too.  For read-modify-write use the sector erase
 * and page program helpers below.
 */
int w25qxx_write(u32 addr, const void *buf, u32 len);

/* Erase one 4 KB sector (addr is rounded down).  0 on success. */
int w25qxx_erase_sector(u32 addr);

/* Program 1..256 bytes without crossing a page boundary.  0 on success. */
int w25qxx_page_program(u32 addr, const void *buf, u16 len);

/* Poll status register bit 0 (WIP).  0 = ready, -1 = timeout. */
int w25qxx_wait_ready(u32 timeout_ms);

#endif /* W25QXX_H */
