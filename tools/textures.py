"""Exports the game's textures to PNG, one file per distinct image, and builds texture mods.

    python tools/textures.py export                 # every texture of every .p3d
    python tools/textures.py find TEXT              # search by texture/package name
    python tools/textures.py build --mod NAME       # edited PNGs -> mods/NAME/rcf/default/...
    python tools/textures.py verify                 # index == packages; no-op patch == original

Run `python tools/assets.py extract` first (packages are read from private/assets/raw).

How the game stores textures (Pure3D, little-endian chunks; see tools/p3d.py): a texture chunk
(0x19000: name, version, width, height, bpp, alpha depth, mip count, type, usage, priority) or a
frontend sprite (0x19005) holds image chunks (0x19001: name, version, width, height, bpp,
palettized, has alpha, format), one per mip level (base first, named "x.bmp", "x_1.png", ...),
each with an image data chunk (0x19002: u32 size + the file). On the 360 build the images are
ordinary files, not Xenos-tiled GPU data: format 1 = PNG (almost all), 2 = TGA (color-mapped),
6/8/10 = DDS (DXT1/DXT3/DXT5, one level). The game decodes and tiles them when loading.

Mip levels (x_1 ... x_5: the same image at half, a quarter ... of the size, which the GPU samples
for distant surfaces) are not exported: build regenerates them from the base level, in each
level's own stored format. Frontend sprites are cut into power-of-two tiles that overlap by 2
pixels (a 1-pixel border on each side, for bilinear filtering); they are exported stitched.

Output (game data: private/, never publish it):
    private/assets/textures/<category>/<name>.png   one file per distinct base image (same pixels
                                                    = same file, whatever package or name it has);
                                                    category from the packages that use it
                                                    (assets.category: personagens/crash,
                                                    fases/L1_E2, objetos, efeitos, interface,
                                                    cinematicas, outros; <kind>/compartilhado when
                                                    several levels or characters share it)
    private/assets/textures/indice.csv              file, kind, size, mips, stored format, uses,
                                                    texture names, packages
    private/assets/textures/textures.json           the same with every use: package, image chunk
                                                    ordinals, header fields and sha1 per level
Names: the texture's own name without the image extension (Crash_V2.bmp -> Crash_V2.png; an
unusual one stays: Crash_V2.bmp1 -> Crash_V2.bmp1.png); a different image with the same name in
the same folder gets the package it comes from: "Crash_V2 (characters_crash_nis_01).png".

Editing: copy the PNG to mods/NAME/textures/<same path>, change it (any size; RGB, RGBA or
palette) and run build. The edit goes to every package and texture that uses that image; mips
are remade from it and sprites cut into their tiles again. DDS images are re-encoded to their DXT
format (Pillow), TGA images keep their palette layout. build writes only the packages that
change, as mods/NAME/rcf/default/<package>, which the runtime serves in place of that entry.
"""
import argparse
import csv
import functools
import hashlib
import io
import json
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import assets  # noqa: E402
import p3d  # noqa: E402

ROOT = assets.ROOT
TEXTURES = assets.ASSETS / "textures"
TABLE = TEXTURES / "textures.json"
TEXTURE, IMAGE, IMAGE_DATA, SPRITE = 0x19000, 0x19001, 0x19002, 0x19005
FORMATS = {1: "png", 2: "tga", 5: "dds", 6: "dds", 7: "dds", 8: "dds", 9: "dds", 10: "dds"}


def sha1(data):
    return hashlib.sha1(data).hexdigest()


def safe(name):
    name = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", name).strip(". ")
    return name or "unnamed"


# --- Chunk fields ---------------------------------------------------------------------------------

def image_fields(chunk):
    name, pos = p3d.pascal(chunk.header, 0)
    version, width, height, bpp, palettized, alpha, fmt = struct.unpack_from("<7I", chunk.header, pos)
    return {"name": name, "version": version, "width": width, "height": height, "bpp": bpp,
            "palettized": palettized, "alpha": alpha, "format": fmt, "_pos": pos}


def set_image_fields(chunk, width, height, bpp, palettized, alpha, fmt):
    f = image_fields(chunk)
    h = bytearray(chunk.header)
    struct.pack_into("<7I", h, f["_pos"], f["version"], width, height, bpp, palettized, alpha, fmt)
    chunk.header = bytes(h)


def image_data(chunk):
    data = chunk.find(IMAGE_DATA)
    if not data:
        return None, None
    d = data[0]
    size = struct.unpack_from("<I", d.header, 0)[0]
    return d, d.header[4:4 + size]


def set_image_data(chunk, payload):
    d, _ = image_data(chunk)
    d.header = struct.pack("<I", len(payload)) + payload


def texture_fields(chunk):
    name, pos = p3d.pascal(chunk.header, 0)
    if chunk.id != TEXTURE or len(chunk.header) < pos + 36:
        return {"name": name}
    v = struct.unpack_from("<9I", chunk.header, pos)
    return {"name": name, "width": v[1], "height": v[2], "bpp": v[3], "alpha_depth": v[4], "mips": v[5],
            "_pos": pos}


