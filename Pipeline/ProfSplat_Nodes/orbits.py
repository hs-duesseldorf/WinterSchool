"""Orbit presets for MiniMax H3 (prompts, lengths, pinned frames, grey keyframe positions)."""

_FROZEN = (
    "One frozen instant. Only the camera moves. Preserve every person and object in exactly the same world position, "
    "orientation, shape and pose throughout the shot: no wobbling, shaking, drifting or continued action. Keep faces, "
    "hands, clothing and the background motionless while retaining their natural appearance. Camera parallax is the "
    "only source of apparent movement. No cuts, zoom, morphing or added objects. ")

PROMPTS = {
    # two circles in one 10 s generation: a full 360 at eye level, then a 360 that climbs to a top-down view behind
    # the person and comes back down. The hero photo is pinned again where the first circle ends (PINS)
    'climb': _FROZEN + (
        "The camera circles the person two full times without stopping, always facing the person. Seconds 0 to 5: the "
        "first full 360 degree circle stays level at eye height the whole way round and ends in front of the person. "
        "Seconds 5 to 10: the second full circle rises steadily to a steep top-down bird's-eye view onto the top of the "
        "head behind the person, then lowers back down to eye level and ends in front of the person."),
    # 5 s, eye level only
    'level': (
        "One frozen instant. Only the camera moves. In a continuous 360 orbit. Preserve every person and object in "
        "exactly the same world position, orientation, shape and pose throughout the shot. Airborne objects remain "
        "suspended at the captured height and angle: no wobbling, shaking, spinning, drifting, falling or continued "
        "action. Keep faces, hands, clothing, liquids and the background motionless while retaining their natural "
        "appearance. Camera parallax is the only source of apparent movement. No cuts, zoom, morphing or added objects."),
    # 5 s, one orbit that rises over the head halfway round
    'wave': _FROZEN + (
        "Spiral crane shot: the camera orbits 360 degrees around the person and moves up and down like a wave, rising "
        "from eye level to a steep top-down bird's-eye view above the head halfway around, then lowering back to eye "
        "level at the end."),
    # 15 s: eye-level ring, then an overhead ring (asking for three makes H3 do the first two cleanly)
    'rings': _FROZEN + (
        "The camera circles the person three full times without stopping, always facing the person: the first full "
        "circle at eye level; the second full circle rises high above, a steep top-down bird's-eye view onto the top "
        "of the head; the third full circle goes down low near the floor, looking up at the person. Each circle starts "
        "and ends at eye level in front of the person."),
}
# H3 swaps the photo's studio for a black void with a spotlight once the camera leaves the front; ask for the
# photo's backdrop all round (the grey keyframes are what actually makes it stick)
BACKDROP = (" The whole scene is inside an infinite, seamless, uniform light-gray photo studio that surrounds the person "
            "on all sides: in every direction, also behind the person and seen from above, the background and the floor "
            "stay the same plain light gray as in the first frame, evenly lit, with no dark background, no black, no "
            "spotlight, no vignette and no light falloff. The lighting on the person stays soft, even and unchanged "
            "from every side.")
LENGTHS = {'climb': 243, 'rings': 362}  # frames (17k+5 grid; 124 = 5 s, 243 = 10 s, 362 = 15 s)
PINS = {'climb': [121]}                 # frames where the hero is pinned again (camera back in front)
KEYS = {'climb': [31, 61, 91, 151, 181, 211], 'rings': [31, 61, 91, 151, 181, 211, 271, 301, 331]}
KEYS_DEFAULT = [31, 62, 93]             # 5 s orbits: sides and back
