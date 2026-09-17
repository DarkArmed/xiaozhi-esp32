#!/usr/bin/env python3
"""Peppa character pack production tool.

Phase-1 pipeline: analyze source GIF/WebP, normalize every asset onto the
unified 256x168 transparent stage canvas, emit light-blue background contact
sheets for frame-by-frame review, and a size/RAM report.

Usage:
    python scripts/character_pack_tools/peppa_pack.py analyze
    python scripts/character_pack_tools/peppa_pack.py build
"""

import argparse
import bisect
import json
import math
import shutil
import struct
from collections import deque
from pathlib import Path

import numpy as np
from PIL import Image, ImageSequence

REPO = Path(__file__).resolve().parents[2]
MEDIA = REPO / "tmp" / "peppa-media"
PACK = REPO / "assets" / "character" / "peppa-v1"

CANVAS_W, CANVAS_H = 256, 168
GROUND_Y = 152            # feet baseline on the stage canvas
STAND_H = 128             # target standing height, single-character actions
FIT_MARGIN = 2            # px margin when a scene is shrunk to fit the stage
TARGET_PERIOD_MS = 70.0   # ~14.3 fps, inside the 12-15 fps target
BG = (0xDC, 0xEF, 0xF8)   # #DCEFF8 light blue
ALPHA_OPAQUE = 128        # >= opaque, < transparent

NEUTRAL_SRC = MEDIA / "neutral" / "peppa-neutral-source.webp"

# Per-action production rules (design doc section 3).
# mode "single": scale = STAND_H / frame-0 character height, ground line at GROUND_Y.
# mode "fit":   whole-scene fit (dual-character / wide splash props), own transform.
ACTIONS = {
    "dance": {
        "src": MEDIA / "round2" / "selected" / "dance.gif",
        "mode": "single",
        "playback": {"mode": "repeat", "plays": 2, "max_ms": 6000},
        "allowed_modes": ["once", "repeat"],
    },
    "walk": {
        "src": MEDIA / "round2" / "selected" / "travel.gif",
        "mode": "single",
        "strip_border": True,
        "playback": {"mode": "repeat", "plays": 2, "max_ms": 4000},
        "allowed_modes": ["once", "repeat"],
    },
    "sing": {
        "src": MEDIA / "round2" / "selected" / "singing.gif",
        "mode": "single",
        "playback": {"mode": "reply", "plays": 0, "max_ms": 8000},
        "allowed_modes": ["reply"],
    },
    "muddy_jump": {
        "src": MEDIA / "round2" / "conditional" / "muddy-puddles-transparent.gif",
        "mode": "fit",  # flying mud spans the full source canvas; keep it unclipped
        "playback": {"mode": "repeat", "plays": 2, "max_ms": 4000},
        "allowed_modes": ["once", "repeat"],
    },
    "play_with_george": {
        "src": MEDIA / "round2" / "interaction" / "bedtime.gif",
        "mode": "fit",  # dual character + bed scene
        "playback": {"mode": "repeat", "plays": 2, "max_ms": 4000},
        "allowed_modes": ["once", "repeat"],
    },
}