def stored_kind(payload, fmt):
    if payload[:8] == b"\x89PNG\r\n\x1a\n":
        return "png"
    if payload[:4] == b"DDS ":
        return "dds"
    if fmt == 2:
        return "tga"
    return "bin"


# Color-mapped TGA with a 32-bit (BGRA) palette, the only TGA kind in the game; Pillow can't read
# 32-bit palettes. Header 18 bytes (id length, colour map type 1, image type 1, map start, map
# length, map entry bits, origin, width, height, 8 bits per pixel, descriptor 0x20 = top-left).
def read_tga(data):
    idlen, cmtype, itype = data[0], data[1], data[2]
    cm_first, cm_len, cm_bits = struct.unpack_from("<HHB", data, 3)
    w, h, bits, desc = struct.unpack_from("<HHBB", data, 12)
    if cmtype != 1 or itype != 1 or cm_bits != 32 or bits != 8:
        return Image.open(io.BytesIO(data))
    pos = 18 + idlen
    pal = data[pos:pos + cm_len * 4]
    pos += cm_len * 4
    rgba = bytearray()
    for i in range(cm_len):
        b, g, r, a = pal[i * 4:i * 4 + 4]
        rgba += bytes((r, g, b, a))
    idx = data[pos:pos + w * h]
    img = Image.frombytes("P", (w, h), bytes(idx))
    img.putpalette(bytes(rgba), rawmode="RGBA")
    img = img.convert("RGBA")
    if not desc & 0x20:
        img = img.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    return img


def write_tga(img, original):
    """`img` as a TGA laid out like `original` (color-mapped, 32-bit palette) when it is one."""
    if original[1] == 1 and original[2] == 1 and original[7] == 32 and original[16] == 8:
        q = img.convert("RGBA").quantize(256, method=Image.Quantize.FASTOCTREE)
        pal = q.getpalette("RGBA") or []
        pal = (pal + [0] * 1024)[:1024]
        header = bytearray(original[:18])
        struct.pack_into("<HH", header, 12, q.width, q.height)
        header[0] = 0
        header[17] = 0x20
        bgra = bytearray()
        for i in range(256):
            r, g, b, a = pal[i * 4:i * 4 + 4]
            bgra += bytes((b, g, r, a))
        return bytes(header) + bytes(bgra) + q.tobytes()
    out = io.BytesIO()
    (img if img.mode in ("RGB", "RGBA") else img.convert("RGBA")).save(out, "TGA", orientation=1)
    return out.getvalue()


def open_stored(payload, kind):
    img = read_tga(payload) if kind == "tga" else Image.open(io.BytesIO(payload))
    img.load()
    return img


def images_of(root):
    """(ordinal, image chunk, owner chunk, level) for every image, in file order."""
    n = 0
    for chunk, parents in root.walk():
        if chunk.id in (TEXTURE, SPRITE):
            for level, img in enumerate(chunk.find(IMAGE)):
                yield n, img, chunk, level
                n += 1


# --- Export -----------------------------------------------------------------------------------------

INDEX_CSV = TEXTURES / "indice.csv"
IMAGE_EXTS = (".bmp", ".png", ".tga", ".dds", ".tif", ".tiff", ".jpg", ".jpeg", ".psd")


def load_table():
    if TABLE.exists():
        table = json.loads(TABLE.read_text(encoding="utf-8"))
        if table.get("version") == 2:
            return table
    return {"version": 2, "files": {}}


def save_table(table):
    TEXTURES.mkdir(parents=True, exist_ok=True)
    tmp = TABLE.with_suffix(".tmp")
    tmp.write_text(json.dumps(table, indent=0, ensure_ascii=False), encoding="utf-8")
    tmp.replace(TABLE)


def to_png_bytes(img):
    out = io.BytesIO()
    img.save(out, "PNG")
    return out.getvalue()


def sprite_fields(chunk):
    """Frontend sprite (0x19005): name, u32 canvas width/height, u8 (0), u32 image width/height,
    u32 tile count, ... The image is cut into power-of-two tiles (the image chunks), row by row."""
    name, pos = p3d.pascal(chunk.header, 0)
    if len(chunk.header) < pos + 21:
        return None
    w, h, count = struct.unpack_from("<3I", chunk.header, pos + 9)
    return {"name": name, "width": w, "height": h, "count": count}


def sprite_layout(width, height, tiles):
    """Image position of each (w, h) tile's top-left pixel, or None. Tiles go row by row; each
    has a 1-pixel border (copied from its neighbours, for bilinear filtering), so the first tile
    starts at -1 and the next one 2 pixels before the previous one ends. A row (or the image)
    ends with the tile whose inside (without the border) reaches the image's edge."""
    positions = []
    x = y = -1
    row_h = None
    for tw, th in tiles:
        if y + 1 >= height:
            return None
        if row_h is not None and th != row_h:
            return None
        positions.append((x, y))
        row_h = th
        if x + tw - 1 >= width:
            x, y, row_h = -1, y + th - 2, None
        else:
            x += tw - 2
    return positions if x == -1 and y + 1 >= height and positions else None


