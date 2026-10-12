/* Quest on-screen keyboard (see vr_keyboard.h). */

#include "tie_app/vr_keyboard.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>

#include "aeron/render.h"
#include "aeron/scene/ui.h"
#include "tie_app/settings/settings.h"
#include "tie_runtime/input/input.h"
#include "tie_vr/tie_vr.h"

enum {
	/* Frames the game keeps ignoring mouse buttons after the keyboard
	 * closes, so the click on "Close" does not land in the game. */
	VR_KEYBOARD_RELEASE_FRAMES = 15,
};

static AeronUiContext* g_ui;
static bool            g_shift = true;
static int             g_block_frames;
/* SDL text events reference their string until they are read. */
static char g_chars[128][2];

void TieVrKeyboard_Init(TieUi* ui) {
	g_ui = ui ? TieUi_Context(ui) : NULL;
	for (int c = 0; c < 128; ++c) {
		g_chars[c][0] = (char)c;
		g_chars[c][1] = '\0';
	}
}

static void TieVrKeyboard_PushText(char c) {
	SDL_Event event;

	Aeron_LogInfo("tie.vr", "keyboard: '%c'", c);
	SDL_zero(event);
	event.type           = SDL_EVENT_TEXT_INPUT;
	event.text.timestamp = SDL_GetTicksNS();
	event.text.text      = g_chars[(unsigned char)c & 0x7f];
	SDL_PushEvent(&event);
}

static void TieVrKeyboard_TapKey(SDL_Scancode scancode) {
	Aeron_LogInfo("tie.vr", "keyboard: %s", SDL_GetScancodeName(scancode));
	for (int down = 1; down >= 0; --down) {
		SDL_Event event;
		SDL_zero(event);
		event.type          = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
		event.key.timestamp = SDL_GetTicksNS();
		event.key.scancode  = scancode;
		event.key.key       = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false);
		event.key.down      = down ? true : false;
		SDL_PushEvent(&event);
	}
}

static void TieVrKeyboard_Row(const char* keys) {
	const int count = (int)SDL_strlen(keys);

	AeronUi_BeginColumns(g_ui, count, NULL);
	for (int i = 0; i < count; ++i) {
		char c = keys[i];
		if (!g_shift && c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		if (i > 0)
			AeronUi_NextColumn(g_ui);
		if (AeronUi_Button(g_ui, g_chars[(unsigned char)c]))
			TieVrKeyboard_PushText(c);
	}
	AeronUi_EndColumns(g_ui);
}

void TieVrKeyboard_Frame(const AeronInputSnapshot* input, float dt_seconds) {
	const bool visible = g_ui && input && TieVr_TextEntryActive() && !TieSettings_Open();

	if (!visible) {
		if (g_block_frames > 0)
			--g_block_frames;
		TieInput_SetOverlayMouseBlocked(g_block_frames > 0);
		return;
	}
	/* The keyboard owns the pointer: game screens must not see its clicks. */
	g_block_frames = VR_KEYBOARD_RELEASE_FRAMES;

	TieInput_SetOverlayMouseBlocked(true);

	AeronUi_BeginFrame(g_ui, &(AeronUiFrameDesc) { .input = input, .dt_seconds = dt_seconds });
	/* Top of the screen, horizontally centred: text fields (the pilot name
	 * on the registration screen) sit in the lower half and stay visible. */
	AeronUiWindowDesc window = { .width_ref = 960.0f, .y_ref = 12.0f, .flags = AERON_UI_WINDOW_NO_TITLE };
	AeronRectI        content;
	if (Aeron_GetContentPixelRect(&content) && content.height > 0)
		window.x_ref = ((float)content.width * 1080.0f / (float)content.height - window.width_ref) * 0.5f;
	if (AeronUi_BeginWindow(g_ui, "Keyboard", &window)) {
		TieVrKeyboard_Row("1234567890");
		TieVrKeyboard_Row("QWERTYUIOP");
		TieVrKeyboard_Row("ASDFGHJKL'");
		TieVrKeyboard_Row("ZXCVBNM-.");
		AeronUi_BeginColumns(g_ui, 5, NULL);
		if (AeronUi_Button(g_ui, g_shift ? "abc" : "ABC"))
			g_shift = !g_shift;
		AeronUi_NextColumn(g_ui);
		if (AeronUi_Button(g_ui, "Space"))
			TieVrKeyboard_PushText(' ');
		AeronUi_NextColumn(g_ui);
		if (AeronUi_Button(g_ui, "Delete"))
			TieVrKeyboard_TapKey(SDL_SCANCODE_BACKSPACE);
		AeronUi_NextColumn(g_ui);
		if (AeronUi_Button(g_ui, "Enter")) {
			TieVrKeyboard_TapKey(SDL_SCANCODE_RETURN);
			TieVr_SetTextEntryActive(0);
		}
		AeronUi_NextColumn(g_ui);
		if (AeronUi_Button(g_ui, "Close"))
			TieVr_SetTextEntryActive(0);
		AeronUi_EndColumns(g_ui);
		AeronUi_EndWindow(g_ui);
	}
	AeronUi_EndFrame(g_ui);
	AeronUi_Submit(g_ui);
}
