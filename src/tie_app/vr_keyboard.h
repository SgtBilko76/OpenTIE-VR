#ifndef TIE_APP_VR_KEYBOARD_H
#define TIE_APP_VR_KEYBOARD_H

#include "aeron/aeron.h"
#include "tie_app/ui.h"

/* On-screen keyboard for the Quest build. Horizon OS does not show its
 * system keyboard over an immersive app, so while VR text entry is active
 * (X in menus) a key grid is drawn with the app UI and clicked with the
 * controller laser. Keys reach the game as ordinary SDL text/key events.
 * Does nothing unless TieVr_TextEntryActive() reports text entry. */
void TieVrKeyboard_Init(TieUi* ui);
void TieVrKeyboard_Frame(const AeronInputSnapshot* input, float dt_seconds);

#endif