def image_row(n, img, level):
    """Index row of an image chunk and its stored bytes (None if it has no data)."""
    f = image_fields(img)
    _d, payload = image_data(img)
    if payload is None:
        return None, None
    return {"n": n, "level": level, "image": f["name"], "width": f["width"], "height": f["height"],
            "bpp": f["bpp"], "palettized": f["palettized"], "alpha": f["alpha"], "format": f["format"],
            "kind": stored_kind(payload, f["format"]), "sha1": sha1(payload)}, payload


def pixel_key(img):
    rgba = img.convert("RGBA")
    return sha1(f"{rgba.width}x{rgba.height}:".encode() + rgba.tobytes())


def package_items(key, data):
    """(content key, kind, display name, use, PIL image) for every
    texture and sprite of a package. Textures (and the tiles of sprites that can't be stitched)
    are keyed by the pixels of their base level, sprites by the stitched image."""
    root = p3d.parse(data)
    groups = defaultdict(list)
    owners = {}
    for n, img, owner, level in images_of(root):
        groups[id(owner)].append((n, img, level))
        owners[id(owner)] = owner
    for oid, images in groups.items():
        owner = owners[oid]
        rows = []
        for n, img, level in images:
            row, payload = image_row(n, img, level)
            if row:
                rows.append((row, payload))
        if not rows:
            continue
        if owner.id == SPRITE:
            info = sprite_fields(owner)
            positions = None
            if info and len(rows) == len(images):
                positions = sprite_layout(info["width"], info["height"], [(r["width"], r["height"]) for r, _p in rows])
            if positions:
                canvas = Image.new("RGBA", (info["width"], info["height"]))
                for (r, payload), (x, y) in zip(rows, positions):
                    canvas.paste(open_stored(payload, r["kind"]).convert("RGBA"), (x, y))
                use = {"package": key, "name": info["name"], "width": info["width"], "height": info["height"],
                       "positions": positions, "images": [r for r, _p in rows]}
                yield "sprite:" + pixel_key(canvas), "sprite", info["name"], use, canvas
                continue
            for r, payload in rows:  # unstitchable sprite: each tile on its own
                img = open_stored(payload, r["kind"])
                use = {"package": key, "name": r["image"], "owner": "sprite", "images": [r]}
                yield "image:" + pixel_key(img), "imagem", r["image"], use, img
            continue
        base_row, base = min(rows, key=lambda rp: rp[0]["level"])
        if base_row["level"] != 0:
            continue
        try:
            img = open_stored(base, base_row["kind"])
        except Exception as e:  # noqa: BLE001
            print(f"AVISO: {key} {owner.name()}: {e}")
            continue
        use = {"package": key, "name": owner.name(), "owner": "texture", "images": [r for r, _p in rows]}
        yield "image:" + pixel_key(img), "textura", owner.name(), use, img


def file_bytes(kind, use):
    """The exported file of a content, from its first use: the stored PNG itself, or a PNG made
    from the DDS/TGA, or the stitched sprite."""
    stored = original_payloads(use["package"])
    if kind == "sprite":
        canvas = Image.new("RGBA", (use["width"], use["height"]))
        for row, (x, y) in zip(use["images"], use["positions"]):
            canvas.paste(open_stored(stored[row["n"]], row["kind"]).convert("RGBA"), (x, y))
        return to_png_bytes(canvas)
    row = use["images"][0]
    payload = stored[row["n"]]
    return payload if row["kind"] == "png" else to_png_bytes(open_stored(payload, row["kind"]))


def thumbnail(img):
    return np.asarray(img.convert("RGBA").resize((16, 16), Image.Resampling.BOX), np.float32)


def content_pixels(c):
    return np.asarray(Image.open(io.BytesIO(file_bytes(c["type"], c["uses"][0]))).convert("RGBA"), np.int16)


def same_image(a, b):
    """Two RGBA arrays of the same size that show the same picture: the differences are the
    noise of a 256-colour (palette) copy or a recompression, not an edit."""
    d = np.abs(a - b)
    rgb = d[..., :3].max(2)
    return d[..., :3].mean() <= 3.5 and d[..., 3].mean() <= 3.5 and np.percentile(rgb, 99) <= 40


def merge_variants(contents):
    """Merges the contents that are the same picture stored in different ways (the game keeps a
    256-colour copy of most character textures for gameplay and a full-colour one for the
    cutscenes, sometimes under another name: a jacked monster's skin is "crash_normal" in
    Characters_Crash_<monster> and C_<monster> in the cutscenes). The merged file is exported from
    the best copy (full colour first) and an edit goes to every copy, each re-encoded in its own
    format. Returns the number of contents merged into another."""
    by_size = defaultdict(list)
    for ck, c in contents.items():
        if c["type"] != "sprite":
            by_size[(c["width"], c["height"])].append(ck)
    parent = {}

    def find(x):
        while parent.get(x, x) != x:
            x = parent[x]
        return x

    pixels = {}
    for keys in by_size.values():
        if len(keys) < 2:
            continue
        thumbs = np.stack([contents[k]["thumb"] for k in keys])
        for i, a in enumerate(keys[:-1]):
            close = np.abs(thumbs[i + 1:] - thumbs[i]).mean(axis=(1, 2, 3))
            for j in np.nonzero(close < 4)[0]:
                b = keys[i + 1 + j]
                if find(a) == find(b):
                    continue
                for k in (a, b):
                    if k not in pixels:
                        pixels[k] = content_pixels(contents[k])
                if same_image(pixels[a], pixels[b]):
                    parent[find(b)] = find(a)
    clusters = defaultdict(set)
    for ck in parent:
        clusters[find(ck)].add(ck)
    merged = 0
    for root, members in clusters.items():
        members = sorted(members | {root})

        def quality(ck):
            first = contents[ck]["uses"][0]["images"][0]
            return first["palettized"] == 0, first["kind"] == "png", len(contents[ck]["uses"]), ck
        best = max(members, key=quality)
        c = contents[best]
        for ck in members:
            if ck == best:
                continue
            other = contents.pop(ck)
            c["uses"] += other["uses"]
            for name, n in other["names"].items():
                c["names"][name] += n
            merged += 1
        c["variants"] = len(members)
    return merged


