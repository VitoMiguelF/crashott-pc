"""The multiplayer colours on Crash's skins (runtime/online/mp_colors.cpp; docs/internals/multiplayer.md,
"Colours").

    python tools/mp_skin_colours.py table OUT.inc      # the table mp_colors.cpp includes (the build runs it)
    python tools/mp_skin_colours.py preview OUT.png    # every skin's costume through the 12 colours

Each skin package (package\\Characters_Crash_<skin>.p3d) has its own palettes (chunk 0x19006, 256 BGRA
entries: crash_normal.bmp, Crash_Player2.bmp) and a 512x512 costume texture (crash_normal.bmp) whose
bottom right holds Crash's shorts, at the same place in every skin. For each skin the table says which
palette entries are the costume's main colour: the hue that covers most of the costume's pixels
(outside the shorts), the entries within 40 degrees of it that the costume uses at least twice as much
as the shorts (so the jeans stay blue even on a blue costume), or, for a costume without a colour
(Skeleton), its light entries. It also keeps the costume's mean saturation and bright value, which
the recolour normalises to the normal Crash's fur, and 16 sample entries (every 16th) by which the
game tells the skin from the palette it draws with. The recipes are read from mp_colors.cpp.

Reads private/assets (python tools/assets.py extract first) and needs Pillow and NumPy. The table is
game data: CMake writes it into the build folder (generated_runtime/), never into the repository.
Without the extracted data or the modules (the GitHub builds) `table` writes an empty table, and the
game paints the skins with the fur test only (mp_colors.cpp).
"""
import colorsys
import csv
import io
import math
import re
import sys
from pathlib import Path

try:
    import numpy as np
    from PIL import Image, ImageDraw, ImageFont, ImagePalette
except ImportError as missing:
    np = None
    MISSING = str(missing)

sys.path.insert(0, str(Path(__file__).resolve().parent))
import p3d  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "private" / "assets"
SOURCE = ROOT / "runtime" / "online" / "mp_colors.cpp"
SHORTS_FROM = ["Crash_Spike", "Crash_Roller", "Crash_Shellephant", "Crash_Battler", "Crash_Parafox"]  # orange costumes
MASK_HUE = 40.0      # degrees around the costume's hue
MIN_SHARE = 0.25     # below this share of saturated costume pixels the costume is colourised instead
FUR_SAT = 0.8        # the normal Crash's fur: its mean saturation


def palettes(root):
    out = {}
    for c, _ in root.walk():
        if c.id == 0x19006:
            name, _ = p3d.pascal(c.header, 0)
            raw = c.children[0].header
            out[name] = [(raw[i * 4 + 2], raw[i * 4 + 1], raw[i * 4]) for i in range(256)]
    return out


def costume(root):
    best = None
    for c, _ in root.walk():
        if c.id != 0x19000 or p3d.pascal(c.header, 0)[0].lower() != "crash_normal.bmp":
            continue
        try:
            im = Image.open(io.BytesIO(c.children[0].children[0].header[4:]))
        except Exception:
            continue
        if im.mode != "P":
            continue
        im.palette = ImagePalette.ImagePalette("RGB", bytes(768))  # its own palette is unused (the game's are 0x19006)
        im.load()
        if best is None or im.size[0] > best.size[0]:
            best = im
    return best


def skins():
    out = {}
    for r in csv.DictReader(open(ASSETS / "index.csv", encoding="utf-8")):
        n = r["name"].lower()
        if n.startswith("package\\characters_crash") and "_nis" not in n:
            skin = r["name"].split("\\")[-1][:-4].replace("Characters_", "")
            root = p3d.parse((ASSETS / "raw" / r["archive"] / r["entry"].replace("\\", "/")).read_bytes())
            pals, tex = palettes(root), costume(root)
            if "crash_normal.bmp" in pals and tex is not None:
                out[skin] = (pals["crash_normal.bmp"], pals.get("Crash_Player2.bmp"), np.array(tex))
    return {k: out[k] for k in sorted(out, key=lambda k: (k != "Crash", k))}


def hsv(c):
    h, s, v = colorsys.rgb_to_hsv(*(x / 255 for x in c))
    return h * 360, s, v


