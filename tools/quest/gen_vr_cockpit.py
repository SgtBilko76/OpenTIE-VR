#!/usr/bin/env python3
"""Generate the Quest VR 3D TIE Fighter cockpit.

The original game draws its cockpit as a flat 640x456 picture
(CP640/TIEFTR12.LFD). In VR the 3D frame (window octagon, struts, hull ball,
instrument housings) is this generated model, and the live instruments are
cut out of the 2D cockpit overlay and shown on quad layers mounted on the
housings.

Outputs (committed, regenerate after editing this file):
  src/tie_remaster/vr_cockpit_model.inc  cooked GLB as a C byte array
  src/tie_vr/tie_vr_cockpit_panels.h     panel table for the OpenXR quads

Coordinates here: OpenXR LOCAL, metres, seat eye at the origin,
x right, y up, -z forward.

Usage: gen_vr_cockpit.py <aeron_gltf_cook executable>
"""
import json
import math
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# ---------------------------------------------------------------- materials
MATERIALS = [
    # name, base rgb, metallic, roughness, emissive rgb
    ("frame", (0.16, 0.17, 0.19), 0.7, 0.45, (0.010, 0.010, 0.012)),
    ("hull", (0.085, 0.088, 0.095), 0.4, 0.7, (0.006, 0.006, 0.007)),
    ("rib", (0.13, 0.135, 0.145), 0.6, 0.5, (0.008, 0.008, 0.009)),
    ("screen_black", (0.004, 0.004, 0.004), 0.0, 0.35, (0.0, 0.0, 0.0)),
    ("screen_navy", (0.004, 0.010, 0.045), 0.0, 0.3, (0.002, 0.006, 0.030)),
    ("red_trim", (0.45, 0.03, 0.02), 0.2, 0.5, (0.18, 0.010, 0.006)),
    ("housing", (0.11, 0.115, 0.125), 0.6, 0.55, (0.008, 0.008, 0.009)),
]
MAT = {m[0]: i for i, m in enumerate(MATERIALS)}

# ---------------------------------------------------------------- geometry
WINDOW_TILT_Y = -0.03  # window axis (tangent) slightly below eye level
OCTAGON_TAN = 0.66  # inner octagon circumradius (tangent ~33 deg)
RING_TAN = 1.05  # outer window ring (tangent ~46 deg)
FRAME_DIST = 0.80  # window frame distance from the eye (m)
HULL_R = 0.92  # hull ball radius (m)