def display_stem(name):
    stem = name
    if Path(stem).suffix.lower() in IMAGE_EXTS:
        stem = stem[: -len(Path(stem).suffix)]
    return safe(stem)


def package_rank(key):
    archive, entry = key.split("/", 1)
    return assets.category(archive, entry)[1], len(entry), assets.readable(archive, entry).lower()


def assign_paths(contents):
    """{content key: relative path}: category folder + readable, unique name."""
    by_folder = defaultdict(list)
    for ck, c in contents.items():
        packages = sorted({u["package"] for u in c["uses"]}, key=package_rank)
        c["owner"] = packages[0]
        by_folder[assets.pick_category(packages)].append(ck)
    out = {}
    for folder, keys in by_folder.items():
        def order(ck):
            c = contents[ck]
            # The most used variant of a name keeps the plain name.
            return (-len(c["uses"]),) + package_rank(c["owner"]) + (c["name"].lower(), ck)
        taken = {}  # lowercase stem -> sizes of the variants already named after it
        for ck in sorted(keys, key=order):
            c = contents[ck]
            base = display_stem(c["name"])
            size = (c["width"], c["height"])
            stem = base
            if base.lower() in taken:
                # Another image with the same name: tell them apart by size, else by package.
                archive, entry = c["owner"].split("/", 1)
                pkg = safe(Path(assets.readable(archive, entry)).stem)
                options = ([f"{base} ({size[0]}x{size[1]})"] if size not in taken[base.lower()] else []) + \
                    [f"{base} ({pkg})", f"{base} ({pkg} {size[0]}x{size[1]})"]
                stem = next((o for o in options if o.lower() not in taken), None)
                k = 2
                while stem is None or stem.lower() in taken:
                    stem, k = f"{base} ({pkg} {k})", k + 1
            taken.setdefault(base.lower(), set()).add(size)
            taken.setdefault(stem.lower(), set()).add(size)
            out[ck] = f"{folder}/{stem}.png"
    return out


def write_index_csv(table):
    with open(INDEX_CSV, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["arquivo", "tipo", "largura", "altura", "mips", "formato", "usos", "versoes", "nomes", "pacotes"])
        for rel, e in sorted(table["files"].items()):
            names = sorted({u["name"] for u in e["uses"]}, key=str.lower)
            packages = sorted({assets.readable(*u["package"].split("/", 1)) for u in e["uses"]}, key=str.lower)
            mips = max(len(u["images"]) for u in e["uses"]) if e["type"] == "textura" else ""
            kinds = sorted({u["images"][0]["kind"] + (" 256 cores" if u["images"][0]["palettized"] else "")
                            for u in e["uses"]})
            w.writerow([rel, e["type"], e["width"], e["height"], mips, "/".join(kinds), len(e["uses"]),
                        e.get("variants", 1), ";".join(names), ";".join(packages)])


def cmd_export(_args):
    index = assets.load_index()
    entries = sorted((e for e in index["entries"].values() if e["type"] == "pure3d"), key=lambda e: e["entry"])
    contents = {}
    images = levels = 0
    for i, e in enumerate(entries):
        key = f"{e['archive']}/{e['entry']}"
        for ck, kind, name, use, img in package_items(key, assets.raw_path(e["archive"], e["entry"]).read_bytes()):
            images += 1
            levels += len(use["images"])
            c = contents.get(ck)
            if c is None:
                c = contents[ck] = {"type": kind, "names": defaultdict(int), "uses": [],
                                    "width": img.width, "height": img.height, "thumb": thumbnail(img)}
            c["names"][name] += 1
            c["uses"].append(use)
        if (i + 1) % 300 == 0:
            print(f"  {i + 1}/{len(entries)} pacotes...")
    variants = merge_variants(contents)
    for c in contents.values():
        c["name"] = max(c["names"].items(), key=lambda kv: (kv[1], -len(kv[0])))[0]
    paths = assign_paths(contents)
    table = {"version": 2, "files": {}}
    written = 0
    by_package = defaultdict(list)
    for ck, rel in sorted(paths.items(), key=lambda kv: kv[1]):
        c = contents[ck]
        data = file_bytes(c["type"], c["uses"][0])
        out = TEXTURES / rel
        if not out.exists() or out.read_bytes() != data:
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(data)
            written += 1
        table["files"][rel] = {"type": c["type"], "name": c["name"], "width": c["width"], "height": c["height"],
                               "sha1": sha1(data), "variants": c.get("variants", 1), "uses": c["uses"]}
        for u in c["uses"]:
            by_package[u["package"]].append(f"textures/{rel}")
    save_table(table)
    write_index_csv(table)
    for key, outputs in by_package.items():
        assets.record_converted(index, *key.split("/", 1), sorted(set(outputs)), replace_prefix="textures/")
    assets.save_index(index)
    known = {TEXTURES / rel for rel in table["files"]}
    extra = [p for p in TEXTURES.rglob("*.png") if p not in known]
    kinds = defaultdict(int)
    for e in table["files"].values():
        kinds[e["type"]] += 1
    print(f"{images} texturas/sprites em {len(by_package)} pacotes ({levels} imagens contando mips e tiles) -> "
          f"{len(table['files'])} arquivos ({', '.join(f'{n} {k}' for k, n in sorted(kinds.items()))}; "
          f"{variants} cópia(s) da mesma imagem em outro formato juntadas); {written} gravado(s); "
          f"índice: {INDEX_CSV.relative_to(ROOT)}")
    if extra:
        print(f"AVISO: {len(extra)} PNG em {TEXTURES.relative_to(ROOT)} fora do índice (de uma exportação "
              f"anterior?), ex.: {extra[0].relative_to(TEXTURES)}")
    return 0