def parse_gif_header(path):
    """Minimal parser: canvas size, global color table flag/size, loop extension."""
    with open(path, "rb") as f:
        data = f.read(65536)
    if data[:6] not in (b"GIF87a", b"GIF89a"):
        raise ValueError(f"not a GIF: {path}")
    w, h = struct.unpack("<HH", data[6:10])
    flags = data[10]
    gct_flag = (flags >> 7) & 1
    gct_size = 2 ** ((flags & 0x07) + 1) if gct_flag else 0

    # Walk blocks to find NETSCAPE2.0 app extension and first image descriptor.
    loop = None
    pos = 13 + gct_size * 3
    while pos < len(data):
        b = data[pos]
        if b == 0x21:  # extension
            label = data[pos + 1]
            pos += 2
            if label == 0xFF:  # application extension
                app_len = data[pos]
                app_id = data[pos + 1 : pos + 1 + app_len]
                pos += 1 + app_len
                sub_len = data[pos]
                if app_id.startswith(b"NETSCAPE2.0") and sub_len >= 3:
                    loop = struct.unpack("<H", data[pos + 2 : pos + 4])[0]
                pos += 1
                while data[pos] != 0:  # skip remaining sub-blocks
                    pos += data[pos] + 1
                pos += 1
            else:  # skip other extensions
                pos += 1
                while data[pos] != 0:
                    pos += data[pos] + 1
                pos += 1
        elif b == 0x2C:  # image descriptor
            id_flags = data[pos + 9]
            lct = (id_flags >> 7) & 1
            return {
                "canvas": (w, h),
                "gct": gct_size,
                "first_frame_local_ct": bool(lct),
                "loop": loop,
            }
        elif b == 0x3B:  # trailer
            break
        else:
            break
    return {"canvas": (w, h), "gct": gct_size, "first_frame_local_ct": None, "loop": loop}


def analyze_gif(path):
    info = parse_gif_header(path)
    im = Image.open(path)
    n = getattr(im, "n_frames", 1)
    durations = []
    disposals = []
    bboxes = []
    tiles_full = True
    for i in range(n):
        im.seek(i)
        durations.append(im.info.get("duration", 0))
        disposals.append(getattr(im, "disposal_method", -1))
        frame = im.convert("RGBA")
        bboxes.append(frame.getbbox())  # non-transparent bbox of composited frame
        tile_box = im.tile[0][1] if im.tile else (0, 0, im.width, im.height)
        if (tile_box[2] - tile_box[0], tile_box[3] - tile_box[1]) != (im.width, im.height):
            tiles_full = False
    total_ms = sum(durations)
    delays = sorted(set(durations))
    return {
        "file": str(path.relative_to(REPO)),
        "size_kb": round(path.stat().st_size / 1024, 1),
        **info,
        "frames": n,
        "duration_ms": total_ms,
        "fps_raw": round(n / (total_ms / 1000), 2) if total_ms else None,
        "distinct_delays_ms": delays,
        "disposal_methods": sorted(set(disposals)),
        "all_tiles_full_canvas": tiles_full,
        "bbox_first": bboxes[0],
        "bbox_union_frames": _union_bbox(bboxes),
    }


def _union_bbox(bboxes):
    xs0 = min(b[0] for b in bboxes)
    ys0 = min(b[1] for b in bboxes)
    xs1 = max(b[2] for b in bboxes)
    ys1 = max(b[3] for b in bboxes)
    return (xs0, ys0, xs1, ys1)


def flatten_frames(path):
    """Return list of full-canvas RGBA PIL images + per-frame durations (ms)."""
    im = Image.open(path)
    frames = []
    for frame in ImageSequence.Iterator(im):
        rgba = frame.convert("RGBA")
        frames.append((rgba, frame.info.get("duration", 0)))
    return frames


def strip_white_border(rgba, min_channel=200):
    """Remove sticker-style white outline: flood white-ish pixels that touch
    transparency. Enclosed whites (eye whites) are unreachable and survive."""
    arr = np.array(rgba)
    alpha = arr[:, :, 3]
    h, w = alpha.shape
    rgb_min = arr[:, :, :3].astype(int).min(axis=2)
    whiteish = (alpha > 0) & (rgb_min >= min_channel)
    transparent = alpha == 0
    vis = np.zeros((h, w), bool)
    dq = deque()
    # seed: whiteish pixels adjacent (4-neighbour) to full transparency
    seeds = whiteish & (
        np.roll(transparent, 1, 0) | np.roll(transparent, -1, 0)
        | np.roll(transparent, 1, 1) | np.roll(transparent, -1, 1)
    )
    for y, x in zip(*np.nonzero(seeds)):
        if not vis[y, x]:
            vis[y, x] = True
            dq.append((y, x))
    while dq:
        y, x = dq.popleft()
        for ny, nx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
            if 0 <= ny < h and 0 <= nx < w and whiteish[ny, nx] and not vis[ny, nx]:
                vis[ny, nx] = True
                dq.append((ny, nx))
    arr = arr.copy()
    arr[vis] = (0, 0, 0, 0)
    leftover = int((whiteish & ~vis).sum())
    return Image.fromarray(arr), int(vis.sum()), leftover


