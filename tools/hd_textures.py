"""HD texture packs for the runtime: host-side replacements of the game's textures that use
neither guest memory nor package rewrites (runtime/gpu/hd_textures.h).

    python tools/hd_textures.py match [--dump DIR]           # dumped textures -> exported PNGs
    python tools/hd_textures.py build --mod NAME --src DIR   # edited exports -> mods/NAME/hd_textures
    python tools/hd_textures.py check --mod NAME             # re-reads a pack (sizes, mips, decode)

1. Dump: run the game with graphics.texture_dump = true (settings.toml, or
   CRASH_SETTINGS="graphics.texture_dump=true"). Every texture the renderer uploads is written
   once to userdata\\texture_dump\\<key>.png (base level, in the channel order of the host texture)
   with a line in index.csv (key, size, guest/host format, fetch swizzle). Play through what the
   pack should cover; dumps accumulate across runs.
2. match: finds, for each dump, the exported texture with the same pixels
   (private/assets/textures, tools/textures.py export): the dump seen through its fetch swizzle is
   compared with the export, exactly for 8-bit textures and within a small error for DXT ones
   (the game stores most textures as PNG but some as DXT). The game keeps its images bottom-up
   (first row = the export's last row), so the export is compared flipped (and upright, just in
   case). Exports the game keeps at half or a quarter of their size also match (scaled down).
   Result: <dump>/matches.json.
   It also checks the predicted keys (below) against every dumped 8-bit texture.
3. build: for each matched export found in --src (the same relative paths as
   private/assets/textures, e.g. an upscaled copy: fonte_max/textures/personagens/crash/Crash_V2.png),
   writes mods/NAME/hd_textures/<key>.dds: the edited image flipped like the game's and with its
   channels put back in the host texture's order (inverse of the fetch swizzle), a full mip chain (box filter), as BC7 (mode 6,
   default) or RGBA8 (--format rgba8). The edit must be larger than the game's texture with the
   same aspect ratio. manifest.json lists every file with its source.
   Predicted keys: the game turns its PNG textures into 8-bit RGBA textures, bottom-up, BGRA in
   memory (swizzle 0x60A), so build also computes, for every edited export, the key the runtime
   will see (runtime_key = gpu::hd::Key) without a dump. Textures the game keeps in another format
   (DXT, smaller copies) still need a dump + match. --no-predict: only dumped textures.

The runtime loads mods/<mod>/hd_textures/*.dds of the enabled mods (graphics.hd_textures = true, the
default) and binds the HD texture in place of the guest one whose base level has that key, keeping
the guest's addressing (UVs are normalized; texel offsets use the guest size). Render targets and
resolve copies are never replaced. Each replacement is logged ("[gpu] hd texture ...").
Game data: never publish the dumps or the packs.
"""
import argparse
import csv
import json
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
EXPORTS = ROOT / "private" / "assets" / "textures"
DEFAULT_DUMP = ROOT / "dist" / "userdata" / "texture_dump"

# DXGI formats written by build.
DXGI_R8G8B8A8_UNORM, DXGI_BC7_UNORM = 28, 98


# --- swizzle ----------------------------------------------------------------------------------

def swizzle_parts(swizzle):
    """Fetch constant swizzle: 3 bits per output component (r, g, b, a); 0-3 pick x/y/z/w of the
    sampled host texel, 4 = 0, 5 = 1."""
    return [(swizzle >> (3 * i)) & 7 for i in range(4)]


def apply_swizzle(raw, swizzle):
    """Host texels (H, W, 4) -> what the shader sees."""
    out = np.empty_like(raw)
    for i, s in enumerate(swizzle_parts(swizzle)):
        out[..., i] = raw[..., s] if s < 4 else (0 if s == 4 else 255)
    return out


def unapply_swizzle(visual, swizzle):
    """Inverse of apply_swizzle for building: raw channel c takes the output that reads it;
    channels no output reads are 255."""
    raw = np.full_like(visual, 255)
    parts = swizzle_parts(swizzle)
    for c in range(4):
        for i, s in enumerate(parts):
            if s == c:
                raw[..., c] = visual[..., i]
                break
    return raw


def used_outputs(swizzle):
    """Output components that come from the texture (not constants)."""
    return [i for i, s in enumerate(swizzle_parts(swizzle)) if s < 4]


# --- matching ---------------------------------------------------------------------------------