def v_add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def v_sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def v_mul(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def v_dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def v_cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def v_norm(a):
    length = math.sqrt(v_dot(a, a))
    return v_mul(a, 1.0 / length) if length > 0 else a


def tan_dir(tx, ty):
    """Direction for tangent coordinates (x right, y up)."""
    return v_norm((tx, ty, -1.0))


class Mesh:
    def __init__(self):
        self.prims = {}  # material -> (positions, normals, indices)

    def tri(self, mat, a, b, c, n=None):
        if n is None:
            n = v_norm(v_cross(v_sub(b, a), v_sub(c, a)))
        pos, nrm, idx = self.prims.setdefault(mat, ([], [], []))
        base = len(pos)
        pos.extend([a, b, c])
        nrm.extend([n, n, n])
        idx.extend([base, base + 1, base + 2])

    def quad(self, mat, a, b, c, d, n=None):
        """a-b-c-d counter-clockwise seen from the front."""
        if n is None:
            n = v_norm(v_cross(v_sub(b, a), v_sub(d, a)))
        self.tri(mat, a, b, c, n)
        self.tri(mat, a, c, d, n)

    def smooth_quad(self, mat, pts, nrms):
        pos, nrm, idx = self.prims.setdefault(mat, ([], [], []))
        base = len(pos)
        pos.extend(pts)
        nrm.extend(nrms)
        idx.extend([base, base + 1, base + 2, base, base + 2, base + 3])

    def beam(self, mat, p0, p1, width, depth, up_hint=None):
        """Box beam from p0 to p1; `depth` runs along the view direction."""
        axis = v_norm(v_sub(p1, p0))
        mid = v_mul(v_add(p0, p1), 0.5)
        view = v_norm(mid)  # from the eye outward
        side = v_cross(axis, view)
        if v_dot(side, side) < 1e-8:
            side = v_cross(axis, up_hint or (0.0, 1.0, 0.0))
        side = v_norm(side)
        out = v_norm(v_cross(side, axis))
        if v_dot(out, view) < 0:
            out = v_mul(out, -1.0)
        hw, hd = width * 0.5, depth * 0.5
        corners = []
        for s, o in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            off = v_add(v_mul(side, s * hw), v_mul(out, o * hd))
            corners.append((v_add(p0, off), v_add(p1, off)))
        for i in range(4):
            a0, a1 = corners[i]
            b0, b1 = corners[(i + 1) % 4]
            n = v_norm(v_sub(v_mul(v_add(v_add(a0, b0), v_add(a1, b1)), 0.25), mid))
            # orient outward from the beam axis
            self.quad(mat, a0, b0, b1, a1, n)

    def box(self, mat, center, right, up, fwd, hx, hy, hz):
        """Oriented box. right/up/fwd unit axes; fwd points toward the eye."""
        def c(sx, sy, sz):
            return v_add(center, v_add(v_mul(right, sx * hx), v_add(v_mul(up, sy * hy), v_mul(fwd, sz * hz))))
        faces = [
            ((1, 0, 0), [(1, -1, -1), (1, 1, -1), (1, 1, 1), (1, -1, 1)]),
            ((-1, 0, 0), [(-1, -1, 1), (-1, 1, 1), (-1, 1, -1), (-1, -1, -1)]),
            ((0, 1, 0), [(-1, 1, -1), (-1, 1, 1), (1, 1, 1), (1, 1, -1)]),
            ((0, -1, 0), [(-1, -1, 1), (-1, -1, -1), (1, -1, -1), (1, -1, 1)]),
            ((0, 0, 1), [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]),
            ((0, 0, -1), [(1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1)]),
        ]
        for (nx, ny, nz), pts in faces:
            n = v_add(v_mul(right, nx), v_add(v_mul(up, ny), v_mul(fwd, nz)))
            q = [c(*p) for p in pts]
            self.quad(mat, q[0], q[1], q[2], q[3], n)

    def disc(self, mat, center, right, up, fwd, radius, segments=40):
        for i in range(segments):
            a0 = 2 * math.pi * i / segments
            a1 = 2 * math.pi * (i + 1) / segments
            p0 = v_add(center, v_add(v_mul(right, math.cos(a0) * radius), v_mul(up, math.sin(a0) * radius)))
            p1 = v_add(center, v_add(v_mul(right, math.cos(a1) * radius), v_mul(up, math.sin(a1) * radius)))
            self.tri(mat, center, p0, p1, fwd)

    def ring(self, mat, center, right, up, fwd, r0, r1, segments=40):
        for i in range(segments):
            a0 = 2 * math.pi * i / segments
            a1 = 2 * math.pi * (i + 1) / segments
            d0 = v_add(v_mul(right, math.cos(a0)), v_mul(up, math.sin(a0)))
            d1 = v_add(v_mul(right, math.cos(a1)), v_mul(up, math.sin(a1)))
            self.quad(mat, v_add(center, v_mul(d0, r0)), v_add(center, v_mul(d0, r1)),
                      v_add(center, v_mul(d1, r1)), v_add(center, v_mul(d1, r0)), fwd)


def window_axis():
    return tan_dir(0.0, WINDOW_TILT_Y)


def window_point(angle, tan_r, dist):
    """Point at polar angle (0 = right, ccw) and tangent radius around the window axis."""
    tx = math.cos(angle) * tan_r
    ty = WINDOW_TILT_Y + math.sin(angle) * tan_r
    return v_mul(tan_dir(tx, ty), dist)


def build_frame(m):
    # Inner octagon (vertex at the top) and eight radial struts.
    verts = [math.pi / 2 + k * math.pi / 4 for k in range(8)]
    inner = [window_point(a, OCTAGON_TAN, FRAME_DIST) for a in verts]
    for k in range(8):
        m.beam("frame", inner[k], inner[(k + 1) % 8], 0.045, 0.06)
    # Corner blocks at the octagon vertices.
    for p in inner:
        d = v_norm(p)
        right = v_norm(v_cross((0.0, 1.0, 0.0), d)) if abs(d[1]) < 0.99 else (1.0, 0.0, 0.0)
        up = v_cross(d, right)
        m.box("rib", p, right, up, v_mul(d, -1.0), 0.035, 0.035, 0.04)
    # Radial struts out to the ring (they curve with the ball).
    steps = 6
    for a in verts:
        prev = None
        for s in range(steps + 1):
            t = s / steps
            tan_r = OCTAGON_TAN + (RING_TAN * 1.25 - OCTAGON_TAN) * t
            dist = FRAME_DIST + (HULL_R - 0.02 - FRAME_DIST) * t
            p = window_point(a, tan_r, dist)
            if prev is not None:
                m.beam("frame", prev, p, 0.05, 0.07)
            prev = p
    # Outer window ring.
    seg = 48
    ring_pts = [window_point(2 * math.pi * i / seg, RING_TAN, HULL_R * 0.97) for i in range(seg)]
    for i in range(seg):
        m.beam("rib", ring_pts[i], ring_pts[(i + 1) % seg], 0.07, 0.05)


def build_hull(m):
    """Ball interior outside the window cone (seen from inside)."""
    axis = window_axis()
    cos_cut = math.cos(math.atan(RING_TAN))
    lat_n, lon_n = 36, 72
    for i in range(lat_n):
        th0 = math.pi * i / lat_n
        th1 = math.pi * (i + 1) / lat_n
        for j in range(lon_n):
            ph0 = 2 * math.pi * j / lon_n
            ph1 = 2 * math.pi * (j + 1) / lon_n

            def sp(th, ph):
                return (math.sin(th) * math.cos(ph), math.cos(th), math.sin(th) * math.sin(ph))

            ds = [sp(th0, ph0), sp(th1, ph0), sp(th1, ph1), sp(th0, ph1)]
            if all(v_dot(d, axis) > cos_cut for d in ds):
                continue
            pts = [v_mul(d, HULL_R) for d in ds]
            nrms = [v_mul(d, -1.0) for d in ds]  # facing inward
            m.smooth_quad("hull", pts, nrms)
    # Ribs: meridians on the ball (every 30 deg), skipped inside the window.
    for k in range(12):
        ph = 2 * math.pi * k / 12
        prev = None
        for i in range(1, 36):
            th = math.pi * i / 36
            d = (math.sin(th) * math.cos(ph), math.cos(th), math.sin(th) * math.sin(ph))
            p = v_mul(d, HULL_R - 0.015)
            if v_dot(d, axis) > math.cos(math.atan(RING_TAN * 1.15)):
                prev = None
                continue
            if prev is not None:
                m.beam("rib", prev, p, 0.04, 0.03)
            prev = p


# ---------------------------------------------------------------- panels
# Classic source rects (x0, y0, x1, y1) in the 640x480 cockpit frame, measured
# from CP640/TIEFTR12.LFD; mount direction as tangents (x right, y up);
# distance and width in metres. Height follows the source aspect.
PANELS = [
    # name,      src rect,               tan x, tan y, dist, width, face
    # Kept within about +-30 deg vertically so everything reads without
    # tilting the head (the radars and CMD sat at the edge of view before).
    ("radar_l", (4, 0, 110, 102), -0.78, 0.40, 0.72, 0.20, "radar"),
    ("radar_r", (530, 0, 638, 104), 0.78, 0.40, 0.72, 0.20, "radar"),
    ("gauge_l", (2, 198, 30, 302), -1.10, -0.02, 0.62, 0.05, "gauge"),
    ("gauge_r", (610, 198, 638, 302), 1.10, -0.02, 0.62, 0.05, "gauge"),
    ("console", (180, 272, 460, 456), 0.0, -0.44, 0.58, 0.34, "console"),
    ("readouts", (546, 300, 640, 448), 0.70, -0.46, 0.58, 0.11, "readouts"),
    ("threat", (2, 350, 114, 452), -0.70, -0.46, 0.58, 0.13, "threat"),
    ("messages", (0, 456, 640, 480), 0.0, -0.20, 0.95, 0.80, "strip"),
]
# The HUD (crosshair, target box) stays world-aligned on the far overlay
# quad; only this classic region (the window, clear of all panels) is shown
# there.
HUD_RECT = (112, 0, 528, 270)

# The other craft (used with X-Wing Alliance's cockpit models, no housings in
# the generated model): instrument rects measured by hand on the
# CP640/<view>.LFD pictures; placed automatically by auto_place().
MESSAGES = ("messages", (0, 456, 640, 480), 0.0, -0.20, 0.95, 0.80, "strip")
OTHER_VIEWS = {
    "TIEINT12": ((112, 0, 528, 276), [
        ("radar_l", (4, 0, 112, 102)), ("radar_r", (528, 0, 636, 102)),
        ("gauge_l", (0, 145, 30, 345)), ("gauge_r", (606, 145, 636, 345)),
        ("console", (190, 277, 450, 456)), ("threat", (0, 340, 110, 440)),
        ("readouts", (535, 350, 636, 452)), ("readout_l", (85, 414, 182, 452)),
        ("readout_r", (457, 414, 553, 452))]),
    "TIEADV12": ((112, 62, 528, 300), [
        ("radar_l", (2, 0, 110, 102)), ("radar_r", (530, 0, 640, 104)),
        ("topbar", (157, 0, 478, 60)), ("gauge_l", (0, 357, 40, 456)),
        ("threat", (42, 360, 140, 452)), ("console", (140, 302, 500, 456)),
        ("readouts", (500, 360, 600, 452)), ("gauge_r", (600, 357, 640, 456))]),
    "TIEDLX12": ((112, 55, 528, 300), [
        ("radar_l", (2, 0, 112, 102)), ("radar_r", (528, 0, 640, 104)),
        ("topbar", (157, 0, 483, 53)), ("readout_l", (0, 295, 63, 345)),
        ("gauge_l", (0, 347, 40, 456)), ("threat", (42, 360, 138, 452)),
        ("console", (145, 302, 493, 456)), ("readouts", (500, 360, 598, 452)),
        ("readout_r", (575, 275, 633, 345)), ("gauge_r", (600, 347, 640, 456))]),
    "TIEBMB12": ((112, 0, 528, 274), [
        ("radar_l", (4, 0, 112, 104)), ("radar_r", (530, 0, 640, 106)),
        ("gauge_l", (0, 135, 32, 345)), ("gauge_r", (600, 125, 636, 340)),
        ("console", (217, 275, 425, 456)), ("threat", (0, 365, 100, 456)),
        ("readout_l", (88, 420, 185, 452)), ("readout_r", (452, 420, 553, 452)),
        ("readouts", (532, 340, 636, 425))]),
    "GUNBT12": ((112, 0, 528, 244), [
        ("radar_l", (0, 0, 112, 112)), ("radar_r", (530, 0, 640, 112)),
        ("console", (112, 245, 528, 456)), ("threat", (10, 360, 103, 450)),
        ("readouts", (545, 350, 633, 440))]),
    "MISSBT12": ((112, 0, 528, 258), [
        ("radar_l", (2, 8, 110, 110)), ("radar_r", (532, 10, 640, 112)),
        ("console", (125, 260, 515, 456)), ("threat", (38, 320, 125, 412)),
        ("readouts", (512, 320, 600, 412))]),
}


def auto_place(name, rect):
    """Mount a panel where it sat on the original screen, spread out a little
    so it clears the window, at arm's length and readable size."""
    cx, cy = (rect[0] + rect[2]) * 0.5, (rect[1] + rect[3]) * 0.5
    # Original projection of the 4:3 classic frame (game camera tangents).
    tx = (cx - 320.0) / 320.0 * 0.625 * 1.35
    ty = (0.367 - cy / 480.0 * 0.937) * 1.2
    ty = max(-0.62, min(0.5, ty))
    dist = 0.72 if name.startswith("radar") or name == "topbar" else 0.62
    width = dist * (rect[2] - rect[0]) / 320.0 * 0.625 * 1.3
    return (name, rect, tx, ty, dist, width, "auto")


def panel_frame(tx, ty, dist):
    d = tan_dir(tx, ty)
    center = v_mul(d, dist)
    fwd = v_mul(d, -1.0)  # toward the eye
    yaw = math.atan2(-d[0], -d[2])
    pitch = math.asin(max(-1.0, min(1.0, d[1])))
    right = (math.cos(yaw), 0.0, -math.sin(yaw))
    up = v_norm(v_cross(fwd, right))
    return center, right, up, fwd, yaw, pitch


def build_panels(m, panels=PANELS):
    out = []
    for name, rect, tx, ty, dist, width, face in panels:
        center, right, up, fwd, yaw, pitch = panel_frame(tx, ty, dist)
        height = width * (rect[3] - rect[1]) / (rect[2] - rect[0])
        hx, hy = width * 0.5, height * 0.5
        margin = 0.012 if face != "strip" else 0.006
        depth = 0.025
        # housing slab, its front face 4 mm behind the instrument quad
        back = v_add(center, v_mul(fwd, -(0.004 + depth)))
        if m is None:
            pass
        elif face != "strip":
            m.box("housing", back, right, up, fwd, hx + margin, hy + margin, depth)
            # support arm to the hull
            arm_end = v_mul(v_norm(center), HULL_R - 0.01)
            m.beam("rib", v_add(center, v_mul(fwd, -0.03)), arm_end, 0.03, 0.03)
        face_c = v_add(center, v_mul(fwd, -0.0035))
        if m is None:
            pass
        elif face == "radar":
            r = min(hx, hy)
            m.disc("screen_black", face_c, right, up, fwd, r * 0.95)
            m.ring("red_trim", v_add(face_c, v_mul(fwd, 0.0005)), right, up, fwd, r * 0.92, r * 1.0)
        elif face == "console":
            m.quad("screen_navy", *[v_add(face_c, v_add(v_mul(right, sx * hx * 0.93), v_mul(up, sy * hy * 0.95)))
                                     for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1))], n=fwd)
        elif face == "strip":
            m.quad("screen_black", *[v_add(face_c, v_add(v_mul(right, sx * hx), v_mul(up, sy * hy)))
                                      for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1))], n=fwd)
        else:
            m.quad("screen_black", *[v_add(face_c, v_add(v_mul(right, sx * hx), v_mul(up, sy * hy)))
                                      for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1))], n=fwd)
            # red trim border
            t = 0.004
            for (ax, ay, bx, by) in ((-1, -1, 1, -1), (1, -1, 1, 1), (1, 1, -1, 1), (-1, 1, -1, -1)):
                p0 = v_add(face_c, v_add(v_mul(right, ax * (hx + t)), v_mul(up, ay * (hy + t))))
                p1 = v_add(face_c, v_add(v_mul(right, bx * (hx + t)), v_mul(up, by * (hy + t))))
                m.beam("red_trim", v_add(p0, v_mul(fwd, 0.001)), v_add(p1, v_mul(fwd, 0.001)), t, 0.002)
        half_yaw, half_pitch = yaw * 0.5, pitch * 0.5
        # q = q_yaw(y axis) * q_pitch(x axis), (x, y, z, w)
        cy, sy_ = math.cos(half_yaw), math.sin(half_yaw)
        cp, sp_ = math.cos(half_pitch), math.sin(half_pitch)
        q = (cy * sp_, sy_ * cp, -sy_ * sp_, cy * cp)
        out.append((name, rect, center, q, width, height))
    return out