def shorts_mask(all_skins):
    votes = 0
    for name in SHORTS_FROM:
        normal, _, idx = all_skins[name]
        h, w = idx.shape
        lut = np.array([not (5 <= hsv(c)[0] <= 50 and hsv(c)[1] > 0.3) for c in normal])
        m = lut[idx]
        m[: h // 2, :] = False
        m[:, : w // 2] = False
        votes = votes + m
    return votes >= len(SHORTS_FROM) - 1


def analyse(normal, idx, shorts):
    cost = np.bincount(idx[~shorts].ravel(), minlength=256)
    jeans = np.bincount(idx[shorts].ravel(), minlength=256)
    hs = [hsv(c) for c in normal]
    hist = np.zeros(36)
    for k in range(256):
        h, s, v = hs[k]
        if s > 0.25 and v > 0.15:
            hist[int(h / 10) % 36] += cost[k]
    window = [hist[b - 1] + hist[b] + hist[(b + 1) % 36] for b in range(36)]
    peak = int(np.argmax(window))
    share = window[peak] / cost.sum()
    colourize = share < MIN_SHARE
    # the costume's hue: the circular mean of the pixels within 20 degrees of the peak
    x = y = 0.0
    for k in range(256):
        h, s, v = hs[k]
        if s > 0.25 and v > 0.15 and abs((h - (peak * 10 + 5) + 180) % 360 - 180) <= 20:
            x += cost[k] * math.cos(math.radians(h))
            y += cost[k] * math.sin(math.radians(h))
    centre = math.degrees(math.atan2(y, x)) % 360
    mask = []
    for k in range(256):
        h, s, v = hs[k]
        mostly = cost[k] > 0 and cost[k] >= 2 * jeans[k]
        if colourize:
            mask.append(bool(mostly and v > 0.5 and s < 0.3))
        else:
            mask.append(bool(mostly and s > 0.2 and v > 0.06 and abs((h - centre + 180) % 360 - 180) <= MASK_HUE))
    used = [k for k in range(256) if mask[k]]
    weights = np.array([cost[k] for k in used], dtype=float)
    sats = np.array([hs[k][1] for k in used])
    vals = np.array([hs[k][2] for k in used])
    s_mean = float((sats * weights).sum() / max(weights.sum(), 1)) if used else 1.0
    order = np.argsort(vals)
    cum = np.cumsum(weights[order]) / max(weights.sum(), 1)
    v95 = float(vals[order][np.searchsorted(cum, 0.95)]) if used else 1.0
    return dict(centre=centre, s_mean=max(s_mean, 0.05), v95=max(v95, 0.05), colourize=colourize, mask=mask, share=share)


def recipes():
    text = SOURCE.read_text(encoding="utf-8")
    out = []
    for m in re.finditer(r'\{ \{ "(\w+)", "[^"]*", "[^"]*" \}, Kind::(\w+)((?:, [-\d.]+f?)*) \}', text):
        nums = [float(v.rstrip("f")) for v in m.group(3).split(",")[1:]]
        out.append((m.group(1), m.group(2), nums))
    return out


def paint(recipe, info, c):
    """One masked entry of a skin through a recipe (the same arithmetic as mp_colors.cpp RecolourSkin)."""
    hue, spread, sat, vmin, vmax, gamma = recipe
    h, s, v = hsv(c)
    if info["colourize"]:
        nh, ns = hue, min(max(0.55 * sat, 0.0), 1.0)
    else:
        nh = hue + ((h - info["centre"] + 180) % 360 - 180) * spread
        ns = min(max(s / info["s_mean"] * FUR_SAT * sat, 0.0), 1.0)
    vn = min(v / info["v95"], 1.0)
    r, g, b = colorsys.hsv_to_rgb((nh % 360) / 360, ns, vmin + (vmax - vmin) * vn ** gamma)
    return (int(min(max(r * 255 + 0.5, 0), 255)), int(min(max(g * 255 + 0.5, 0), 255)), int(min(max(b * 255 + 0.5, 0), 255)))


HEADER = ["// Generated by tools/mp_skin_colours.py from the skin packages of the user's game (in the build",
          "// folder, never in the repository): which palette entries of each Crash skin are its costume's",
          "// main colour, and how to recolour them (mp_colors.cpp). samples = the palette's entries 0, 16,",
          "// ..., 240 (R, G, B).", ""]


def write_table(out, lines):
    """Writes the table only when it changed (the build recompiles mp_colors.cpp after a write)."""
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    text = "\n".join(HEADER + lines) + "\n"
    if not out.is_file() or out.read_text(encoding="utf-8") != text:
        out.write_text(text, encoding="utf-8")


def cmd_table(all_skins, shorts, out):
    lines = []
    names = [n for n in all_skins if n != "Crash"]
    for name in names:
        normal, _, idx = all_skins[name]
        a = analyse(normal, idx, shorts)
        bits = [sum(1 << b for b in range(8) if a["mask"][i * 8 + b]) for i in range(32)]
        samples = ", ".join("{ %d, %d, %d }" % normal[k] for k in range(0, 256, 16))
        lines.append('{ "%s", { %s },' % (name.replace("Crash_", ""), samples))
        lines.append("  { %s }," % ", ".join("0x%02X" % b for b in bits))
        lines.append("  %.1ff, %.3ff, %.3ff, %s },  // %d entries" % (a["centre"], a["s_mean"], a["v95"],
                                                                     "true" if a["colourize"] else "false", sum(a["mask"])))
    # the samples must tell every palette apart, the normal Crash's included
    for i, x in enumerate(all_skins):
        for y in list(all_skins)[i + 1:]:
            px, py = all_skins[x][0], all_skins[y][0]
            if all(max(abs(p - q) for p, q in zip(px[k], py[k])) <= 4 for k in range(0, 256, 16)):
                sys.exit(f"{x} and {y} have the same samples")
    write_table(out, lines)
    print(f"{out}: {len(names)} skins")


def cmd_preview(all_skins, shorts, out):
    recs = recipes()
    cell = 96
    names = list(all_skins)
    font = ImageFont.truetype("arialbd.ttf", 14)
    sheet = Image.new("RGB", (150 + cell * len(recs), 24 + cell * len(names)), (24, 24, 24))
    d = ImageDraw.Draw(sheet)
    for j, (rid, _, _) in enumerate(recs):
        d.text((150 + j * cell + 4, 4), rid, fill=(230, 230, 230), font=font)
    for i, name in enumerate(names):
        normal, jacked, idx = all_skins[name]
        a = analyse(normal, idx, shorts) if name != "Crash" else None
        y = 24 + i * cell
        d.text((6, y + cell // 2 - 8), name.replace("Crash_", ""), fill=(230, 230, 230), font=font)
        for j, (rid, kind, nums) in enumerate(recs):
            if kind == "Jacked":
                pal = jacked or normal
            elif kind == "Normal":
                pal = normal
            elif a is None:  # the normal Crash: its fur (IsFur / Recolour in mp_colors.cpp)
                pal = []
                for c in normal:
                    h, s, v = hsv(c)
                    if v > 0 and 5 <= h <= 45 and s > (0.3 if v < 0.55 else 0.5):
                        hue, spread, sat, vmin, vmax, gamma = nums
                        r, g, b = colorsys.hsv_to_rgb(((hue + (h - 25) * spread) % 360) / 360, min(s * sat, 1), vmin + (vmax - vmin) * v ** gamma)
                        pal.append((int(r * 255 + 0.5), int(g * 255 + 0.5), int(b * 255 + 0.5)))
                    else:
                        pal.append(c)
            else:
                pal = [paint(nums, a, c) if a["mask"][k] else c for k, c in enumerate(normal)]
            lut = np.array(pal, dtype=np.uint8)
            im = Image.fromarray(lut[idx]).resize((cell - 4, cell - 4), Image.LANCZOS)
            sheet.paste(im, (150 + j * cell + 2, y + 2))
    sheet.save(out)
    print(out)


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("table", "preview"):
        sys.exit(__doc__)
    command, out = sys.argv[1], sys.argv[2]
    reason = ("no " + MISSING) if np is None else None if (ASSETS / "index.csv").is_file() else "no private/assets (tools/assets.py extract)"
    all_skins = None
    if reason is None:
        try:
            all_skins = skins()
            missing = [name for name in SHORTS_FROM + ["Crash"] if name not in all_skins]
            if missing:
                reason = "skin packages missing: " + ", ".join(missing)
        except Exception as error:  # an incomplete extraction
            reason = f"the skin packages could not be read ({error!r})"
    if reason:
        if command == "preview":
            sys.exit(reason)
        write_table(out, ["// empty: " + reason])
        print(f"{out}: empty ({reason}); the skins keep the fur test")
        return
    shorts = shorts_mask(all_skins)
    if command == "table":
        cmd_table(all_skins, shorts, out)
    else:
        cmd_preview(all_skins, shorts, out)


if __name__ == "__main__":
    main()