def premultiply(img):
    arr = np.array(img).astype(np.float32)
    a = arr[:, :, 3:4] / 255.0
    pm = arr.copy()
    pm[:, :, :3] = arr[:, :, :3] * a
    return pm


def unmultiply(pm):
    arr = pm.copy()
    a = arr[:, :, 3:4]
    with np.errstate(divide="ignore", invalid="ignore"):
        rgb = np.where(a > 0, arr[:, :, :3] * 255.0 / a, 0)
    arr[:, :, :3] = np.clip(rgb, 0, 255)
    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGBA")


def resize_rgba(img, width, height):
    """Lanczos downscale with premultiplied alpha to avoid dark fringes."""
    pm = premultiply(img).astype(np.uint8)
    pm = np.array(Image.fromarray(pm, "RGBA").resize(
        (width, height), Image.LANCZOS), dtype=np.float32)
    a = pm[:, :, 3:4]
    with np.errstate(divide="ignore", invalid="ignore"):
        rgb = np.where(a > 0, pm[:, :, :3] * 255.0 / a, 0)
    pm[:, :, :3] = np.clip(rgb, 0, 255)
    return Image.fromarray(np.clip(pm, 0, 255).astype(np.uint8), "RGBA")


def analyze_neutral():
    im = Image.open(NEUTRAL_SRC).convert("RGBA")
    bbox = im.getbbox()
    # Edge fringe probe: opaque pixels adjacent to transparency that are gray-ish.
    arr = np.array(im)
    alpha = arr[:, :, 3]
    opaque = alpha > 0
    er = opaque.copy()
    er[1:, :] &= opaque[:-1, :]
    er[:-1, :] &= opaque[1:, :]
    er[:, 1:] &= opaque[:, :-1]
    er[:, :-1] &= opaque[:, 1:]
    edge = opaque & ~er
    rgb = arr[:, :, :3].astype(int)
    chroma = rgb.max(axis=2) - rgb.min(axis=2)
    return {
        "file": str(NEUTRAL_SRC.relative_to(REPO)),
        "size_kb": round(NEUTRAL_SRC.stat().st_size / 1024, 1),
        "canvas": im.size,
        "bbox": bbox,
        "bbox_wh": (bbox[2] - bbox[0], bbox[3] - bbox[1]),
        "edge_pixels": int(edge.sum()),
        "gray_edge_pixels": int((edge & (chroma < 24) & (alpha > 0)).sum()),
    }


def measure_union_bbox(frames):
    x0 = y0 = 10**9
    x1 = y1 = -1
    for f, _ in frames:
        b = f.getbbox()
        if b is None:
            continue
        x0, y0 = min(x0, b[0]), min(y0, b[1])
        x1, y1 = max(x1, b[2]), max(y1, b[3])
    return (x0, y0, x1, y1)


def resample_timing(frames, period_ms=TARGET_PERIOD_MS):
    """Resample by cumulative time so playback speed is unchanged while the
    frame rate lands near the target. Returns [(frame_idx, delay_ms)], src total."""
    times = [0.0]
    for _, d in frames:
        times.append(times[-1] + max(d, 10))
    total = times[-1]
    picked = []
    t = 0.0
    while t < total - 1e-6:
        i = min(bisect.bisect_right(times, t) - 1, len(frames) - 1)
        if picked and picked[-1][0] == i:
            picked[-1][1] += period_ms
        else:
            picked.append([i, period_ms])
        t += period_ms
    return picked, total