# ---------------------------------------------------------------- glTF
def to_gltf(p):
    # OpenXR (x right, y up, -z fwd) -> game eye (X right, Y down, Z fwd)
    # -> glTF such that Aeron's gltf_to_aeron (-gx, -gz, gy) yields game axes.
    X, Y, Z = p[0], -p[1], -p[2]
    return (-X, Z, -Y)


def write_gltf(m, path_gltf):
    bin_data = bytearray()
    buffer_views, accessors, primitives = [], [], []
    for mat_name, (pos, nrm, idx) in m.prims.items():
        gpos = [to_gltf(p) for p in pos]
        gnrm = [v_norm(to_gltf(n)) for n in nrm]
        # Handedness flips with this axis map; reverse winding to keep fronts.
        idx2 = []
        for i in range(0, len(idx), 3):
            idx2 += [idx[i], idx[i + 2], idx[i + 1]]
        for data, kind in ((gpos, "pos"), (gnrm, "nrm")):
            off = len(bin_data)
            for v in data:
                bin_data += struct.pack("<3f", *v)
            buffer_views.append({"buffer": 0, "byteOffset": off, "byteLength": len(bin_data) - off, "target": 34962})
            acc = {"bufferView": len(buffer_views) - 1, "componentType": 5126, "count": len(data), "type": "VEC3"}
            if kind == "pos":
                acc["min"] = [min(v[i] for v in data) for i in range(3)]
                acc["max"] = [max(v[i] for v in data) for i in range(3)]
            accessors.append(acc)
        off = len(bin_data)
        for i in idx2:
            bin_data += struct.pack("<H", i)
        while len(bin_data) % 4:
            bin_data += b"\0"
        buffer_views.append({"buffer": 0, "byteOffset": off, "byteLength": len(idx2) * 2, "target": 34963})
        accessors.append({"bufferView": len(buffer_views) - 1, "componentType": 5123, "count": len(idx2),
                          "type": "SCALAR"})
        n = len(accessors)
        primitives.append({"attributes": {"POSITION": n - 3, "NORMAL": n - 2}, "indices": n - 1,
                           "material": MAT[mat_name], "mode": 4})
        if len(pos) > 60000:
            raise SystemExit(f"primitive {mat_name} has too many vertices")
    materials = []
    for name, rgb, metal, rough, emis in MATERIALS:
        materials.append({
            "name": name,
            "pbrMetallicRoughness": {"baseColorFactor": [*rgb, 1.0], "metallicFactor": metal,
                                     "roughnessFactor": rough},
            "emissiveFactor": list(emis),
            "doubleSided": True,
        })
    gltf = {
        "asset": {"version": "2.0", "generator": "OpenTIE gen_vr_cockpit.py"},
        "extensionsUsed": ["AERON_flight_model"],
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [
            {"name": "tie_vr_cockpit", "children": [1],
             "extensions": {"AERON_flight_model": {"role": "model"}}},
            {"name": "cockpit", "mesh": 0,
             "extensions": {"AERON_flight_model": {"role": "component", "meshType": 0}}},
        ],
        "meshes": [{"name": "cockpit", "primitives": primitives}],
        "materials": materials,
        "buffers": [{"uri": os.path.basename(path_gltf)[:-5] + ".bin", "byteLength": len(bin_data)}],
        "bufferViews": buffer_views,
        "accessors": accessors,
    }
    with open(path_gltf, "w") as f:
        json.dump(gltf, f)
    with open(path_gltf[:-5] + ".bin", "wb") as f:
        f.write(bin_data)
    return sum(len(p[0]) for p in m.prims.values()), sum(len(p[2]) for p in m.prims.values()) // 3


