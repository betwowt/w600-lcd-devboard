#ifndef BAD_APPLE_H
#define BAD_APPLE_H

/*
 * Play /BADAPPLE.BIN from the TF card until a board key is pressed.
 * Returns 0 after playback stopped by a key, -1 if the card or file is
 * missing/invalid.  The LCD must already be initialized.
 */
int badapple_run(void);

#endif /* BAD_APPLE_H */