def texture_files():
    """{texture name (lowercase): exported PNG path} (for tools/models.py)."""
    out = {}
    for rel, e in load_table()["files"].items():
        if e["type"] == "textura":
            for u in e["uses"]:
                out.setdefault(u["name"].lower(), TEXTURES / rel)
    return out


def cmd_find(args):
    pattern = args.text.lower()
    n = 0
    for rel, e in sorted(load_table()["files"].items()):
        names = {u["name"] for u in e["uses"]}
        packages = {assets.readable(*u["package"].split("/", 1)) for u in e["uses"]}
        if pattern in rel.lower() or any(pattern in x.lower() for x in names | packages):
            print(f"{e['width']:5}x{e['height']:<5} {e['type']:7} {len(e['uses']):3} uso(s)  textures/{rel}")
            n += 1
    print(f"{n} arquivo(s)")
    return 0


# --- Build --------------------------------------------------------------------------------------------

def png_header(data):
    """(bpp, palettized, has alpha) of a PNG, or None if it must be re-encoded."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    depth, ctype = data[24], data[25]
    if depth != 8 and not (ctype == 3 and depth in (1, 2, 4, 8)):
        return None
    if ctype == 2:
        return 24, 0, 0
    if ctype == 6:
        return 32, 0, 1
    if ctype == 3:
        return depth, 1, 1 if b"tRNS" in data[:data.find(b"IDAT")] else 0
    return None


def png_payload(img):
    """PNG bytes the game can load and their (bpp, palettized, alpha)."""
    if img.mode not in ("RGB", "RGBA", "P"):
        img = img.convert("RGBA")
    data = to_png_bytes(img)
    return data, png_header(data)


def png_like(row, img):
    """PNG bytes for an image stored as `row`: a 256-colour copy stays 256-colour (with its
    transparency in the palette), a full-colour one stays full colour."""
    if row["palettized"] and img.mode != "P":
        rgba = img.convert("RGBA")
        if rgba.getextrema()[3][0] < 255:
            img = rgba.quantize(256, method=Image.Quantize.FASTOCTREE)
        else:
            img = rgba.convert("RGB").quantize(256)
    elif not row["palettized"] and img.mode == "P":
        img = img.convert("RGBA")
    return png_payload(img)


# Folders searched for edited files (cmd_build sets it: --src folders, then the mod's own
# textures folder), each laid out like private/assets/textures.
SOURCES = []


def user_file(rel):
    for src in SOURCES:
        if (src / rel).exists():
            return src / rel
    return None


@functools.lru_cache(maxsize=32)
def original_payloads(key):
    """{image ordinal: stored bytes} of a package as in the game (not the possibly edited export)."""
    archive, entry = key.split("/", 1)
    root = p3d.parse(assets.raw_path(archive, entry).read_bytes())
    return {n: image_data(img)[1] for n, img, _o, _l in images_of(root)}


_ENCODED = {}


def encode_like(key, row, img, cache_id=None):
    """New stored bytes for `row` from a PIL image, in the row's original kind, and the header's
    (bpp, palettized, alpha). `cache_id` identifies `img` to reuse the encoding for other packages."""
    original = original_payloads(key)[row["n"]] if row["kind"] in ("dds", "tga") else b""
    sig = original[84:88] if row["kind"] == "dds" else original[:18] if row["kind"] == "tga" else row["palettized"]
    ck = (cache_id, row["kind"], sig) if cache_id else None
    if ck and ck in _ENCODED:
        return _ENCODED[ck]
    if row["kind"] == "dds":
        fourcc = original[84:88].decode("latin-1") if original[:4] == b"DDS " else None
        out = io.BytesIO()
        img.convert("RGBA").save(out, "DDS", pixel_format=fourcc if fourcc in ("DXT1", "DXT3", "DXT5") else "DXT5")
        result = out.getvalue(), (row["bpp"], row["palettized"], 1 if img.mode in ("RGBA", "LA", "P") else row["alpha"])
    elif row["kind"] == "tga":
        result = write_tga(img, original), (row["bpp"], row["palettized"], row["alpha"])
    else:
        result = png_like(row, img)
    if ck:
        _ENCODED[ck] = result
    return result


def mip_image(base, level):
    """Mip level `level` of an edited base image (encode_like puts it in the level's format)."""
    w, h = max(1, base.width >> level), max(1, base.height >> level)
    src = base if base.mode in ("RGB", "RGBA") else base.convert("RGBA")
    return src.resize((w, h), Image.Resampling.LANCZOS)


# --- 256-colour textures and their palette chunks ---------------------------------------------------
#
# A 256-colour texture (a colour-mapped TGA or a palettized PNG) is drawn with the palette chunk
# of the same name (0x19006 + 0x19007: 256 x B,G,R,A), not with the file's own palette; other
# palette chunks recolour the same indices (Crash_Player2.bmp for crash_normal.bmp, the
# *_jacked.bmp of the monsters, the animated x.tga.1..3). An edited 256-colour texture gets a new
# palette, written to the file and to its palette chunk; every mip level uses that palette; the
# other palettes of that texture are remapped so that each new index takes the colour the old
# palette had at the same place of the image.

PALETTE, PALETTE_DATA = 0x19006, 0x19007


def stored_indices(payload, kind):
    """(indices H x W uint8, palette 256 x 4 RGBA int) of a 256-colour TGA/PNG, or None."""
    if kind == "tga" and payload[1] == 1 and payload[2] == 1 and payload[7] == 32 and payload[16] == 8:
        cm_len = struct.unpack_from("<H", payload, 5)[0]
        w, h = struct.unpack_from("<HH", payload, 12)
        pos = 18 + payload[0]
        pal = np.zeros((256, 4), np.int32)
        pal[:min(cm_len, 256)] = np.frombuffer(payload, np.uint8, min(cm_len, 256) * 4, pos).reshape(-1, 4)[:, [2, 1, 0, 3]]
        idx = np.frombuffer(payload, np.uint8, w * h, pos + cm_len * 4).reshape(h, w)
        return (idx if payload[17] & 0x20 else idx[::-1]).copy(), pal
    if kind == "png":
        img = Image.open(io.BytesIO(payload))
        if img.mode != "P":
            return None
        rgb = (img.getpalette() or []) + [0] * 768
        pal = np.full((256, 4), 255, np.int32)
        pal[:, :3] = np.array(rgb[:768]).reshape(256, 3)
        t = img.info.get("transparency")
        if isinstance(t, bytes):
            pal[:len(t), 3] = np.frombuffer(t, np.uint8)
        elif isinstance(t, int):
            pal[t, 3] = 0
        return np.asarray(img, np.uint8).copy(), pal
    return None


def nearest_indices(rgba, pal):
    """Index of the nearest palette entry (RGBA distance) for every pixel of an H x W x 4 array."""
    flat = rgba.reshape(-1, 4).astype(np.int32)
    out = np.empty(len(flat), np.uint8)
    for s in range(0, len(flat), 8192):
        d = ((flat[s:s + 8192, None, :] - pal[None, :, :]) ** 2).sum(2)
        out[s:s + 8192] = d.argmin(1)
    return out.reshape(rgba.shape[:2])


_PALETTES = {}


def new_palette(cache_id, img):
    """(palette 256 x 4 RGBA, base indices) for an edited image (cached per edited file)."""
    if cache_id not in _PALETTES:
        rgba = img.convert("RGBA")
        if rgba.getextrema()[3][0] < 255:
            q = rgba.quantize(256, method=Image.Quantize.FASTOCTREE)
            flat = q.getpalette("RGBA") or []
        else:
            q = rgba.convert("RGB").quantize(256, method=Image.Quantize.MEDIANCUT)
            flat = [v for i in range(0, len(q.getpalette() or []), 3) for v in q.getpalette()[i:i + 3] + [255]]
        pal = np.zeros((256, 4), np.int32)
        pal[:, 3] = 255
        used = min(256, len(flat) // 4)
        pal[:used] = np.array(flat[:used * 4]).reshape(used, 4)
        _PALETTES[cache_id] = pal, np.asarray(q, np.uint8).copy()
    return _PALETTES[cache_id]


def palette_payload(row, indices, pal, original):
    """Stored bytes of a 256-colour image (the row's kind) and its header fields."""
    h, w = indices.shape
    if row["kind"] == "tga":
        header = bytearray(original[:18])
        header[0] = 0
        struct.pack_into("<HHB", header, 3, 0, 256, 32)
        struct.pack_into("<HH", header, 12, w, h)
        header[16], header[17] = 8, 0x20
        return bytes(header) + pal[:, [2, 1, 0, 3]].astype(np.uint8).tobytes() + indices.tobytes(), \
            (row["bpp"], row["palettized"], row["alpha"])
    img = Image.frombytes("P", (w, h), indices.tobytes())
    img.putpalette(pal[:, :3].astype(np.uint8).tobytes())
    out = io.BytesIO()
    alpha = pal[:, 3].astype(np.uint8).tobytes()
    if alpha != b"\xff" * 256:
        img.save(out, "PNG", transparency=alpha)
    else:
        img.save(out, "PNG")
    data = out.getvalue()
    return data, png_header(data)


def roughness(rgb):
    return float(np.abs(np.diff(rgb, axis=0)).mean() + np.abs(np.diff(rgb, axis=1)).mean())


def remap_palette(alt, edit):
    """New entries of another palette of an edited texture: each new index takes the colour this
    palette had where that index is now used (the most common old index under it)."""
    new_idx, old_idx = edit["indices"], edit["orig_indices"]
    if old_idx.shape != new_idx.shape:
        old_idx = np.asarray(Image.fromarray(old_idx).resize(new_idx.shape[::-1], Image.Resampling.NEAREST))
    counts = np.bincount(new_idx.astype(np.int64).ravel() * 256 + old_idx.ravel(), minlength=65536).reshape(256, 256)
    out = alt[counts.argmax(1)]
    unused = counts.sum(1) == 0
    out[unused] = edit["palette"][unused]
    return out


def patch_palettes(root, palettes):
    """Writes the new palettes of the edited 256-colour textures of a package (`palettes`:
    {texture name (lowercase): edit}) into its palette chunks; the other palettes of those
    textures (recognised as the ones that make the texture's old indices a smooth picture, like
    its own palette does) are remapped. Returns the number of palette chunks changed."""
    textures_here = {c.name().lower() for c in root.children if c.id == TEXTURE}
    changed = 0
    for c in root.children:
        if c.id != PALETTE:
            continue
        data = c.find(PALETTE_DATA)
        if not data or len(data[0].header) != 1024:
            continue
        name = c.name().lower()
        old = np.frombuffer(data[0].header, np.uint8).reshape(256, 4)[:, [2, 1, 0, 3]].astype(np.int32)
        if name in palettes:
            new = palettes[name]["palette"]
        elif name in textures_here:
            continue  # the palette of a texture that wasn't edited
        else:
            best = None
            for edit in palettes.values():
                own = roughness(edit["orig_palette"][edit["orig_indices"]][..., :3])
                this = roughness(old[edit["orig_indices"]][..., :3])
                if this <= own * 1.6 + 2 and (best is None or this / max(own, 1e-6) < best[0]):
                    best = (this / max(own, 1e-6), edit)
            if best is None:
                continue
            new = remap_palette(old, best[1])
        data[0].header = new[:, [2, 1, 0, 3]].astype(np.uint8).tobytes()
        changed += 1
    return changed


def texture_edits(rel, use, img, data):
    """({image ordinal: (bytes, fields, size)}, palette edit or None) for a texture use: the
    edited base level and every mip level remade from it."""
    out = {}
    mips = {}
    first = use["images"][0]
    stored = original_payloads(use["package"])
    orig = stored_indices(stored[first["n"]], first["kind"]) if first["palettized"] else None
    if orig is not None and use.get("owner") == "texture":
        # 256 colours: one new palette for every level, also written to the palette chunks.
        pal, base = new_palette(rel, img)
        for row in use["images"]:
            level = row["level"]
            if level == 0:
                idx, size = base, img.size
            else:
                mip = mip_image(img, level)
                idx, size = nearest_indices(np.asarray(mip.convert("RGBA"), np.int32), pal), mip.size
            payload, fields = palette_payload(row, idx, pal, stored[row["n"]])
            out[row["n"]] = (payload, fields, size)
        return out, {"palette": pal, "indices": base, "orig_indices": orig[0], "orig_palette": orig[1]}
    for row in use["images"]:
        level = row["level"]
        header = png_header(data) if level == 0 and row["kind"] == "png" else None
        if header and bool(header[1]) == bool(row["palettized"]):
            out[row["n"]] = (data, header, img.size)  # the edited PNG as is
            continue
        if level not in mips:
            mips[level] = img if level == 0 else mip_image(img, level)
        payload, fields = encode_like(use["package"], row, mips[level], cache_id=(rel, level))
        out[row["n"]] = (payload, fields, mips[level].size)
    return out, None


def sprite_edits(use, new):
    """{image ordinal: (bytes, fields, size)} for the tiles of a sprite that `new` changes."""
    out = {}
    width, height = new.size
    for row, (x, y) in zip(use["images"], use["positions"]):
        tile = open_stored(original_payloads(use["package"])[row["n"]], row["kind"]).convert("RGBA")
        before = tile.tobytes()
        # Only the pixels inside the sprite change; the border past its edges keeps the tile's own.
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(width, x + tile.width), min(height, y + tile.height)
        if x1 > x0 and y1 > y0:
            tile.paste(new.crop((x0, y0, x1, y1)), (x0 - x, y0 - y))
        if tile.tobytes() == before:
            continue
        payload, fields = encode_like(use["package"], row, tile)
        out[row["n"]] = (payload, fields, tile.size)
    return out


def collect_edits(table):
    """({package key: {image ordinal: (bytes, fields, size)}}, {package key: {texture name:
    palette edit}}, files) for every edited file."""
    edits = defaultdict(dict)
    palettes = defaultdict(dict)
    files = 0
    for rel, e in sorted(table["files"].items()):
        path = user_file(rel)
        if path is None:
            continue
        data = path.read_bytes()
        if sha1(data) == e["sha1"]:
            continue
        img = Image.open(io.BytesIO(data))
        img.load()
        files += 1
        if e["type"] == "sprite":
            new = img.convert("RGBA")
            if new.size != (e["width"], e["height"]):
                print(f"AVISO: {rel}: {new.size[0]}x{new.size[1]}, o sprite é {e['width']}x{e['height']}; "
                      "redimensionando")
                new = new.resize((e["width"], e["height"]), Image.Resampling.LANCZOS)
            for use in e["uses"]:
                edits[use["package"]].update(sprite_edits(use, new))
        else:
            for use in e["uses"]:
                images, palette = texture_edits(rel, use, img, data)
                edits[use["package"]].update(images)
                if palette:
                    palettes[use["package"]][use["name"].lower()] = palette
        print(f"  {rel}: {len(e['uses'])} uso(s) em {len({u['package'] for u in e['uses']})} pacote(s)")
    return edits, palettes, files


def patch_package(data, edits, palettes=None):
    """New package bytes with `edits` ({image ordinal: (bytes, fields, size)}) and `palettes`
    (see patch_palettes) applied, or None."""
    root = p3d.parse(data)
    changed = bool(palettes) and patch_palettes(root, palettes) > 0
    for n, img, owner, level in list(images_of(root)):
        e = edits.get(n)
        if not e:
            continue
        new, (bpp, pal, alpha), (w, h) = e
        f = image_fields(img)
        set_image_data(img, new)
        set_image_fields(img, w, h, bpp, pal, alpha, f["format"])
        if level == 0 and owner.id == TEXTURE:
            t = texture_fields(owner)
            if "_pos" in t:
                hdr = bytearray(owner.header)
                struct.pack_into("<3I", hdr, t["_pos"] + 4, w, h, bpp)
                owner.header = bytes(hdr)
        changed = True
    return root.serialize() if changed else None


def cmd_build(args):
    table = load_table()
    if not table["files"]:
        print("rode `python tools/textures.py export` primeiro")
        return 1
    mod = Path(args.mods_dir) / args.mod
    SOURCES[:] = [Path(x) for x in (args.src or [])] + [mod / "textures"]
    print("procurando imagens editadas em: " + ", ".join(str(x) for x in SOURCES))
    edits, palettes, files = collect_edits(table)
    if not edits:
        print("nenhuma imagem editada (copie de private/assets/textures, mesmo caminho, e edite)")
        return 0
    written = 0
    for key, package_edits in sorted(edits.items()):
        archive, entry = key.split("/", 1)
        data = assets.raw_path(archive, entry).read_bytes()
        new = patch_package(data, package_edits, palettes.get(key))
        if new is None:
            continue
        target = mod / "rcf" / archive / Path(*entry.split("\\"))
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(new)
        written += 1
        print(f"  {assets.readable(archive, entry)} ({entry}): {len(package_edits)} imagem(ns), "
              f"{len(data)} -> {len(new)} bytes")
    print(f"{files} arquivo(s) editado(s) -> {written} pacote(s) em {mod / 'rcf'}")
    return 0


def cmd_verify(_args):
    """Every package parses and re-serializes byte for byte; every exported file is there with
    the indexed bytes; the indexed image chunks match the packages; a no-op patch (each image
    replaced by itself) gives the original package."""
    table = load_table()
    uses = defaultdict(list)
    bad = 0
    for rel, e in table["files"].items():
        path = TEXTURES / rel
        if not path.exists() or sha1(path.read_bytes()) != e["sha1"]:
            bad += 1
            print(f"ARQUIVO DIFERENTE: {rel}")
        for u in e["uses"]:
            uses[u["package"]].extend(u["images"])
    checked = 0
    for key, rows in sorted(uses.items()):
        archive, entry = key.split("/", 1)
        data = assets.raw_path(archive, entry).read_bytes()
        root = p3d.parse(data)
        ok = root.serialize() == data
        stored = {n: (image_data(img)[1], image_fields(img)) for n, img, _o, _l in images_of(root)}
        identity = {}
        for r in rows:
            payload, f = stored.get(r["n"], (None, None))
            ok &= payload is not None and sha1(payload) == r["sha1"]
            if payload is not None:
                identity[r["n"]] = (payload, (f["bpp"], f["palettized"], f["alpha"]), (f["width"], f["height"]))
        ok &= patch_package(data, identity) == data
        checked += 1
        if not ok:
            bad += 1
            print(f"DIFERENTE: {key}")
    print(f"{len(table['files'])} arquivos e {checked} pacotes conferidos, {bad} diferença(s)")
    return 1 if bad else 0


def main(argv):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("export")
    s = sub.add_parser("find")
    s.add_argument("text")
    s = sub.add_parser("build")
    s.add_argument("--mod", required=True)
    s.add_argument("--mods-dir", default=str(ROOT / "dist" / "mods"))
    s.add_argument("--src", action="append", help="outra pasta com imagens editadas (mesma estrutura)")
    sub.add_parser("verify")
    args = p.parse_args(argv)
    return {"export": cmd_export, "find": cmd_find, "build": cmd_build, "verify": cmd_verify}[args.cmd](args)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main(sys.argv[1:]))