def write_c_array(glb_path, out_path):
    data = open(glb_path, "rb").read()
    lines = ["/* Generated by tools/quest/gen_vr_cockpit.py - do not edit. */",
             f"static const unsigned int tie_vr_cockpit_glb_size = {len(data)}u;",
             "static const unsigned char tie_vr_cockpit_glb[] = {"]
    for i in range(0, len(data), 20):
        lines.append("\t" + ", ".join(str(b) for b in data[i:i + 20]) + ",")
    lines.append("};")
    open(out_path, "w").write("\n".join(lines) + "\n")
    return len(data)


def write_panels(views, out_path):
    lines = [
        "/* Generated by tools/quest/gen_vr_cockpit.py - do not edit.",
        " * VR cockpit instrument panels per craft view: classic 640x480 source",
        " * rect, pose in the seat (OpenXR LOCAL) space and size in metres. */",
        "#ifndef TIE_VR_COCKPIT_PANELS_H",
        "#define TIE_VR_COCKPIT_PANELS_H",
        "",
        "#include <stddef.h>",
        "#include <string.h>",
        "",
        "typedef struct TieVrCockpitPanel {",
        "\tconst char* name;",
        "\tfloat src[4];         /* x0, y0, x1, y1 in classic 640x480 pixels */",
        "\tfloat position[3];    /* metres, x right, y up, -z forward */",
        "\tfloat orientation[4]; /* x, y, z, w */",
        "\tfloat size[2];        /* width, height in metres */",
        "} TieVrCockpitPanel;",
        "",
        "typedef struct TieVrCockpitView {",
        "\tconst char*              view;         /* CP640 LFD basename */",
        "\tconst char*              xwa_model;    /* X-Wing Alliance cockpit OPT */",
        "\tint                      has_builtin;  /* generated cockpit fits it */",
        "\tfloat                    hud_rect[4];  /* window region, classic px */",
        "\tconst TieVrCockpitPanel* panels;",
        "\tsize_t                   panel_count;",
        "} TieVrCockpitView;",
        "",
    ]
    table = []
    for view, model, builtin, hud, panels in views:
        ident = "TIE_VR_PANELS_" + view
        lines.append(f"static const TieVrCockpitPanel {ident}[] = {{")
        for name, rect, c, q, w, h in panels:
            lines.append(
                f'\t{{ "{name}", {{ {rect[0]}.0f, {rect[1]}.0f, {rect[2]}.0f, {rect[3]}.0f }}, '
                f"{{ {c[0]:.5f}f, {c[1]:.5f}f, {c[2]:.5f}f }}, "
                f"{{ {q[0]:.6f}f, {q[1]:.6f}f, {q[2]:.6f}f, {q[3]:.6f}f }}, {{ {w:.5f}f, {h:.5f}f }} }},")
        lines += ["};", ""]
        table.append(f'\t{{ "{view}", "{model}", {builtin}, {{ {hud[0]}.0f, {hud[1]}.0f, {hud[2]}.0f, {hud[3]}.0f }}, '
                     f"{ident}, sizeof {ident} / sizeof {ident}[0] }},")
    lines.append("static const TieVrCockpitView TIE_VR_COCKPIT_VIEWS[] = {")
    lines += table
    lines += [
        "};",
        "",
        "/* The 3D cockpit entry for a cockpit view name, or NULL. */",
        "static inline const TieVrCockpitView* TieVrCockpit_FindView(const char* view_name) {",
        "\tif (!view_name)",
        "\t\treturn NULL;",
        "\tfor (size_t i = 0; i < sizeof TIE_VR_COCKPIT_VIEWS / sizeof TIE_VR_COCKPIT_VIEWS[0]; ++i)",
        "\t\tif (strncmp(view_name, TIE_VR_COCKPIT_VIEWS[i].view, 9) == 0)",
        "\t\t\treturn &TIE_VR_COCKPIT_VIEWS[i];",
        "\treturn NULL;",
        "}",
        "",
        "#endif",
        "",
    ]
    open(out_path, "w").write("\n".join(lines))


