#!/usr/bin/env python3
"""Checks the controller bindings in Source/platform/vr/xr_input.cpp without a headset.

One path a profile does not have makes the runtime reject every binding suggested for that
profile, and then that controller does nothing at all. So: every suggested path must be one of
its profile's components, on a hand that has it, and the Steam Frame profile must bind everything
the game reads. Action names must be unique too, or no action is created.

  tools/steam_frame/tests/check_bindings.py [xr_input.cpp]      exit 1 on a problem
"""
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[3] / "Source/platform/vr/xr_input.cpp"

POSES = {"grip/pose", "aim/pose"}
HAPTIC = {"output/haptic"}
STICK = {"thumbstick", "thumbstick/x", "thumbstick/y", "thumbstick/click", "thumbstick/touch"}

# Component lists from the OpenXR specification and, for the Frame, Valve's documentation.
PROFILES = {
    "/interaction_profiles/khr/simple_controller": {"select/click", "menu/click"} | POSES | HAPTIC,
    "/interaction_profiles/oculus/touch_controller": {
        "x/click", "x/touch", "y/click", "y/touch", "menu/click", "a/click", "a/touch", "b/click", "b/touch",
        "squeeze/value", "trigger/value", "trigger/touch", "thumbrest/touch"} | STICK | POSES | HAPTIC,
    "/interaction_profiles/valve/frame_controller_valve": {
        "a/click", "b/click", "x/click", "y/click", "menu/click", "view/click", "bumper/click", "trigger/value",
        "trigger/click", "squeeze/value", "dpad_up/click", "dpad_down/click", "dpad_left/click",
        "dpad_right/click"} | STICK | POSES | HAPTIC,
}
# Buttons that exist on one hand only.
ONE_HAND = {
    "/interaction_profiles/oculus/touch_controller": {
        "x/click": "left", "x/touch": "left", "y/click": "left", "y/touch": "left", "menu/click": "left",
        "a/click": "right", "a/touch": "right", "b/click": "right", "b/touch": "right"},
    "/interaction_profiles/valve/frame_controller_valve": {
        "a/click": "right", "b/click": "right", "x/click": "right", "y/click": "right", "menu/click": "right",
        "view/click": "left", "dpad_up/click": "left", "dpad_down/click": "left", "dpad_left/click": "left",
        "dpad_right/click": "left"},
}
FRAME = "/interaction_profiles/valve/frame_controller_valve"
FRAME_NEEDS = ["aimAction", "triggerAction", "squeezeAction", "stickAction", "stickClickAction", "aAction", "bAction",
               "xAction", "yAction", "menuAction", "viewAction", "bumperAction", "dpadAction[0]", "dpadAction[1]",
               "dpadAction[2]", "dpadAction[3]", "hapticAction"]


def main():
    text = (Path(sys.argv[1]) if len(sys.argv) > 1 else SRC).read_text()
    problems = []

    names = re.findall(r'CreateAction\("(\w+)"', text)
    for name in sorted(set(names)):
        if names.count(name) > 1:
            problems.append(f"action name {name!r} is used {names.count(name)} times")

    blocks = re.findall(r'SuggestBindings\("([^"]+)",\s*\{(.*?)\n\s*\}\);', text, re.S)
    seen = {}
    for profile, body in blocks:
        if profile not in PROFILES:
            problems.append(f"{profile}: no component list for this profile here, add one")
            continue
        bound = seen.setdefault(profile, set())
        for action, hand, kind, component in re.findall(r'\{\s*([\w\[\]]+),\s*Path\("/user/hand/(\w+)/(input|output)/([\w/]+)"\)\s*\}', body):
            component = component if kind == "input" else "output/" + component
            bound.add(action)
            if component not in PROFILES[profile]:
                problems.append(f"{profile}: {action} is bound to {component}, which the profile does not have")
            wanted = ONE_HAND.get(profile, {}).get(component)
            if wanted is not None and wanted != hand:
                problems.append(f"{profile}: {component} is on the {wanted} hand, not the {hand}")
        print(f"{profile}: {len(bound)} actions bound")

    if FRAME not in seen:
        problems.append("no Steam Frame profile is suggested")
    else:
        for action in FRAME_NEEDS:
            if action not in seen[FRAME]:
                problems.append(f"{FRAME}: {action} is not bound")

    for problem in problems:
        print("BAD ", problem)
    print("bindings ok" if not problems else f"{len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