def quantize_delays(picked, src_total_ms):
    """Snap delays to the 10 ms GIF grid, then correct total drift on the last
    frame so the loop duration matches the source."""
    delays = [max(10, round(d / 10.0) * 10) for _, d in picked]
    total = sum(delays)
    delta = round((src_total_ms - total) / 10.0) * 10
    delays[-1] = max(10, delays[-1] + delta)
    return delays


def build_palette(frames, max_colors=255):
    """Global adaptive palette (median cut, no dithering) from all frames."""
    sample = []
    for f, _ in frames:
        arr = np.array(f)
        op = arr[arr[:, :, 3] >= ALPHA_OPAQUE][:, :3]
        if op.size:
            sample.append(op[::5])
    px = np.concatenate(sample)
    side = math.ceil(math.sqrt(len(px)))
    buf = np.zeros((side * side, 3), np.uint8)
    buf[: len(px)] = px
    img = Image.fromarray(buf.reshape(side, side, 3))
    pal_img = img.quantize(colors=max_colors, method=Image.MEDIANCUT, dither=Image.Dither.NONE)
    palette = np.array(pal_img.getpalette()[: max_colors * 3], np.uint8).reshape(-1, 3)
    return palette


def apply_palette(frames, palette):
    """Map every frame to P mode: index 255 = transparent, else nearest color."""
    pal = palette.astype(np.int32)
    out = []
    for f, _ in frames:
        arr = np.array(f)
        alpha = arr[:, :, 3]
        rgb = arr[:, :, :3].astype(np.int32)
        h, w = rgb.shape[:2]
        flat = rgb.reshape(-1, 3)
        idx = np.empty(len(flat), np.uint8)
        opaque = alpha.reshape(-1) >= ALPHA_OPAQUE
        # nearest palette entry in chunks (avoids big temporaries)
        op_px = flat[opaque]
        if len(op_px):
            d = np.empty((len(op_px), len(pal)), np.int32)
            for c in range(3):
                d += (op_px[:, c, None] - pal[None, :, c].astype(np.int32)) ** 2
            idx[opaque] = np.argmin(d, axis=1).astype(np.uint8)
        idx[~opaque] = 255
        pimg = Image.fromarray(idx.reshape(h, w), "P")
        full_pal = np.vstack([pal, [[0, 0, 0]]]).astype(np.uint8)
        pimg.putpalette(full_pal.flatten().tolist())
        pimg.info["palette"] = full_pal.tobytes()
        out.append(pimg)
    return out


def roundtrip_check(path, p_frames):
    """Decode the produced GIF with PIL and require exact per-frame equality
    with the accumulated canvas (transparent index = keep previous content).
    Guards against any LZW encoder bug."""
    im = Image.open(path)
    canvas = np.zeros((p_frames[0].size[1], p_frames[0].size[0], 4), np.uint8)
    for i, pf in enumerate(p_frames):
        pal = np.frombuffer(pf.info["palette"], np.uint8).reshape(256, 3)
        idx = np.array(pf)
        op = idx != 255
        canvas[op, :3] = pal[idx[op]]
        canvas[op, 3] = 255
        im.seek(i)
        got = np.array(im.convert("RGBA"))
        if not np.array_equal(got, canvas):
            bad = int((got != canvas).any(axis=2).sum())
            raise AssertionError(f"round-trip mismatch frame {i}: {bad} px differ")


