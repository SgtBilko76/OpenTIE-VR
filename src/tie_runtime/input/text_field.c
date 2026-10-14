/* Frontend text field tracking (see text_field.h). */

#include "tie_runtime/input/text_field.h"

#include <SDL3/SDL.h>

#include "landru/io.h"

enum {
	TEXT_FIELD_SLOTS = 4,
	/* A field counts as on screen this long after its last draw. */
	TEXT_FIELD_VISIBLE_MS = 500,
	/* Extra margin around a field so the laser does not have to hit the
	 * thin box exactly. */
	TEXT_FIELD_MARGIN = 4,
};

typedef struct TieTextFieldSlot {
	int16_t left, top, right, bottom;
	uint64_t drawn_ms;
} TieTextFieldSlot;

static TieTextFieldSlot slots[TEXT_FIELD_SLOTS];

static bool TieTextField_SlotVisible(const TieTextFieldSlot* slot, uint64_t now_ms) {
	return slot->drawn_ms && now_ms - slot->drawn_ms <= TEXT_FIELD_VISIBLE_MS;
}

void TieTextField_Report(int16_t left, int16_t top, int16_t right, int16_t bottom) {
	const uint64_t now_ms = SDL_GetTicks();
	TieTextFieldSlot* target = &slots[0];

	for (int i = 0; i < TEXT_FIELD_SLOTS; ++i) {
		TieTextFieldSlot* slot = &slots[i];
		if (slot->left == left && slot->top == top && slot->right == right && slot->bottom == bottom) {
			target = slot;
			break;
		}
		if (slot->drawn_ms < target->drawn_ms)
			target = slot;
	}
	target->left = left;
	target->top = top;
	target->right = right;
	target->bottom = bottom;
	target->drawn_ms = now_ms;
}

bool TieTextField_Visible(void) {
	const uint64_t now_ms = SDL_GetTicks();
	for (int i = 0; i < TEXT_FIELD_SLOTS; ++i)
		if (TieTextField_SlotVisible(&slots[i], now_ms))
			return true;
	return false;
}

bool TieTextField_UnderMouse(void) {
	const uint64_t now_ms = SDL_GetTicks();
	const int x = xio_Mouse_X();
	const int y = xio_Mouse_Y();

	for (int i = 0; i < TEXT_FIELD_SLOTS; ++i) {
		const TieTextFieldSlot* slot = &slots[i];
		if (TieTextField_SlotVisible(slot, now_ms) && x >= slot->left - TEXT_FIELD_MARGIN &&
			x < slot->right + TEXT_FIELD_MARGIN && y >= slot->top - TEXT_FIELD_MARGIN &&
			y < slot->bottom + TEXT_FIELD_MARGIN)
			return true;
	}
	return false;
}
