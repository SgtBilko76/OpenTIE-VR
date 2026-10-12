#!/usr/bin/env python3
"""Swap in the Quest Touch controller defaults when staging the APK runtime.

OpenTIE reads its shipped gamepad defaults from resources/config.yaml
(`input.gamepad_defaults`). The Quest build exposes both Touch controllers as
one virtual gamepad (src/tie_vr/tie_vr.c, vr_update_gamepad):

  right stick      -> rightx / righty      left stick x -> leftx
  left stick y     -> lefty (throttle lever, stays where it is left)
  triggers         -> lefttrigger / righttrigger
  A / B / X / Y    -> south / east / west / north
  right grip       -> rightshoulder
  stick clicks     -> leftstick / rightstick
  left Menu (tap)  -> start
  left grip held (shift layer): A/B/X/Y -> dpaddown/dpadright/dpadleft/dpadup,
                   right stick click -> back, left stick click -> guide,
                   left trigger -> leftshoulder

Usage: quest_config.py <staged resources/config.yaml>
Only the Quest APK is affected; desktop builds keep the upstream defaults.
"""
import re
import sys

QUEST_GAMEPAD_DEFAULTS = """\
  gamepad_defaults:
    axes:
      yaw: {source: rightx, invert: false, deadzone: 0.08}
      pitch: {source: righty, invert: false, deadzone: 0.08}
      roll: {source: leftx, invert: false, deadzone: 0.08}
      throttle: {source: lefty, invert: false}
    buttons:
      fire_weapon:
        - {axis: righttrigger, direction: positive, threshold: 0.5}
      target_roll_throttle_modifier:
        - {axis: lefttrigger, direction: positive, threshold: 0.5}
      target_next: south
      target_nearest_fighter_or_mine: east
      target_my_attacker: west
      match_speed: north
      view_toggle_high_angle: rightshoulder
      view_return_to_cockpit: rightstick
      hyperspace: leftstick
      pause: start
      throttle_full: dpaddown
      throttle_down: dpadright
      target_clear: dpadleft
      escape: dpadup
      cycle_weapon_group: back
      cycle_shield_mode: guide
      cycle_weapon_firing_mode: leftshoulder
"""


def main() -> int:
    path = sys.argv[1]
    text = open(path, encoding="utf-8").read()
    # The block runs from "  gamepad_defaults:" to the next key at the same
    # indentation (two spaces).
    pattern = re.compile(r"^  gamepad_defaults:\n(?:^(?:    .*)?\n)*", re.M)
    if not pattern.search(text):
        print("quest_config: input.gamepad_defaults not found", file=sys.stderr)
        return 1
    text = pattern.sub(QUEST_GAMEPAD_DEFAULTS, text, count=1)
    open(path, "w", encoding="utf-8").write(text)
    print(f"quest_config: Touch controller defaults written to {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