def load_rgba(path):
    im = Image.open(path)
    has_alpha = im.mode in ("RGBA", "LA", "PA") or (im.mode == "P" and "transparency" in im.info)
    return np.asarray(im.convert("RGBA"), dtype=np.uint8), has_alpha


def read_dump_index(dump):
    rows = {}
    with open(dump / "index.csv", newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            rows[r["key"]] = dict(key=r["key"], width=int(r["width"]), height=int(r["height"]),
                                  guest_format=int(r["guest_format"]), host_format=r["host_format"],
                                  swizzle=int(r["swizzle"], 16), levels=int(r["levels"]))
    return rows


def read_exports(textures):
    exports = []
    with open(textures / "indice.csv", newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r["tipo"] != "textura":
                continue  # frontend sprites are cut into tiles in the game
            exports.append(dict(file=r["arquivo"], width=int(r["largura"]), height=int(r["altura"]), format=r["formato"]))
    return exports


def thumb(rgba, size=8):
    """Mean colour of size x size cells (float32, RGB)."""
    h, w = rgba.shape[:2]
    if h % size or w % size:
        return np.asarray(Image.fromarray(rgba).convert("RGB").resize((size, size), Image.BOX), dtype=np.float32)
    return rgba[..., :3].reshape(size, h // size, size, w // size, 3).astype(np.float32).mean(axis=(1, 3))


def downscale(rgba, factor):
    if factor == 1:
        return rgba
    h, w = rgba.shape[:2]
    return rgba.reshape(h // factor, factor, w // factor, factor, 4).astype(np.float32).mean(axis=(1, 3)).round().astype(np.uint8)


def cmd_match(args):
    dump = Path(args.dump)
    textures = Path(args.textures)
    dumps = read_dump_index(dump)
    exports = read_exports(textures)
    by_size = {}
    for e in exports:
        by_size.setdefault((e["width"], e["height"]), []).append(e)
    cache = {}

    def export_pixels(e):
        if e["file"] not in cache:
            cache[e["file"]] = load_rgba(textures / e["file"])
        return cache[e["file"]]

    matches, unmatched = {}, []
    for n, (key, d) in enumerate(sorted(dumps.items())):
        png = dump / f"{key}.png"
        if not png.exists():
            continue
        if d["width"] < 4 or d["height"] < 4:
            unmatched.append((key, f"{d['width']}x{d['height']}: 1D/lookup texture (not an exported image)"))
            continue
        raw = np.asarray(Image.open(png).convert("RGBA"), dtype=np.uint8)
        visual = apply_swizzle(raw, d["swizzle"])
        outputs = used_outputs(d["swizzle"])
        rgb = [i for i in outputs if i < 3]
        if not rgb:
            unmatched.append((key, "no colour channel"))
            continue
        compressed = d["host_format"].startswith("BC")
        tolerance = 8.0 if compressed else 0.75
        vt = thumb(visual) if visual.shape[0] >= 8 and visual.shape[1] >= 8 else None
        best = None
        for factor in (1, 2, 4):
            candidates = by_size.get((d["width"] * factor, d["height"] * factor), [])
            for e in candidates:
                pixels, has_alpha = export_pixels(e)
                # The game keeps its images bottom-up (they come from .bmp-named textures): the
                # texture's first row is the export's last one. Upright is tried too.
                for flip in (True, False):
                    oriented = pixels[::-1] if flip else pixels
                    if vt is not None and factor == 1:
                        if np.abs(thumb(oriented)[..., rgb] - vt[..., rgb]).mean() > tolerance * 2 + 2:
                            continue
                    small = downscale(oriented, factor)
                    channels = rgb + ([3] if 3 in outputs and has_alpha else [])
                    err = np.abs(small[..., channels].astype(np.int16) - visual[..., channels].astype(np.int16)).mean()
                    if factor > 1:
                        err = max(0.0, err - 1.0)  # rounding of the game's own downscale
                    if best is None or err < best[0]:
                        best = (err, e, factor, flip)
            if best and best[0] <= tolerance:
                break
        if best and best[0] <= (tolerance if best[2] == 1 else tolerance + 4):
            err, e, factor, flip = best
            matches[key] = dict(file=e["file"], width=d["width"], height=d["height"], export_size=[e["width"], e["height"]],
                                swizzle=f"0x{d['swizzle']:03X}", flip=flip, host_format=d["host_format"], guest_format=d["guest_format"],
                                error=round(float(err), 3), export_scale=factor)
        else:
            unmatched.append((key, f"{d['width']}x{d['height']} {d['host_format']}" +
                              (f", closest {best[1]['file']} (error {best[0]:.1f})" if best else ", no export of that size")))
        if (n + 1) % 200 == 0:
            print(f"  {n + 1}/{len(dumps)}...")
    # The predicted keys (build) must agree with the dumped 8-bit textures.
    agree = disagree = 0
    for key, m in matches.items():
        if m["host_format"] == "RGBA8" and m["swizzle"] == f"0x{GAME_SWIZZLE:03X}" and m["flip"] and m["export_scale"] == 1:
            if predicted_key(export_pixels(dict(file=m["file"]))[0]) == key:
                agree += 1
            else:
                disagree += 1
    print(f"chaves previstas a partir das exportações: {agree} conferem com o dump, {disagree} não")
    out = dump / "matches.json"
    out.write_text(json.dumps(dict(matches=matches, unmatched=dict(unmatched)), indent=1, ensure_ascii=False), encoding="utf-8")
    files = {m["file"] for m in matches.values()}
    print(f"{len(matches)} of {len(dumps)} dumped textures matched {len(files)} exported file(s); {len(unmatched)} unmatched -> {out}")
    unmatched.sort(key=lambda kv: ("lookup" in kv[1], kv[0]))
    for key, why in unmatched[:args.show]:
        print(f"  {key}: {why}")
    if len(unmatched) > args.show:
        print(f"  ... ({len(unmatched) - args.show} more in matches.json)")


# --- BC7 (mode 6) -----------------------------------------------------------------------------

BC7_WEIGHTS4 = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64], dtype=np.int32)


def bc7_encode(rgba):
    """RGBA8 (H, W, 4), H and W multiples of 4 -> BC7 blocks (mode 6: one subset, RGBA endpoints
    of 7 bits + a p-bit each, 4-bit indices). Endpoints along each block's principal axis, then
    every texel takes the nearest of the 16 palette colours."""
    h, w = rgba.shape[:2]
    blocks = rgba.reshape(h // 4, 4, w // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4).astype(np.float32)
    out = np.empty((blocks.shape[0], 16), dtype=np.uint8)
    for start in range(0, blocks.shape[0], 32768):
        out[start:start + 32768] = bc7_encode_blocks(blocks[start:start + 32768])
    return out.tobytes()


def bc7_encode_blocks(px):
    n = px.shape[0]
    mean = px.mean(axis=1, keepdims=True)
    centered = px - mean
    cov = np.einsum("nki,nkj->nij", centered, centered)
    axis = np.ones((n, 4), dtype=np.float32) * 0.5
    for _ in range(8):
        axis = np.einsum("nij,nj->ni", cov, axis)
        norm = np.linalg.norm(axis, axis=1, keepdims=True)
        axis = np.where(norm > 1e-6, axis / np.maximum(norm, 1e-12), 0.5)
    t = np.einsum("nki,ni->nk", centered, axis)
    e0 = mean[:, 0] + t.min(axis=1, keepdims=True) * axis
    e1 = mean[:, 0] + t.max(axis=1, keepdims=True) * axis
    e0, e1 = np.clip(e0, 0, 255), np.clip(e1, 0, 255)

    def quantize(e):
        best_q = best_err = best_p = None
        for p in (0, 1):
            q = np.clip(np.round((e - p) / 2), 0, 127)
            err = np.abs(q * 2 + p - e).sum(axis=1)
            if best_err is None:
                best_q, best_err, best_p = q, err, np.full(n, p)
            else:
                better = err < best_err
                best_q = np.where(better[:, None], q, best_q)
                best_err = np.where(better, err, best_err)
                best_p = np.where(better, p, best_p)
        return best_q.astype(np.int32), best_p.astype(np.int32)

    q0, p0 = quantize(e0)
    q1, p1 = quantize(e1)
    v0 = q0 * 2 + p0[:, None]
    v1 = q1 * 2 + p1[:, None]
    palette = ((64 - BC7_WEIGHTS4)[None, :, None] * v0[:, None, :] + BC7_WEIGHTS4[None, :, None] * v1[:, None, :] + 32) >> 6
    dist = ((px[:, :, None, :] - palette[:, None, :, :].astype(np.float32)) ** 2).sum(axis=3)
    idx = dist.argmin(axis=2).astype(np.int32)
    # The anchor (texel 0) index must have its top bit clear: swap the endpoints otherwise.
    swap = idx[:, 0] >= 8
    q0, q1 = np.where(swap[:, None], q1, q0), np.where(swap[:, None], q0, q1)
    p0, p1 = np.where(swap, p1, p0), np.where(swap, p0, p1)
    idx = np.where(swap[:, None], 15 - idx, idx)

    # Pack 128 bits, LSB first: mode (7: 1 << 6), R0 R1 G0 G1 B0 B1 A0 A1 (7 each), P0, P1,
    # indices (texel 0: 3 bits, the rest 4).
    lo = np.full(n, 1 << 6, dtype=np.uint64)
    hi = np.zeros(n, dtype=np.uint64)
    pos = 7

    def put(value, bits):
        nonlocal lo, hi, pos
        value = value.astype(np.uint64)
        if pos >= 64:
            hi |= value << np.uint64(pos - 64)
        elif pos + bits <= 64:
            lo |= value << np.uint64(pos)
        else:
            low_bits = 64 - pos
            lo |= (value & np.uint64((1 << low_bits) - 1)) << np.uint64(pos)
            hi |= value >> np.uint64(low_bits)
        pos += bits

    for c in range(4):
        put(q0[:, c], 7)
        put(q1[:, c], 7)
    put(p0, 1)
    put(p1, 1)
    put(idx[:, 0], 3)
    for i in range(1, 16):
        put(idx[:, i], 4)
    assert pos == 128
    out = np.empty((n, 16), dtype=np.uint8)
    out[:, :8] = lo.view(np.uint8).reshape(n, 8)
    out[:, 8:] = hi.view(np.uint8).reshape(n, 8)
    return out


def bc7_decode_mode6(data, width, height):
    """Decoder for the blocks bc7_encode writes (mode 6 only), for check and the self-test."""
    blocks = np.frombuffer(data, dtype=np.uint8).reshape(-1, 16)
    lo = blocks[:, :8].copy().view(np.uint64)[:, 0]
    hi = blocks[:, 8:].copy().view(np.uint64)[:, 0]
    pos = 7

    def get(bits):
        nonlocal pos
        if pos >= 64:
            v = hi >> np.uint64(pos - 64)
        elif pos + bits <= 64:
            v = lo >> np.uint64(pos)
        else:
            v = (lo >> np.uint64(pos)) | (hi << np.uint64(64 - pos))
        pos += bits
        return (v & np.uint64((1 << bits) - 1)).astype(np.int32)

    if not np.all((lo & np.uint64(0x7F)) == np.uint64(0x40)):
        raise ValueError("not a mode 6 BC7 stream")
    e = np.stack([np.stack([get(7), get(7)], axis=1) for _ in range(4)], axis=2)  # (n, 2, 4)
    p = np.stack([get(1), get(1)], axis=1)
    v = e * 2 + p[:, :, None]
    idx = np.stack([get(3)] + [get(4) for _ in range(15)], axis=1)
    w = BC7_WEIGHTS4[idx]
    texels = ((64 - w)[:, :, None] * v[:, None, 0, :] + w[:, :, None] * v[:, None, 1, :] + 32) >> 6
    return texels.reshape(height // 4, width // 4, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(height, width, 4).astype(np.uint8)


# --- DDS --------------------------------------------------------------------------------------

def mip_chain(rgba):
    levels = [rgba]
    while levels[-1].shape[0] > 1 or levels[-1].shape[1] > 1:
        cur = levels[-1].astype(np.float32)
        h, w = cur.shape[:2]
        if h > 1:
            cur = (cur[0::2] + cur[1::2]) / 2 if h % 2 == 0 else cur
        if w > 1:
            cur = (cur[:, 0::2] + cur[:, 1::2]) / 2 if w % 2 == 0 else cur
        levels.append(cur.round().astype(np.uint8))
    return levels


def pad4(rgba):
    h, w = rgba.shape[:2]
    ph, pw = (-h) % 4, (-w) % 4
    return np.pad(rgba, ((0, ph), (0, pw), (0, 0)), mode="edge") if ph or pw else rgba


def write_dds(path, levels, fmt):
    h, w = levels[0].shape[:2]
    payload = bytearray()
    for level in levels:
        payload += bc7_encode(pad4(level)) if fmt == DXGI_BC7_UNORM else level.tobytes()
    bc = fmt == DXGI_BC7_UNORM
    pitch = ((w + 3) // 4) * 16 if bc else w * 4
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | (0x80000 if bc else 0x8)  # caps, height, width, pixelformat, mipcount, linearsize/pitch
    header = struct.pack("<4sIIIIIII44x", b"DDS ", 124, flags, h, w, pitch * (((h + 3) // 4) if bc else 1), 0, len(levels))
    header += struct.pack("<II4sIIIII", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
    header += struct.pack("<IIIII", 0x1000 | 0x400000 | 0x8, 0, 0, 0, 0)  # texture, mipmap, complex
    header += struct.pack("<IIIII", fmt, 3, 0, 1, 0)  # dxgi format, TEXTURE2D, misc, array size, alpha mode
    path.write_bytes(header + payload)
    return len(header) + len(payload)


def read_dds(path):
    data = path.read_bytes()
    h, w = struct.unpack_from("<II", data, 12)
    mips = struct.unpack_from("<I", data, 28)[0]
    fourcc = data[84:88]
    fmt = struct.unpack_from("<I", data, 128)[0] if fourcc == b"DX10" else None
    return data, w, h, mips, fmt


# --- predicted keys ---------------------------------------------------------------------------

M64 = (1 << 64) - 1
KEY_P = 0x9E3779B97F4A7C15
GAME_SWIZZLE = 0x60A  # what the game's 8-bit textures use (BGRA in memory), see the dumps


def runtime_key(raw, dxgi_format=DXGI_R8G8B8A8_UNORM):
    """gpu::hd::Key of runtime/gpu/hd_textures.cpp for an uncompressed host texture (H, W, 4)."""
    h_, w_ = raw.shape[:2]
    h = (0xCBF29CE484222325 ^ (w_ << 40) ^ (h_ << 16) ^ dxgi_format) & M64
    h = (h * KEY_P) & M64
    row_bytes = w_ * 4
    words = row_bytes // 8
    data = np.ascontiguousarray(raw, dtype=np.uint8).reshape(h_, row_bytes)
    for row in data:
        for v in row[:words * 8].view("<u8").tolist():
            h = ((h ^ v) * KEY_P) & M64
            h ^= h >> 29
        for b in row[words * 8:].tolist():
            h = ((h ^ b) * 0x100000001B3) & M64
    h ^= h >> 32
    h = (h * KEY_P) & M64
    h ^= h >> 29
    return f"{h:016X}"


def predicted_key(export_rgba):
    """Key the game's texture made from this export would have: an 8-bit RGBA texture, bottom-up,
    BGRA in memory (fetch swizzle 0x60A). Holds for the game's PNG textures (checked by match
    against every dumped one); other formats need a dump."""
    return runtime_key(unapply_swizzle(export_rgba[::-1], GAME_SWIZZLE))


# --- build ------------------------------------------------------------------------------------

def cmd_build(args):
    dump = Path(args.dump)
    matches_path = dump / "matches.json"
    matches = json.loads(matches_path.read_text(encoding="utf-8"))["matches"] if matches_path.exists() else {}
    src = Path(args.src)
    textures = Path(args.textures)
    # Every edited file that is an export also gets its predicted key (no dump needed).
    dumped_files = {m["file"] for m in matches.values()}
    predicted = 0
    for e in read_exports(textures):
        if not (src / e["file"]).exists() or args.no_predict:
            continue
        pixels = load_rgba(textures / e["file"])[0]
        key = predicted_key(pixels)
        if key not in matches:
            matches[key] = dict(file=e["file"], width=e["width"], height=e["height"], swizzle=f"0x{GAME_SWIZZLE:03X}", flip=True,
                                host_format="RGBA8", predicted=True)
            predicted += e["file"] not in dumped_files
    print(f"{len(matches)} chave(s): {len(dumped_files)} arquivo(s) vistos no dump, {predicted} só previstos")
    out = Path(args.mods_dir) / args.mod / "hd_textures"
    out.mkdir(parents=True, exist_ok=True)
    fmt = DXGI_BC7_UNORM if args.format == "bc7" else DXGI_R8G8B8A8_UNORM
    manifest_path = out / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.exists() else {}
    edited = {}
    written = skipped = 0
    total = 0
    for key, m in sorted(matches.items(), key=lambda kv: kv[1]["file"]):
        path = src / m["file"]
        if not path.exists():
            continue
        if path not in edited:
            edited[path] = load_rgba(path)[0]
        visual = edited[path]
        h, w = visual.shape[:2]
        gw, gh = m["width"], m["height"]
        if w * gh != h * gw or w <= gw:
            print(f"  {m['file']}: {w}x{h} não é uma ampliação de {gw}x{gh} com a mesma proporção (pulado)")
            skipped += 1
            continue
        raw = unapply_swizzle(visual[::-1] if m.get("flip") else visual, int(m["swizzle"], 16))
        levels = mip_chain(raw)
        name = f"{key}.dds"
        size = write_dds(out / name, levels, fmt)
        total += size
        manifest[key] = dict(file=name, source=m["file"], guest=[gw, gh], hd=[w, h], mips=len(levels),
                             format=args.format, swizzle=m["swizzle"], flip=bool(m.get("flip")), host_format=m["host_format"],
                             key="prevista" if m.get("predicted") else "dump")
        written += 1
    manifest_path.write_text(json.dumps(dict(sorted(manifest.items())), indent=1, ensure_ascii=False), encoding="utf-8")
    print(f"{written} textura(s) HD ({len(edited)} arquivo(s) editados, {args.format}, {total / 2**20:.0f} MB) -> {out}"
          + (f"; {skipped} pulada(s)" if skipped else ""))


def cmd_check(args):
    out = Path(args.mods_dir) / args.mod / "hd_textures"
    manifest = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
    bad = 0
    for key, m in manifest.items():
        data, w, h, mips, fmt = read_dds(out / m["file"])
        ok = [w, h] == m["hd"] and mips == m["mips"] and fmt in (DXGI_BC7_UNORM, DXGI_R8G8B8A8_UNORM)
        if ok and args.decode:
            if fmt == DXGI_BC7_UNORM:
                top = bc7_decode_mode6(data[148:148 + ((w + 3) // 4) * ((h + 3) // 4) * 16], (w + 3) // 4 * 4, (h + 3) // 4 * 4)[:h, :w]
            else:
                top = np.frombuffer(data, np.uint8, w * h * 4, 148).reshape(h, w, 4)
            visual = apply_swizzle(top, int(m["swizzle"], 16))
            visual = visual[::-1] if m.get("flip") else visual
            src = load_rgba(Path(args.src) / m["source"])[0] if args.src else None
            if src is not None:
                outs = used_outputs(int(m["swizzle"], 16))
                err = np.abs(visual[..., outs].astype(np.int16) - src[..., outs].astype(np.int16)).mean()
                print(f"  {key} {m['source']}: erro médio {err:.2f}")
        if not ok:
            bad += 1
            print(f"  {key}: arquivo não confere com o manifest ({w}x{h}, {mips} mips, formato {fmt})")
    print(f"{len(manifest)} textura(s), {bad} com problema")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("match", help="dumps -> texturas exportadas")
    s.add_argument("--dump", default=str(DEFAULT_DUMP))
    s.add_argument("--textures", default=str(EXPORTS))
    s.add_argument("--show", type=int, default=20)
    s = sub.add_parser("build", help="exportações editadas -> mods/NOME/hd_textures")
    s.add_argument("--mod", required=True)
    s.add_argument("--src", required=True, help="pasta com as imagens editadas (mesmos caminhos de private/assets/textures)")
    s.add_argument("--mods-dir", default=str(ROOT / "dist" / "mods"))
    s.add_argument("--dump", default=str(DEFAULT_DUMP))
    s.add_argument("--format", choices=("bc7", "rgba8"), default="bc7")
    s.add_argument("--textures", default=str(EXPORTS))
    s.add_argument("--no-predict", action="store_true", help="só as texturas vistas no dump")
    s = sub.add_parser("check", help="relê um pacote")
    s.add_argument("--mod", required=True)
    s.add_argument("--mods-dir", default=str(ROOT / "dist" / "mods"))
    s.add_argument("--decode", action="store_true", help="decodifica o nível 0 e compara com --src")
    s.add_argument("--src")
    args = parser.parse_args()
    {"match": cmd_match, "build": cmd_build, "check": cmd_check}[args.cmd](args)


if __name__ == "__main__":
    main()
