#ifndef BA_RLE_H
#define BA_RLE_H

/*
 * Decode a Bad Apple RLE payload into a packed 1-bpp frame.
 *
 * The payload is a sequence of LEB128 run lengths in row-major order,
 * alternating black/white and starting with black.  A zero-length first
 * run encodes a white first pixel.  The runs must cover exactly `pixels`
 * pixels.  Returns 0 on success, -1 on a truncated or oversized stream.
 *
 * Kept free of SDK types so the same file can be compiled and tested on
 * the host.
 */
int ba_rle_decode(const unsigned char *src, unsigned int len,
                  unsigned char *dst, unsigned int pixels);

#endif