def lzw_compress(indices, min_code_size=8):
    """GIF LZW encoder matching gifdec's width schedule: the decoder bumps its
    code size only after its table's next-assignable slot passes 2^cs, so the
    encoder must bump when its next new code EXCEEDS 2^cs (not when equal).
    12-bit codes; table full -> emit clear and restart."""
    clear = 1 << min_code_size
    eoi = clear + 1
    code_size = min_code_size + 1
    table = {(i,): i for i in range(clear)}
    out = bytearray()
    bitbuf = 0
    bitcnt = 0

    def emit(code):
        nonlocal bitbuf, bitcnt
        bitbuf |= code << bitcnt
        bitcnt += code_size
        while bitcnt >= 8:
            out.append(bitbuf & 0xFF)
            bitbuf >>= 8
            bitcnt -= 8

    emit(clear)
    prev = None
    for ix in indices:
        if prev is None:
            prev = (ix,)
            continue
        cand = prev + (ix,)
        if cand in table:
            prev = cand
            continue
        emit(table[prev])
        nxt = len(table) + 2  # code about to be assigned (first new = 258)
        if nxt <= 4095:
            table[cand] = nxt
            if nxt == (1 << code_size) and code_size < 12:
                code_size += 1
        else:
            emit(clear)
            table = {(i,): i for i in range(clear)}
            code_size = min_code_size + 1
        prev = (ix,)
    if prev is not None:
        emit(table[prev])
    emit(eoi)
    if bitcnt > 0:
        out.append(bitbuf & 0xFF)
    return bytes(out)


def _sub_blocks(data):
    out = bytearray()
    for i in range(0, len(data), 255):
        chunk = data[i : i + 255]
        out.append(len(chunk))
        out += chunk
    out.append(0)
    return bytes(out)


def encode_gif(p_frames, delays, out_path):
    """Hand-rolled GIF89a encoder: exactly one global color table, full-canvas
    frames, disposal=1, explicit NETSCAPE loop ext (format info only - playback
    policy is enforced by the app), transparency index 255."""
    w, h = p_frames[0].size
    palette = bytes(p_frames[0].info["palette"])  # 256*3, set by apply_palette
    assert len(palette) == 768

    out = bytearray()
    out += b"GIF89a"
    out += struct.pack("<HHBBB", w, h, 0xF7, 0, 0)  # GCT flag, 256 colors
    out += palette
    # NETSCAPE2.0 loop forever (loop count 0); app policy overrides anyway
    out += b"\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00"
    for f, d in zip(p_frames, delays):
        delay_units = max(1, round(d / 10)) & 0xFFFF
        # [21 F9 04] [flags: disposal=1 | transparency] [delay:2 LE] [index FF]
        out += struct.pack("<4BHB", 0x21, 0xF9, 0x04, 0x05, delay_units, 255)
        out += b"\x00"  # GCE terminator
        out += struct.pack("<B HH HH B", 0x2C, 0, 0, w, h, 0x00)
        out += b"\x08"  # LZW min code size
        idx = bytes(f.tobytes())
        out += _sub_blocks(lzw_compress(idx))  # includes block terminator
    out += b"\x3B"
    out_path.write_bytes(bytes(out))


