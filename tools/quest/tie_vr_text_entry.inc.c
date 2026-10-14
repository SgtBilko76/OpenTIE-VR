/* ------------------------------------------------------------------------ */
/* Controller text entry                                                    */
/*                                                                          */
/* Horizon OS refuses to open the system keyboard while an immersive app is */
/* running, so clicking a text field with the laser turns on text mode,     */
/* which shows the app's own on-screen keyboard (src/tie_app/vr_keyboard.c, */
/* laser + trigger). In text mode B deletes, Y types a space and A confirms */
/* with Enter. The right stick is not used for letters (too twitchy).       */
/* (Inserted into tie_vr.c by tools/quest/gen_tie_vr.py.)                   */

/* Typed characters reach the game as SDL text events (the string must stay
 * valid until the event is read, hence the static table). */
static void vr_push_text(const char* text) {
	SDL_Event event;

	SDL_zero(event);
	event.type           = SDL_EVENT_TEXT_INPUT;
	event.text.timestamp = SDL_GetTicksNS();
	event.text.text      = text;
	SDL_PushEvent(&event);
}

static void vr_tap_key(SDL_Scancode scancode) {
	vr_push_key(scancode, 1);
	vr_push_key(scancode, 0);
}

static void vr_text_pulse(void) {
	XrHapticActionInfo info      = { XR_TYPE_HAPTIC_ACTION_INFO };
	XrHapticVibration  vibration = { XR_TYPE_HAPTIC_VIBRATION };

	info.action         = g_vr.act_haptic;
	info.subactionPath  = g_vr.hand_path[VR_RIGHT];
	vibration.amplitude = 0.25f;
	vibration.duration  = 15000000; /* 15 ms */
	vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
	p_xrApplyHapticFeedback(g_vr.session, &info, (const XrHapticBaseHeader*)&vibration);
}

static void vr_update_text_entry(int menu, float dt) {
	(void)dt;
	const int a_pressed = g_vr.button_a && !g_vr.text_a_was_down;
	const int b_pressed = g_vr.button_b && !g_vr.text_b_was_down;
	const int y_pressed = g_vr.button_y && !g_vr.text_y_was_down;

	g_vr.text_a_was_down = g_vr.button_a;
	g_vr.text_b_was_down = g_vr.button_b;
	g_vr.text_y_was_down = g_vr.button_y;

	if (!menu) {
		if (g_vr.text_mode) {
			Aeron_LogInfo(VR_LOG, "text entry off (flying)");
		}
		g_vr.text_mode = 0;
		return;
	}
	if (g_vr.text_mode && !g_vr.text_mode_was_on) {
		/* Opened by a text field click (TieVr_SetTextEntryActive). */
		vr_text_pulse();
		Aeron_LogInfo(VR_LOG, "text entry on");
	}
	g_vr.text_mode_was_on = g_vr.text_mode;
	if (!g_vr.text_mode) {
		return;
	}

	if (b_pressed) {
		vr_tap_key(SDL_SCANCODE_BACKSPACE);
		g_vr.dial_pending = 0;
	}
	if (y_pressed) {
		vr_push_text(" ");
		g_vr.dial_pending = 0;
	}
	if (a_pressed) {
		/* A still types Enter through the menu keys; leave text mode. */
		g_vr.text_mode    = 0;
		g_vr.dial_pending = 0;
		Aeron_LogInfo(VR_LOG, "text entry off (Enter)");
	}
}

