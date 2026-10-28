#!/usr/bin/env python3
"""Generate src/tie_vr/tie_vr.c from OpenXWA-VR's src/xwa_vr/xwa_vr.c.

The OpenXR session, layers, pointer and virtual gamepad are engine-level code
shared with the X-Wing Alliance port; only the game hooks differ. This script
renames the API and swaps the XWA game queries for their OpenTIE equivalents.
Hand edits belong in the REPLACEMENTS table below so they survive
regeneration.

The source is pinned to OpenXWA-VR commit XWA_BASE (read with `git show`, so
the OpenXWA-VR checkout may be on any commit). Later OpenXWA-VR features are
ported here as REPLACEMENTS / .inc.c files (e.g. the radial menus) rather than
by moving the base.
"""
import pathlib
import re
import subprocess
import sys

XWA_REPO = pathlib.Path.home() / "Projekte/OpenXWA-VR"
XWA_BASE = "dfb8972"  # OpenXWA VR 0.1.2-beta: Quest/PICO controller profiles
SRC_PATH = "src/xwa_vr/xwa_vr.c"
DST = pathlib.Path(__file__).resolve().parents[2] / "src/tie_vr/tie_vr.c"
TEXT_ENTRY = (pathlib.Path(__file__).resolve().parent / "tie_vr_text_entry.inc.c").read_text()
RECENTER = (pathlib.Path(__file__).resolve().parent / "tie_vr_recenter.inc.c").read_text()
COCKPIT_PANELS = (pathlib.Path(__file__).resolve().parent / "tie_vr_cockpit_panels.inc.c").read_text()
RADIAL = (pathlib.Path(__file__).resolve().parent / "tie_vr_radial.inc.c").read_text()

# Radial menus: constants, panel/menu state and the forward declaration used by
# vr_device_created (the functions live in tools/quest/tie_vr_radial.inc.c).
RADIAL_TYPES = """/* Radial menus: a panel above the controller, facing the head. */
static const float    VR_RADIAL_SIZE_M    = 0.30f;
static const float    VR_RADIAL_LIFT_M    = 0.17f; /* panel centre above the controller */
static const float    VR_RADIAL_PICK      = 0.55f; /* stick deflection that selects */
static const float    VR_RADIAL_KEEP      = 0.30f; /* ...and keeps the selection */
static const uint64_t VR_RADIAL_LINGER_MS = 250;   /* selection survives the stick springing back */

/* A quad layer with its own swapchain, drawn on the CPU (radial menus). */
typedef struct VrPanel {
\tXrSwapchain            swapchain;
\tSDL_GPUTexture**       images;
\tSDL_GPUTransferBuffer* transfer;
\tuint8_t*               pixels;
\tint                    width, height;
\tint                    ever_released;
\tint                    dirty;
} VrPanel;

typedef struct VrRadialState {
\tVrPanel  panel;
\tint      visible;
\tint      selected; /* VR_RADIAL_NONE or item */
\tint      drawn_selected;
\tuint64_t selected_ms; /* last time the stick pointed at `selected` */
\tXrPosef  pose;        /* panel pose in LOCAL space */
\tint      pose_valid;
} VrRadialState;

"""

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