def verify_gif(path, expect_frames, expect_delays):
    """Structural verification: global color table present, no local tables,
    correct frame count/delays/loop, canvas size."""
    with open(path, "rb") as f:
        data = f.read()
    assert data[:6] in (b"GIF87a", b"GIF89a"), "bad header"
    w, h = struct.unpack("<HH", data[6:10])
    flags = data[10]
    gct_size = 2 ** ((flags & 0x07) + 1) if (flags >> 7) & 1 else 0
    assert gct_size > 0, "missing global color table (gifdec requires it)"
    pos = 13 + gct_size * 3
    n_img = 0
    local_ct = 0
    loop = None
    delays = []
    transparent_flags = 0
    rects = []
    while pos < len(data):
        b = data[pos]
        if b == 0x21:
            label = data[pos + 1]
            pos += 2
            if label == 0xF9:  # graphic control
                assert data[pos] == 4
                gcb = data[pos + 1]
                if gcb & 1:
                    transparent_flags += 1
                delays.append(struct.unpack("<H", data[pos + 2 : pos + 4])[0] * 10)
                assert (gcb >> 2) & 0x07 == 1, f"disposal != 1 at frame {n_img}"
                pos += 6  # size byte + 4 data + terminator
            elif label == 0xFF:
                app_len = data[pos]
                app_id = data[pos + 1 : pos + 1 + app_len]
                pos += 1 + app_len  # -> first sub-block size byte
                sub_len = data[pos]
                if app_id.startswith(b"NETSCAPE2.0") and sub_len >= 3:
                    loop = struct.unpack("<H", data[pos + 2 : pos + 4])[0]
                pos += sub_len + 1  # past this sub-block
                while data[pos] != 0:
                    pos += data[pos] + 1
                pos += 1
            else:
                # pos at sub-block size byte
                while data[pos] != 0:
                    pos += data[pos] + 1
                pos += 1
        elif b == 0x2C:
            left, top, fw, fh, id_flags = struct.unpack("<HHHHB", data[pos + 1 : pos + 10])
            if (id_flags >> 7) & 1:
                local_ct += 1
                pos += 3 * 2 ** ((id_flags & 0x07) + 1)
            rects.append((left, top, fw, fh))
            n_img += 1
            pos += 10
            pos += 1  # LZW min code size
            while data[pos] != 0:
                pos += data[pos] + 1
            pos += 1
        elif b == 0x3B:
            break
        else:
            raise AssertionError(f"unknown block 0x{b:02x} at {pos}")
    assert n_img == expect_frames, f"frames {n_img} != {expect_frames}"
    assert local_ct == 0, f"{local_ct} local color tables"
    assert all(r == (0, 0, w, h) for r in rects), f"non-full-canvas rect: {rects[:3]}"
    assert transparent_flags == n_img, "GCE transparency flag missing on some frames"
    assert loop == 0, f"loop ext {loop}"
    assert delays[: len(expect_delays)] == list(expect_delays), "delay mismatch"
    return {"canvas": (w, h), "gct": gct_size, "frames": n_img, "loop": loop,
            "total_ms": sum(delays)}


def build_action(name, spec, report):
    src = spec["src"]
    frames = flatten_frames(src)
    strip_stats = None
    if spec.get("strip_border"):
        stripped = []
        removed_total = 0
        leftover_total = 0
        for f, d in frames:
            f2, removed, leftover = strip_white_border(f)
            stripped.append((f2, d))
            removed_total += removed
            leftover_total += leftover
        frames = stripped
        strip_stats = {"removed_px_total": removed_total,
                       "leftover_white_px_total": leftover_total}

    union = measure_union_bbox(frames)
    uw, uh = union[2] - union[0], union[3] - union[1]

    if spec["mode"] == "single":
        first_bbox = frames[0][0].getbbox()
        char_h = first_bbox[3] - first_bbox[1]
        scale = STAND_H / char_h
    else:  # fit whole scene, keep everything visible
        scale = min((CANVAS_W - 2 * FIT_MARGIN) / uw, (GROUND_Y - FIT_MARGIN) / uh)

    src_w, src_h = frames[0][0].size
    sw, sh = round(src_w * scale), round(src_h * scale)
    # source point (union center x, union bottom y) -> (128, GROUND_Y)
    x_off = round(CANVAS_W / 2 - (union[0] + uw / 2) * scale)
    y_off = round(GROUND_Y - union[3] * scale)

    scaled = [(resize_rgba(f, sw, sh), d) for f, d in frames]
    placed = []
    for f, d in scaled:
        canvas = Image.new("RGBA", (CANVAS_W, CANVAS_H), (0, 0, 0, 0))
        canvas.paste(f, (x_off, y_off))
        placed.append((canvas, d))

    picked, src_total = resample_timing(placed)
    delays = quantize_delays(picked, src_total)
    seq = [placed[i][0] for i, _ in picked]

    palette = build_palette(placed)
    p_frames = apply_palette([(seq[i], None) for i in range(len(seq))], palette)
    out_path = PACK / "actions" / f"{name}.gif"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    encode_gif(p_frames, delays, out_path)
    ver = verify_gif(out_path, len(seq), delays)
    roundtrip_check(out_path, p_frames)

    out_bytes = out_path.stat().st_size
    entry = {
        "id": name,
        "source": str(src.relative_to(REPO)),
        "mode": spec["mode"],
        "src_frames": len(frames),
        "src_ms": round(src_total),
        "src_union_bbox": union,
        "scale": round(scale, 4),
        "offset": [x_off, y_off],
        "resized_canvas": [sw, sh],
        "frames": len(seq),
        "delays_ms": delays,
        "loop_ms": sum(delays),
        "fps": round(len(seq) / (sum(delays) / 1000), 2),
        "gif_bytes": out_bytes,
        "bytes_per_frame": round(out_bytes / len(seq), 1),
        "gif_verify": ver,
        "playback": spec["playback"],
        "allowed_modes": spec["allowed_modes"],
    }
    if strip_stats:
        entry["border_strip"] = strip_stats
    report["actions"][name] = entry
    return placed