def main():
    cook = sys.argv[1]
    m = Mesh()
    build_frame(m)
    build_hull(m)
    panels = build_panels(m)
    views = [("TIEFTR12", "TIECOCKPIT.OPT", 1, HUD_RECT, panels)]
    for view, (hud, rects) in OTHER_VIEWS.items():
        messages = MESSAGES
        if view.startswith(("GUNBT", "MISSBT")):
            # The boats' wide console fills the lower view: messages go on top.
            messages = MESSAGES[:3] + (0.42,) + MESSAGES[4:]
        placed = [auto_place(name, rect) for name, rect in rects] + [messages]
        model = "TIEBOMBERCOCKPIT.OPT" if view.startswith("TIEBMB") else "TIECOCKPIT.OPT"
        views.append((view, model, 0, hud, build_panels(None, placed)))
    work = tempfile.mkdtemp(prefix="tie_vr_cockpit_")
    gltf = os.path.join(work, "tie_vr_cockpit.gltf")
    glb = os.path.join(work, "tie_vr_cockpit.glb")
    nv, nt = write_gltf(m, gltf)
    subprocess.run([cook, gltf, "--out", glb, "--no-zstd", "--rgba8"], check=True)
    size = write_c_array(glb, os.path.join(ROOT, "src/tie_remaster/vr_cockpit_model.inc"))
    write_panels(views, os.path.join(ROOT, "src/tie_vr/tie_vr_cockpit_panels.h"))
    print(f"cockpit: {nv} vertices, {nt} triangles, GLB {size} bytes, {len(views)} cockpit views")


if __name__ == "__main__":
    main()