# Applied after REPLACEMENTS; each pattern must occur exactly once.
RADIAL_REPLACEMENTS = [
    # Radial menus (tools/quest/tie_vr_radial.inc.c, port of OpenXWA-VR 0.1.8):
    # holding a grip in flight shows that hand's star of eight commands, the
    # same hand's stick picks one, releasing the grip runs it. They replace the
    # left-grip shift layer and the right grip's high-angle view (not needed
    # in VR, so it is no longer reachable from the controllers), so the pad no
    # longer sends the shoulders, D-pad, back or guide
    # (tools/quest/quest_config.py drops those bindings).
    ('#include "tie_remaster/vr_stereo.h"\n#include "tie_vr/tie_vr_cockpit_panels.h"\n',
     '#include "tie_remaster/vr_stereo.h"\n#include "tie_vr/tie_vr_cockpit_panels.h"\n'
     '#include "tie_runtime/input/actions.h"\n#include "tie_vr/vr_radial_menu.h"\n'),
    ("static struct {\n\t/* OpenXR */\n",
     RADIAL_TYPES + "static struct {\n\t/* OpenXR */\n"),
    ("\tSDL_Scancode          held_key[VR_KEY_SLOT_COUNT];\n} g_vr;\n",
     "\tSDL_Scancode          held_key[VR_KEY_SLOT_COUNT];\n"
     "\t/* In-flight radial menus, one per hand */\n"
     "\tVrRadialState         radial[VR_HAND_COUNT];\n"
     "\tint                   panel_bgra;\n"
     "\tXrVector3f            head_pos; /* last known head position (LOCAL) */\n"
     "} g_vr;\n\n"
     "static void vr_create_radials(SDL_GPUTextureFormat format, const XrSwapchainCreateInfo* base);\n"),
    ("\t\t\t   \"SDL_CreateGPUXRSwapchain\")) {\n\t\treturn 0;\n\t}\n",
     "\t\t\t   \"SDL_CreateGPUXRSwapchain\")) {\n\t\treturn 0;\n\t}\n"
     "\tvr_create_radials(format, &swap_info);\n"),
    ("/* The Touch controllers as one gamepad, mirrored 1:1",
     RADIAL + "/* The Touch controllers as one gamepad, mirrored 1:1"),
    (" * The left grip is a shift key: while held, Y/B/A/X become D-pad\n"
     " * up/right/down/left, the right stick click becomes back and the left stick\n"
     " * click becomes guide and the left trigger becomes the left shoulder. */\n",
     " * The grips open the radial menus (vr_update_radials) and are not part of\n"
     " * the pad; a stick choosing in its menu sends nothing. */\n"),
    ("\tconst int        shift = g_vr.grip[VR_LEFT] > 0.5f;\n", ""),
    ("\tlever_input = SDL_fabsf(left.y) > SDL_fabsf(left.x) * 0.75f ? left.y : 0.0f;\n",
     "\tlever_input = SDL_fabsf(left.y) > SDL_fabsf(left.x) * 0.75f ? left.y : 0.0f;\n"
     "\tif (g_vr.radial[VR_RIGHT].visible) {\n"
     "\t\troll        = 0.0f;\n"
     "\t\tlever_input = 0.0f;\n"
     "\t}\n"),
    ("\tSDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_RIGHTX, vr_axis(right.x));\n"
     "\tSDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_RIGHTY, vr_axis(-right.y)); /* SDL: up is negative */\n",
     "\tSDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_RIGHTX,\n"
     "\t\t\t\t\t\t\t   g_vr.radial[VR_LEFT].visible ? 0 : vr_axis(right.x));\n"
     "\tSDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_RIGHTY,\n"
     "\t\t\t\t\t\t\t   g_vr.radial[VR_LEFT].visible ? 0 : vr_axis(-right.y)); /* SDL: up is negative */\n"),
    ("\tSDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, vr_trigger_axis(shift ? 0.0f : g_vr.trigger[VR_LEFT]));\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,\n"
     "\t\t\t\t\t\t\t\t shift && vr_hysteresis(g_vr.trigger_down[VR_LEFT], g_vr.trigger[VR_LEFT]));\n",
     "\tSDL_SetJoystickVirtualAxis(g_vr.pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, vr_trigger_axis(g_vr.trigger[VR_LEFT]));\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0);\n"),
    ("\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_SOUTH, !shift && g_vr.button_a);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_EAST, !shift && g_vr.button_b);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_WEST, !shift && g_vr.button_x);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_NORTH, !shift && g_vr.button_y);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, shift && g_vr.button_a);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, shift && g_vr.button_b);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT, shift && g_vr.button_x);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_DPAD_UP, shift && g_vr.button_y);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,\n"
     "\t\t\t\t\t\t\t\t vr_hysteresis(g_vr.grip_down[VR_RIGHT], g_vr.grip[VR_RIGHT]));\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_LEFT_STICK, !shift && g_vr.stick_click[VR_RIGHT]);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_GUIDE, shift && g_vr.stick_click[VR_RIGHT]);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK, !shift && g_vr.stick_click[VR_LEFT]);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_BACK, shift && g_vr.stick_click[VR_LEFT]);\n",
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_SOUTH, g_vr.button_a);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_EAST, g_vr.button_b);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_WEST, g_vr.button_x);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_NORTH, g_vr.button_y);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_DPAD_UP, 0);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_LEFT_STICK, g_vr.stick_click[VR_RIGHT]);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_GUIDE, 0);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK, g_vr.stick_click[VR_LEFT]);\n"
     "\tSDL_SetJoystickVirtualButton(g_vr.pad, SDL_GAMEPAD_BUTTON_BACK, 0);\n"),
    ("\tvr_update_menu_button(dt);\n\tvr_update_gamepad(flying, dt);\n",
     "\tvr_update_menu_button(dt);\n\tvr_update_radials(flying);\n\tvr_update_gamepad(flying, dt);\n"),
    ("\tvr_update_input();\n\tvr_prepare_stereo();\n",
     "\tvr_update_input();\n\tvr_upload_radials();\n\tvr_prepare_stereo();\n"),
    ("\tg_vr.views_valid      = 1;\n\tg_vr.stereo_published = 1;\n",
     "\tg_vr.head_pos.x       = (g_vr.views[0].pose.position.x + g_vr.views[1].pose.position.x) * 0.5f;\n"
     "\tg_vr.head_pos.y       = (g_vr.views[0].pose.position.y + g_vr.views[1].pose.position.y) * 0.5f;\n"
     "\tg_vr.head_pos.z       = (g_vr.views[0].pose.position.z + g_vr.views[1].pose.position.z) * 0.5f;\n"
     "\tg_vr.views_valid      = 1;\n\tg_vr.stereo_published = 1;\n"),
    ("\tconst XrCompositionLayerBaseHeader* layers[20];\n"
     "\tXrCompositionLayerQuad             panel_quads[16];\n",
     "\tconst XrCompositionLayerBaseHeader* layers[24];\n"
     "\tXrCompositionLayerQuad             panel_quads[16];\n"
     "\tXrCompositionLayerQuad             radial_quads[VR_HAND_COUNT];\n"),
    ("\t\t\t\t\t\t\t\t\t\t\t\t  (uint32_t)SDL_arraysize(layers), panel_quads);\n"
     "\t\t\t}\n\t\t}\n",
     "\t\t\t\t\t\t\t\t\t\t\t\t  (uint32_t)SDL_arraysize(layers), panel_quads);\n"
     "\t\t\t}\n\t\t}\n"
     "\t\tlayer_count = vr_add_radials(layers, layer_count, (uint32_t)SDL_arraysize(layers), radial_quads);\n"),
    ("\tif (g_vr.passthrough && p_xrDestroyPassthroughFB) {\n\t\tp_xrDestroyPassthroughFB(g_vr.passthrough);\n\t}\n",
     "\tif (g_vr.passthrough && p_xrDestroyPassthroughFB) {\n\t\tp_xrDestroyPassthroughFB(g_vr.passthrough);\n\t}\n"
     "\tfor (int hand = 0; hand < VR_HAND_COUNT; ++hand) {\n"
     "\t\tvr_panel_destroy(device, &g_vr.radial[hand].panel);\n"
     "\t}\n"),
]


