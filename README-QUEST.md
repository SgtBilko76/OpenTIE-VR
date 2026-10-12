# OpenTIE VR for Meta Quest

Standalone Meta Quest 3 port of OpenTIE (Star Wars: TIE Fighter) with
OpenXR: menus on a virtual screen over passthrough, head-tracked stereo 3D
space in flight, a 3D TIE Fighter cockpit with the original instruments, and
Touch controller support.

You need your own copy of TIE Fighter (Steam "STAR WARS TIE Fighter Special
Edition"). The APK contains no game data.

## Install

1. Install the APK with SideQuest or `adb install OpenTIE-VR-*.apk`.
2. Copy the game data to the Quest's internal storage (Steam folder
   `STAR WARS Tie Fighter`):
   - `collectors_cd` (1995 CD version) to `/sdcard/OpenTIE/TIE95`
   - `remastered` (1998 Windows version) to `/sdcard/OpenTIE/TIE98`

   For example:

       adb push ".../STAR WARS Tie Fighter/collectors_cd/." /sdcard/OpenTIE/TIE95/
       adb push ".../STAR WARS Tie Fighter/remastered/." /sdcard/OpenTIE/TIE98/

3. Start "OpenTIE VR" from Unknown Sources. On first start, allow
   "access to all files" when Android asks (or run
   `adb shell appops set --uid com.bilko.opentievr MANAGE_EXTERNAL_STORAGE allow`).

## Controls

Recenter: hold the Meta button on the right controller, or hold the left
Menu button for one second.

Menus (and in-flight screens such as the map and options):

| Input | Action |
|---|---|
| Laser (either controller) + trigger | Click |
| Right grip | Right-click |
| Left stick | Arrow keys, one step per push |
| A | Enter |
| B | Escape (Delete while the keyboard is open) |
| Y | Space |
| X | Open/close the on-screen keyboard (pilot names) |
| Left Menu, short press | Escape |

Flight (the Touch controllers act as one gamepad; every button can be
remapped in Settings > Controller):

| Input | Action |
|---|---|
| Left stick | Pitch and yaw |
| Right stick left/right | Roll |
| Right stick up/down | Throttle lever (stays where you leave it) |
| Right trigger | Fire |
| Left trigger | Target / roll-throttle modifier |
| Right grip | High-angle view |
| A / B | Next target / nearest fighter |
| X / Y | Target my attacker / match target speed |
| Left stick click | Back to cockpit view |
| Right stick click | Hyperspace |
| Left Menu | Pause |
| Left grip + A / B | Full throttle / throttle down |
| Left grip + X / Y | Clear target / Escape (flight menu) |
| Left grip + left trigger | Cycle firing mode |
| Left grip + left stick click | Cycle weapon (lasers / warheads) |
| Left grip + right stick click | Cycle shield direction |

## Known limitations (beta)

- Quest 3 tested only.
- Only the TIE Fighter has the 3D cockpit; other craft use the flat
  cockpit picture in front of the stereo view.
- Motion blur, temporal upscaling and ambient occlusion are off in stereo.
- Signed with a debug key.

## Build (Linux host)

Requirements: Android SDK + NDK 27.2.12479018, JDK 17, SDL_shadercross,
CMake, Ninja, Python 3.

    git submodule update --init --recursive
    tools/quest/build-deps.sh      # SDL3, zstd, FFmpeg, host shader tools
    tools/quest/build-native.sh    # libmain.so + runtime resources
    (cd android && ./gradlew assembleDebug)
    tools/quest/deploy.sh          # install, copy game data once, launch

VR code: `src/tie_vr/` (OpenXR session, input, layers; `tie_vr.c` is
generated from OpenXWA-VR's `xwa_vr.c` by `tools/quest/gen_tie_vr.py`),
`src/tie_remaster/vr_stereo.*` and `flight/passes.c` (per-eye rendering),
`tools/quest/gen_vr_cockpit.py` (3D cockpit model), `src/tie_app/vr_keyboard.*`
(on-screen keyboard) and the `quest-vr-tie` branch of the
[Aeron engine fork](https://github.com/SgtBilko76/Aeron/tree/quest-vr-tie).
