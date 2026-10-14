#!/usr/bin/env python3
"""Generate src/tie_vr/tie_vr.c from OpenXWA-VR's src/xwa_vr/xwa_vr.c.

The OpenXR session, layers, pointer and virtual gamepad are engine-level code
shared with the X-Wing Alliance port; only the game hooks differ. This script
renames the API and swaps the XWA game queries for their OpenTIE equivalents.
Re-run after pulling OpenXWA-VR VR fixes; hand edits belong in the
REPLACEMENTS table below so they survive regeneration.
"""
import pathlib
import sys

SRC = pathlib.Path.home() / "Projekte/OpenXWA-VR/src/xwa_vr/xwa_vr.c"
DST = pathlib.Path(__file__).resolve().parents[2] / "src/tie_vr/tie_vr.c"
TEXT_ENTRY = (pathlib.Path(__file__).resolve().parent / "tie_vr_text_entry.inc.c").read_text()
RECENTER = (pathlib.Path(__file__).resolve().parent / "tie_vr_recenter.inc.c").read_text()
COCKPIT_PANELS = (pathlib.Path(__file__).resolve().parent / "tie_vr_cockpit_panels.inc.c").read_text()

REPLACEMENTS = [
    # Flight sticks follow OpenXWA-VR: left = pitch/yaw, right = roll/throttle.
    # Header + includes
    ("/* Quest VR presentation for OpenXWA.", "/* Quest VR presentation for OpenTIE (shared with the X-Wing Alliance VR port)."),
    ('#include "xwa_vr/xwa_vr.h"', '#include "tie_vr/tie_vr.h"'),
    ('#include "xwa_remaster/vr_stereo.h"\n'
     '#include "xwa_runtime/runtime/flight_task.h"\n'
     '#include "xwa/flight/flight_display.h"\n'
     '#include "xwa/flight/hangar.h"\n',
     '#include "tie_app/settings/settings.h"\n'
     '#include "tie_remaster/vr_stereo.h"\n'
     '#include "tie_runtime/runtime/flight_screen.h"\n'
     '#include "tie_runtime/snapshot/snapshot.h"\n'),
    ('#define VR_LOG "xwa.vr"', '#define VR_LOG "tie.vr"'),
    # Game state queries
    ("static int vr_in_flight(void) { return XwaFlightTask_IsActive(); }",
     "static int vr_in_flight(void) {\n"
     "\tconst TieSnapshot* snap = TieSnapshot_Current();\n"
     "\treturn snap && snap->scene_kind == TIE_SCENE_FLIGHT;\n"
     "}"),
    ("/* Screens inside a mission that only understand mouse and keyboard: the\n"
     " * hangar (Space launches, arrows/Enter drive the hangar menu) and the\n"
     " * Escape options screen. They get the menu controls, not the gamepad. */\n"
     "static int vr_flight_menu_active(void) {\n"
     "\treturn g_inHangarReady || FlightDisplay_IsFrontendModalActive();\n"
     "}",
     "/* Screens inside a mission that only understand mouse and keyboard: the\n"
     " * full-screen in-flight pages (goals, map, messages, damage, wingmen, help,\n"
     " * options, replay prompt) and the OpenTIE settings overlay (Escape). They\n"
     " * get the menu controls, not the gamepad. */\n"
     "static int vr_flight_menu_active(void) {\n"
     "\treturn TieFlightScreen_Active() != TIE_FLIGHT_SCREEN_NORMAL || TieSettings_Open();\n"
     "}"),
    # TIE has no hangar launch: the left trigger types nothing in menus.
    ("\tvr_set_key(VR_KEY_LTRIGGER, SDL_SCANCODE_SPACE,\n"
     "\t\t\t   menu && g_inHangarReady && !FlightDisplay_IsFrontendModalActive() &&\n"
     "\t\t\t\t   vr_hysteresis(g_vr.trigger_down[VR_LEFT], g_vr.trigger[VR_LEFT]));\n",
     "\tvr_set_key(VR_KEY_LTRIGGER, SDL_SCANCODE_UNKNOWN, 0);\n"),
    ("type keys: A = Enter, B = Escape, Y = Space, left trigger = Space (hangar\n"
     " * only: launch), menu = Escape, left stick = arrows.",
     "type keys: A = Enter, B = Escape, Y = Space, menu = Escape,\n"
     " * left stick = arrows."),
    # Names
    ('"OpenXWA VR"', '"OpenTIE VR"'),
    ('SDL_strlcpy(set_info.actionSetName, "xwa"', 'SDL_strlcpy(set_info.actionSetName, "tie"'),
    ('"X-Wing Alliance"', '"TIE Fighter"'),
    ('"OPENXWA_VR_DISABLE"', '"OPENTIE_VR_DISABLE"'),
    ("XwaVrStereoFrame", "TieVrStereoFrame"),
    ("XwaVrStereoEye", "TieVrStereoEye"),
    ("XwaRemasterVr_", "TieRemasterVr_"),
    ("XwaVr_Install", "TieVr_Install"),
    # OpenTIE enables SDL text input (pilot names, settings fields); on Quest
    # that pops up the system keyboard over the game. Typing goes through
    # injected keys instead.
    ("\tSDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, VR_PAD_ID_HINT);\n",
     "\tSDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, VR_PAD_ID_HINT);\n"
     "\tSDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, \"0\");\n"),
    # Log the first frame that reaches the virtual screen (bring-up aid).
    ("\t\tg_vr.image_acquired      = 0;\n\t\tg_vr.image_ever_released = 1;\n",
     "\t\tg_vr.image_acquired = 0;\n"
     "\t\tif (!g_vr.image_ever_released) {\n"
     "\t\t\tAeron_LogInfo(VR_LOG, \"first frame presented to the virtual screen\");\n"
     "\t\t}\n"
     "\t\tg_vr.image_ever_released = 1;\n"),
    # Controller text entry ("letter dial", see tools/quest/tie_vr_text_entry.inc.c):
    # Horizon OS will not open its system keyboard over an immersive app.
    ("/* Menus (concourse, briefing, options, and in a mission the hangar and the\n",
     TEXT_ENTRY + "/* Menus (concourse, briefing, options, and in a mission the hangar and the\n"),
    ("\tvr_set_key(VR_KEY_B, SDL_SCANCODE_ESCAPE, menu && g_vr.button_b);\n",
     "\tvr_set_key(VR_KEY_B, SDL_SCANCODE_ESCAPE, menu && !g_vr.text_mode && g_vr.button_b);\n"),
    ("\tvr_set_key(VR_KEY_Y, SDL_SCANCODE_SPACE, menu && g_vr.button_y);\n",
     "\tvr_set_key(VR_KEY_Y, SDL_SCANCODE_SPACE, menu && !g_vr.text_mode && g_vr.button_y);\n"),
    ("\tvr_update_menu_keys(flying);\n",
     "\tvr_update_text_entry(!flying, dt);\n\tvr_update_menu_keys(flying);\n"),
    ("\tint                   button_a, button_b, button_x, button_y, button_menu;\n",
     "\tint                   button_a, button_b, button_x, button_y, button_menu;\n"
     "\t/* Controller text entry */\n"
     "\tint                   text_mode;\n"
     "\tint                   text_mode_was_on;\n"
     "\tint                   text_a_was_down, text_b_was_down, text_y_was_down;\n"
     "\tint                   dial_index, dial_pending, dial_dir_x, dial_dir_y;\n"
     "\tfloat                 dial_repeat_s;\n"),
    ("type keys: A = Enter, B = Escape, Y = Space, menu = Escape,\n",
     "type keys: A = Enter, B = Escape, Y = Space, menu = Escape (a laser\n"
     " * click on a text field opens the keyboard, vr_update_text_entry),\n"),
    # Left stick arrows: one tap per deliberate push (holding a key made the
    # game auto-repeat, so menus kept moving), re-armed only after the stick
    # returns near the centre; off while the on-screen keyboard is open.
    ("\tvr_set_key(VR_KEY_UP, SDL_SCANCODE_UP, menu && ly > (g_vr.held_key[VR_KEY_UP] ? 0.4f : 0.7f));\n"
     "\tvr_set_key(VR_KEY_DOWN, SDL_SCANCODE_DOWN, menu && ly < -(g_vr.held_key[VR_KEY_DOWN] ? 0.4f : 0.7f));\n"
     "\tvr_set_key(VR_KEY_LEFT, SDL_SCANCODE_LEFT, menu && lx < -(g_vr.held_key[VR_KEY_LEFT] ? 0.4f : 0.7f));\n"
     "\tvr_set_key(VR_KEY_RIGHT, SDL_SCANCODE_RIGHT, menu && lx > (g_vr.held_key[VR_KEY_RIGHT] ? 0.4f : 0.7f));\n",
     "\tvr_update_menu_arrows(menu && !g_vr.text_mode, lx, ly);\n"),
    ("/* Menus (concourse, briefing, options, and in a mission the hangar and the\n",
     "static void vr_update_menu_arrows(int enabled, float lx, float ly) {\n"
     "\tconst float mag = SDL_sqrtf(lx * lx + ly * ly);\n"
     "\tSDL_Scancode key = SDL_SCANCODE_UNKNOWN;\n"
     "\n"
     "\tif (!enabled || mag < 0.3f) {\n"
     "\t\tg_vr.arrow_armed = 1;\n"
     "\t\treturn;\n"
     "\t}\n"
     "\tif (!g_vr.arrow_armed || mag < 0.85f) {\n"
     "\t\treturn;\n"
     "\t}\n"
     "\tif (SDL_fabsf(ly) >= SDL_fabsf(lx)) {\n"
     "\t\tkey = ly > 0.0f ? SDL_SCANCODE_UP : SDL_SCANCODE_DOWN;\n"
     "\t} else {\n"
     "\t\tkey = lx > 0.0f ? SDL_SCANCODE_RIGHT : SDL_SCANCODE_LEFT;\n"
     "\t}\n"
     "\tvr_push_key(key, 1);\n"
     "\tvr_push_key(key, 0);\n"
     "\tg_vr.arrow_armed = 0;\n"
     "}\n"
     "\n"
     "/* Menus (concourse, briefing, options, and in a mission the hangar and the\n"),
    ("\tint                   text_mode;\n",
     "\tint                   text_mode;\n"
     "\tint                   arrow_armed;\n"),
    ("int TieVr_Install(void) {\n",
     "int TieVr_TextEntryActive(void) { return g_vr.text_mode; }\n\n"
     "void TieVr_SetTextEntryActive(int active) {\n"
     "\tg_vr.text_mode    = active ? 1 : 0;\n"
     "\tg_vr.dial_pending = 0;\n"
     "}\n\n"
     "int TieVr_Install(void) {\n"),
    ("int TieVr_Install(void) { return 0; }\n",
     "int TieVr_Install(void) { return 0; }\n"
     "int TieVr_TextEntryActive(void) { return 0; }\n"
     "void TieVr_SetTextEntryActive(int active) { (void)active; }\n"),
    # Menus: the pad stays neutral except the throttle lever (LEFTY), which
    # OpenTIE's Quest defaults bind to the absolute throttle axis (applied
    # only when it moves). Pitch/yaw are on the right stick, which TIE's menu
    # cursor reads, and those stay centred (tools/quest/quest_config.py).
    # Recenter: hold left Menu 0.8 s (tools/quest/tie_vr_recenter.inc.c);
    # a short press is Escape / Start.
    ("static void vr_update_input(void) {\n", RECENTER + "static void vr_update_input(void) {\n"),
    ("static void vr_poll_events(void) {\n",
     "static void vr_set_local_space(const XrPosef* pose);\n\nstatic void vr_poll_events(void) {\n"),
    ("\t\t} else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {\n",
     "\t\t} else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {\n"
     "\t\t\t/* System recenter: drop any app recenter offset. */\n"
     "\t\t\tvr_set_local_space(NULL);\n"
     "\t\t\tAeron_LogInfo(VR_LOG, \"system recenter\");\n"
     "\t\t} else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {\n"),
    ("\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_START, g_vr.button_menu);\n",
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_START, g_vr.menu_tap_frames > 0);\n"),
    ("\tvr_set_key(VR_KEY_MENU, SDL_SCANCODE_ESCAPE, menu && g_vr.button_menu);\n",
     "\tvr_set_key(VR_KEY_MENU, SDL_SCANCODE_ESCAPE, menu && g_vr.menu_tap_frames > 0);\n"),
    ("\tvr_update_gamepad(flying, dt);\n", "\tvr_update_menu_button(dt);\n\tvr_update_gamepad(flying, dt);\n"),
    ("\tint                   arrow_armed;\n",
     "\tint                   arrow_armed;\n"
     "\tfloat                 menu_held_s;\n"
     "\tint                   menu_long_done, menu_tap_frames;\n"),
    # 3D cockpit: instruments on quads mounted in the 3D model, HUD limited to
    # the window region (tools/quest/tie_vr_cockpit_panels.inc.c).
    ('#include "tie_remaster/vr_stereo.h"\n',
     '#include "tie_remaster/vr_stereo.h"\n#include "tie_vr/tie_vr_cockpit_panels.h"\n'),
    ("static void vr_end_xr_frame(void) {\n", COCKPIT_PANELS + "static void vr_end_xr_frame(void) {\n"),
    ("\tconst XrCompositionLayerBaseHeader* layers[3];\n",
     "\tconst XrCompositionLayerBaseHeader* layers[20];\n"
     "\tXrCompositionLayerQuad             panel_quads[16];\n"),
    ("\t\t\t\tquad.size.height     = VR_HUD_DISTANCE * (hud_tan[2] + hud_tan[3]);\n"
     "\t\t\t}\n"
     "\t\t\tlayers[layer_count++]                 = (const XrCompositionLayerBaseHeader*)&quad;\n",
     "\t\t\t\tquad.size.height     = VR_HUD_DISTANCE * (hud_tan[2] + hud_tan[3]);\n"
     "\t\t\t}\n"
     "\t\t\tlayers[layer_count++]                 = (const XrCompositionLayerBaseHeader*)&quad;\n"
     "\t\t\tif (stereo && TieRemasterVr_HudTangents(&hud_tan[0], &hud_tan[1], &hud_tan[2], &hud_tan[3])) {\n"
     "\t\t\t\tlayer_count = vr_add_cockpit_panels(&quad, hud_tan, layers, layer_count,\n"
     "\t\t\t\t\t\t\t\t\t\t\t\t  (uint32_t)SDL_arraysize(layers), panel_quads);\n"
     "\t\t\t}\n"),
]


def main() -> int:
    text = SRC.read_text()
    for old, new in REPLACEMENTS:
        if old not in text:
            print(f"pattern not found: {old[:70]!r}", file=sys.stderr)
            return 1
        text = text.replace(old, new)
    leftovers = [w for w in ("Xwa", "xwa", "XWA", "g_inHangar") if w in text]
    if leftovers:
        print(f"unconverted identifiers remain: {leftovers}", file=sys.stderr)
        return 1
    DST.parent.mkdir(parents=True, exist_ok=True)
    DST.write_text(text)
    print(f"wrote {DST}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