# Passthrough removed (as in OpenXWA-VR 0.1.8): the menus float in front of
# black on every headset. Applied last; each pattern must occur exactly once.
PASSTHROUGH_REMOVALS = [
    (" * virtual screen in front of the player, over Quest passthrough in the menus\n"
     " * and over black space in flight.\n",
     " * virtual screen in front of the player, over black in the menus and over\n"
     " * space in flight.\n"),
    ("static PFN_xrCreatePassthroughFB       p_xrCreatePassthroughFB;\n"
     "static PFN_xrPassthroughStartFB        p_xrPassthroughStartFB;\n"
     "static PFN_xrCreatePassthroughLayerFB  p_xrCreatePassthroughLayerFB;\n"
     "static PFN_xrDestroyPassthroughFB      p_xrDestroyPassthroughFB;\n"
     "static PFN_xrDestroyPassthroughLayerFB p_xrDestroyPassthroughLayerFB;\n", ""),
    ("\t/* Passthrough (XR_FB_passthrough) */\n"
     "\tint                   want_passthrough;\n"
     "\tint                   has_passthrough;\n"
     "\tXrPassthroughFB       passthrough;\n"
     "\tXrPassthroughLayerFB  passthrough_layer;\n", ""),
    ("\tXR_LOAD_OPTIONAL(xrCreatePassthroughFB)\n"
     "\tXR_LOAD_OPTIONAL(xrPassthroughStartFB)\n"
     "\tXR_LOAD_OPTIONAL(xrCreatePassthroughLayerFB)\n"
     "\tXR_LOAD_OPTIONAL(xrDestroyPassthroughFB)\n"
     "\tXR_LOAD_OPTIONAL(xrDestroyPassthroughLayerFB)\n", ""),
    ("\t\tXR_FB_PASSTHROUGH_EXTENSION_NAME,\n", ""),
    ("\t\tif (strcmp(g_vr_extensions[i], XR_FB_PASSTHROUGH_EXTENSION_NAME) == 0) {\n"
     "\t\t\tg_vr.want_passthrough = 1;\n"
     "\t\t} else if (", "\t\tif ("),
    ("\tvr_create_passthrough();\n", ""),
    ("\tXrCompositionLayerPassthroughFB    passthrough = { XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB };\n", ""),
    ("\tconst int                          in_flight   = vr_in_flight();\n"
     "\tconst int stereo = g_vr.stereo_published && g_vr.views_valid && TieRemasterVr_StereoRendered();\n",
     "\tconst int stereo = g_vr.stereo_published && g_vr.views_valid && TieRemasterVr_StereoRendered();\n"),
    ("\t\tif (g_vr.has_passthrough && !in_flight) {\n"
     "\t\t\tpassthrough.flags       = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;\n"
     "\t\t\tpassthrough.space       = XR_NULL_HANDLE;\n"
     "\t\t\tpassthrough.layerHandle = g_vr.passthrough_layer;\n"
     "\t\t\tlayers[layer_count++]   = (const XrCompositionLayerBaseHeader*)&passthrough;\n"
     "\t\t}\n", ""),
    ("\tif (g_vr.passthrough_layer && p_xrDestroyPassthroughLayerFB) {\n"
     "\t\tp_xrDestroyPassthroughLayerFB(g_vr.passthrough_layer);\n"
     "\t}\n"
     "\tif (g_vr.passthrough && p_xrDestroyPassthroughFB) {\n"
     "\t\tp_xrDestroyPassthroughFB(g_vr.passthrough);\n"
     "\t}\n", ""),
]
PASSTHROUGH_FUNCTION = re.compile(r"static void vr_create_passthrough\(void\) \{\n.*?\n\}\n\n", re.S)