def build_neutral(report):
    im = Image.open(NEUTRAL_SRC).convert("RGBA")
    bbox = im.getbbox()
    crop = im.crop(bbox)
    scale = STAND_H / crop.height
    tw, th = round(crop.width * scale), round(crop.height * scale)
    canvas = Image.new("RGBA", (CANVAS_W, CANVAS_H), (0, 0, 0, 0))
    thumb = resize_rgba(crop, tw, th)
    x = round(CANVAS_W / 2 - tw / 2)
    y = GROUND_Y - th
    canvas.paste(thumb, (x, y))
    out = PACK / "neutral.png"
    out.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(out)

    # fringe check on the light-blue composite
    bg = Image.new("RGBA", (CANVAS_W, CANVAS_H), (*BG, 255))
    bg.alpha_composite(canvas)
    arr = np.array(bg).astype(int)
    rgba = np.array(canvas)
    alpha = rgba[:, :, 3]
    opaque = alpha > 0
    er = opaque.copy()
    er[1:, :] &= opaque[:-1, :]
    er[:-1, :] &= opaque[1:, :]
    er[:, 1:] &= opaque[:, :-1]
    er[:, :-1] &= opaque[:, 1:]
    edge = opaque & ~er
    chroma = arr[:, :, :3].max(axis=2) - arr[:, :, :3].min(axis=2)
    gray_edge = int((edge & (chroma < 24)).sum())

    report["neutral"] = {
        "source": str(NEUTRAL_SRC.relative_to(REPO)),
        "src_bbox": bbox,
        "scale": round(scale, 5),
        "resized": [tw, th],
        "offset": [x, y],
        "file": "neutral.png",
        "png_bytes": out.stat().st_size,
        "a8_full_canvas_bytes": CANVAS_W * CANVAS_H,
        "a8_bbox_bytes": tw * th,
        "edge_px": int(edge.sum()),
        "gray_edge_px": gray_edge,
    }
    return canvas


def contact_sheet(frames, path, cols=8):
    """Frames composited on the light-blue stage, 1x, tiled for review."""
    n = len(frames)
    rows = math.ceil(n / cols)
    gap = 2
    sheet = Image.new("RGB", (cols * (CANVAS_W + gap) + gap,
                              rows * (CANVAS_H + gap) + gap), (64, 64, 64))
    for i, (f, _) in enumerate(frames):
        r, c = divmod(i, cols)
        tile = Image.new("RGBA", (CANVAS_W, CANVAS_H), (*BG, 255))
        tile.alpha_composite(f)
        sheet.paste(tile.convert("RGB"),
                    (gap + c * (CANVAS_W + gap), gap + r * (CANVAS_H + gap)))
    sheet.save(path)


