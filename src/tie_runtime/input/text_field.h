#ifndef TIE_RUNTIME_INPUT_TEXT_FIELD_H
#define TIE_RUNTIME_INPUT_TEXT_FIELD_H

#include <stdbool.h>
#include <stdint.h>

/* Frontend text fields (pilot name, pilot password) report their screen
 * frame every time they are drawn. The Quest build uses this to open its
 * on-screen keyboard when the pointer clicks a field, and to close it again
 * once no field is on screen. */
void TieTextField_Report(int16_t left, int16_t top, int16_t right, int16_t bottom);

/* True while a text field was drawn within the last moment. */
bool TieTextField_Visible(void);

/* True when the frontend mouse position is inside a visible text field. */
bool TieTextField_UnderMouse(void);

#endif