# Pitch inverted on the left stick (Bilko's choice): pushing the stick up now
# reports +RIGHTY (stick down on an SDL pad) instead of -RIGHTY. Applied after
# RADIAL_REPLACEMENTS; must occur exactly once.
PITCH_INVERT = [
    ("\t\t\t\t\t\t\t   g_vr.radial[VR_LEFT].visible ? 0 : vr_axis(-right.y)); /* SDL: up is negative */\n",
     "\t\t\t\t\t\t\t   g_vr.radial[VR_LEFT].visible ? 0 : vr_axis(right.y)); /* inverted pitch: stick up = SDL down */\n"),
]


def base_source() -> str:
    return subprocess.run(["git", "-C", str(XWA_REPO), "show", f"{XWA_BASE}:{SRC_PATH}"],
                          check=True, capture_output=True, text=True).stdout


def main() -> int:
    text = base_source()
    for old, new in REPLACEMENTS:
        if old not in text:
            print(f"pattern not found: {old[:70]!r}", file=sys.stderr)
            return 1
        text = text.replace(old, new)
    for old, new in RADIAL_REPLACEMENTS + PITCH_INVERT + PASSTHROUGH_REMOVALS:
        if text.count(old) != 1:
            print(f"pattern found {text.count(old)} times (want 1): {old[:70]!r}", file=sys.stderr)
            return 1
        text = text.replace(old, new)
    text, n = PASSTHROUGH_FUNCTION.subn("", text)
    if n != 1:
        print(f"vr_create_passthrough found {n} times (want 1)", file=sys.stderr)
        return 1
    if re.search(r"(?i)passthrough", text):
        print("passthrough code remains", file=sys.stderr)
        return 1
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