def write_pack_json(report):
    canvas = {
        "width": CANVAS_W,
        "height": CANVAS_H,
        "ground_y": GROUND_Y,
        "stage_x": 32,
    }
    pack = {
        "character_pack": {
            "version": 1,
            "pack_id": "peppa-v1",
            "background": "#DCEFF8",
            "canvas": canvas,
            "neutral": {
                "file": "neutral.bin",
                "format": "lvgl-bin-argb8888",  # converted from neutral.png via Image_Converter/LVGLImage.py
                "width": CANVAS_W,
                "height": CANVAS_H,
                "feet_y": GROUND_Y,
            },
            "actions": [],
        }
    }
    for name in ACTIONS:
        e = report["actions"][name]
        pack["character_pack"]["actions"].append({
            "id": name,
            "file": f"actions/{name}.gif",
            "format": "gif",
            "width": CANVAS_W,
            "height": CANVAS_H,
            "frames": e["frames"],
            "loop_ms": e["loop_ms"],
            "default_playback": e["playback"],
            "allowed_modes": e["allowed_modes"],
            "fallback": "neutral",
        })
    (PACK / "pack.json").write_text(
        json.dumps(pack, indent=2, ensure_ascii=False), encoding="utf-8")


def write_report(report):
    total_gif = sum(e["gif_bytes"] for e in report["actions"].values())
    lines = [
        "# Peppa v1 资源包制作报告",
        "",
        "| 动作 | 模式 | 缩放 | 帧数(源→出) | 时长(ms 源→出) | fps | GIF 字节 |",
        "|---|---|---|---|---|---|---|",
    ]
    for name, e in report["actions"].items():
        lines.append(
            f"| {name} | {e['mode']} | {e['scale']} | {e['src_frames']}→{e['frames']} "
            f"| {e['src_ms']}→{e['loop_ms']} | {e['fps']} | {e['gif_bytes']} |")
    n = report["neutral"]
    lines += [
        "",
        f"- neutral: 缩放 {n['scale']}，贴到 {n['offset']}，PNG {n['png_bytes']} B；"
        f"A8 全画布 {n['a8_full_canvas_bytes']} B / 按 bbox {n['a8_bbox_bytes']} B",
        f"- neutral 边缘检查：边缘像素 {n['edge_px']}，灰边像素 {n['gray_edge_px']}",
        f"- 动作 GIF 合计 {total_gif} B",
        f"- 解码 RAM 估算（设计文档 §12 公式 5×W×H）：{5 * CANVAS_W * CANVAS_H} B/解码器"
        f"（256×168 画布），另加 LZW 表与结构体",
    ]
    (PACK / "report.md").write_text("\n".join(lines), encoding="utf-8")


def build():
    if PACK.exists():
        shutil.rmtree(PACK)
    (PACK / "raw").mkdir(parents=True)
    (PACK / "preview").mkdir(parents=True)

    # self-contained raw sources
    shutil.copy2(NEUTRAL_SRC, PACK / "raw" / "peppa-neutral-source.webp")
    for name, spec in ACTIONS.items():
        shutil.copy2(spec["src"], PACK / "raw" / spec["src"].name)

    report = {"actions": {}}
    neutral_img = build_neutral(report)
    contact_sheet([(neutral_img, 0)], PACK / "preview" / "neutral.png", cols=1)
    for name, spec in ACTIONS.items():
        placed = build_action(name, spec, report)
        contact_sheet(placed, PACK / "preview" / f"{name}.png")
    write_pack_json(report)
    write_report(report)
    (PACK / "report.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps(report, indent=2, ensure_ascii=False))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=["analyze", "build"])
    args = ap.parse_args()
    if args.command == "analyze":
        report = {"neutral": analyze_neutral(), "actions": {}}
        for name, spec in ACTIONS.items():
            report["actions"][name] = analyze_gif(spec["src"])
        print(json.dumps(report, indent=2, ensure_ascii=False))
    else:
        build()


if __name__ == "__main__":
    main()
