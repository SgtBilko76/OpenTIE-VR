#ifndef TIE_APP_VR_KEYBOARD_H
#define TIE_APP_VR_KEYBOARD_H

#include "aeron/aeron.h"
#include "tie_app/ui.h"

/* On-screen keyboard for the Quest build. Horizon OS does not show its
 * system keyboard over an immersive app, so when the controller laser
 * clicks a text field (pilot name, password) a key grid is drawn with the
 * app UI and clicked with the laser. It closes with Enter/Close or when no
 * text field is on screen any more. Keys reach the game as ordinary SDL
 * text/key events. Does nothing outside the Quest build. */
void TieVrKeyboard_Init(TieUi* ui);
void TieVrKeyboard_Frame(const AeronInputSnapshot* input, float dt_seconds);

#endif
