"""Extracts every entry of every .rcf of the game and keeps an index of what was extracted/converted.

    python tools/assets.py extract [--archive NAME ...] [--game DIR]   # raw files + index
    python tools/assets.py index                                       # rebuild the index only
    python tools/assets.py status                                      # counts per archive/type
    python tools/assets.py verify [--sample N]                         # raw files == archive bytes
    python tools/assets.py all                                         # extract + every converter

Output (game data: never publish it):
    private/assets/raw/<archive>/<entry path>     the entry bytes, exactly as stored in the .rcf
    private/assets/index.json, index.csv           one row per entry: archive, entry, original
                                                   name (from hashdictionary.txt), size, type,
                                                   offset, hash and the converted files
The converters (tools/audio.py, tools/textures.py, tools/models.py) add their outputs to the
"converted" column through record_converted(). Extraction is incremental: an entry whose raw file
already exists with the right size is skipped.
"""
import argparse
import csv
import json
import os
import re
import struct
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rcf  # noqa: E402
import buildenv  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_GAME = buildenv.game_folder()  # config/local.toml or the player's settings.toml
ASSETS = ROOT / "private" / "assets"
RAW = ASSETS / "raw"
INDEX_JSON = ASSETS / "index.json"
INDEX_CSV = ASSETS / "index.csv"
ARCHIVES = ["default", "english", "french", "german", "spanish", "italian", "dutch", "sound1", "sound2"]
LANGUAGES = ["english", "french", "german", "spanish", "italian", "dutch"]

TYPES = {
    ".p3d": "pure3d", ".rsd": "audio", ".bik": "video", ".lua": "lua", ".blua": "lua-bytecode",
    ".bfig": "fighttree", ".dtr": "datatree", ".txt": "text",
}


ASSETTOOL_EXE = ROOT / "build" / "assettool" / "crash_assettool.exe"


def build_assettool(force=False):
    """Configures/builds tools/assettool (C++, FFmpeg) with the toolchain of tools/buildenv.py."""
    import subprocess
    sources = list((ROOT / "tools" / "assettool").glob("*.*"))
    if not force and ASSETTOOL_EXE.exists() and \
            all(f.stat().st_mtime <= ASSETTOOL_EXE.stat().st_mtime for f in sources):
        return ASSETTOOL_EXE
    out = ASSETTOOL_EXE.parent
    out.mkdir(parents=True, exist_ok=True)
    src = ROOT / "tools" / "assettool"
    script = out / "build.cmd"
    script.write_text("\r\n".join([
        "@echo off",
        *buildenv.cmd_prelude(),
        *buildenv.build_lines(src, out), ""]), encoding="ascii")
    result = subprocess.run(["cmd", "/d", "/c", str(script)], capture_output=True, text=True, errors="replace")
    if result.returncode != 0:
        print(result.stdout[-4000:], result.stderr[-2000:])
        raise SystemExit("falha ao compilar tools/assettool")
    return ASSETTOOL_EXE


_NAMES = None


def readable_names():
    """{entry path (lowercase): original path} from default.rcf's hashdictionary.txt, which lists
    "<original path> <hash>" for the hashed entries (package\\b06ecd92.p3d is
    package/Characters_Crash.p3d). Entries it doesn't list keep their own name."""
    global _NAMES
    if _NAMES is None:
        _NAMES = {}
        path = raw_path("default", "hashdictionary.txt")
        if path.exists():
            for line in path.read_text(encoding="latin-1").splitlines():
                parts = line.rsplit(None, 1)
                if len(parts) != 2:
                    continue
                original, h = parts[0].replace("/", "\\"), parts[1].lower()
                folder, _, base = original.rpartition("\\")
                ext = os.path.splitext(base)[1].lower()
                _NAMES[(folder + "\\" if folder else "").lower() + h + ext] = original
    return _NAMES


def readable(archive, entry):
    """Original path of an entry (backslashes), or the entry itself."""
    if archive != "default":
        return entry
    return readable_names().get(entry.lower(), entry)


def raw_path(archive, entry):
    return RAW / archive / Path(*entry.split("\\"))


# --- Categories of the export (textures, models) ----------------------------------------------------

LEVEL_RE = re.compile(r"^(l\d_e\d[ab]?|ai_pathfind|combatarena01|soundtest)$", re.I)
CHARACTER_PACKAGES = {"crash", "koala", "monkey", "rabbit", "bratgirl", "chiratta", "boss", "ukaukaboss",
                      "ninaspiderbot", "enemy_pack", "skin_pack", "cortex", "crashshadow", "rollershadow",
                      "sludgeshadow"}
INTERFACE_PACKAGES = {"frontend", "frontendloading", "hud", "hudassets", "ingamefrontend", "voodooballtexture"}
OBJECT_PREFIXES = ("stateprops_", "staticprops_", "objects_", "worldobjects_", "collectables_", "weapons_",
                   "projectile")


def category(archive, entry):
    """(folder, priority) of a package for the export: personagens/<who>, fases/<level>, objetos,
    efeitos, interface, cinematicas or outros. A file used by several packages goes to the one
    with the lowest priority (see pick_category)."""
    path = readable(archive, entry)
    parts = path.split("\\")
    top, stem = parts[0].lower(), Path(parts[-1]).stem
    low = stem.lower()
    if len(parts) > 2 and top in ("package", "levels") and LEVEL_RE.match(parts[1]):
        return f"fases/{parts[1].upper() if parts[1][0] in 'lL' else parts[1]}", 2
    m = re.match(r"^(l\d_e\d[ab]?)_pack$", low)
    if m:
        return f"fases/{m.group(1).upper()}", 3
    if "nis" in re.split(r"[_\W]+", low) or re.search(r"_nis\d", low) or top == "cinematics" or low.startswith("nis"):
        return "cinematicas", 5
    if top == "fx" or low.startswith("cha_fx"):
        return "efeitos", 2
    if low.startswith("fe_") or low.startswith("c_voodoofe") or low in INTERFACE_PACKAGES:
        return "interface", 2
    if top == "props" or low.startswith(OBJECT_PREFIXES):
        return "objetos", 2
    if low.startswith("characters_"):
        who = low.split("_")
        if len(who) > 2 and who[1] == "crash":  # Crash riding a monster: the monster's folder
            return f"personagens/{who[2]}", 1.5
        return f"personagens/{who[1]}", 1
    if low.startswith("accessories_"):
        return "personagens/acessorios", 1
    if top == "mdl" or low in CHARACTER_PACKAGES:
        return f"personagens/{low.removesuffix('_pack').removesuffix('shadow')}", 1
    if re.fullmatch(r"[0-9a-f]{5,8}", low):
        return "outros", 9  # hashed name not in hashdictionary.txt
    return "outros", 6


def pick_category(keys):
    """Folder for a file used by the packages `keys` ("archive/entry"): the category of the best
    one; several subfolders of the same kind (two levels, two characters) give <kind>/compartilhado."""
    cats = [category(*k.split("/", 1)) for k in keys]
    best = min(p for _c, p in cats)
    folders = sorted({c for c, p in cats if p == best})
    if len(folders) == 1:
        return folders[0]
    tops = {f.split("/")[0] for f in folders}
    if len(tops) == 1 and "/" in folders[0]:
        return f"{folders[0].split('/')[0]}/compartilhado"
    return folders[0]


def entry_type(name, head):
    ext = os.path.splitext(name)[1].lower()
    kind = TYPES.get(ext, ext.lstrip(".") or "unknown")
    if kind == "pure3d" and head[:4] == b"\xffD3P":
        kind = "pure3d-be"  # sound scripts (sound1.rcf) are big-endian Pure3D
    return kind


def load_index():
    if INDEX_JSON.exists():
        return json.loads(INDEX_JSON.read_text(encoding="utf-8"))
    return {"entries": {}}


def save_index(index):
    ASSETS.mkdir(parents=True, exist_ok=True)
    tmp = INDEX_JSON.with_suffix(".tmp")
    tmp.write_text(json.dumps(index, indent=1, sort_keys=True), encoding="utf-8")
    os.replace(tmp, INDEX_JSON)
    with open(INDEX_CSV, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["archive", "entry", "name", "size", "type", "offset", "hash", "converted"])
        for key in sorted(index["entries"]):
            e = index["entries"][key]
            w.writerow([e["archive"], e["entry"], e.get("name", e["entry"]), e["size"], e["type"], f"{e['offset']:#x}",
                        f"{e['hash']:#010x}", ";".join(e.get("converted", []))])


def record_converted(index, archive, entry, outputs, replace_prefix=None):
    """Adds converted files (paths relative to private/assets) to an index row."""
    e = index["entries"].get(f"{archive}/{entry}")
    if e is None:
        return
    conv = e.setdefault("converted", [])
    if replace_prefix:
        conv[:] = [c for c in conv if not c.startswith(replace_prefix)]
    for o in outputs:
        o = str(o).replace("\\", "/")
        if o not in conv:
            conv.append(o)


def scan_archive(path, archive, index, extract):
    arc = rcf.Archive(path)
    done = skipped = 0
    for e in arc.entries:
        key = f"{archive}/{e.name}"
        target = raw_path(archive, e.name)
        row = index["entries"].get(key)
        if extract and not (target.exists() and target.stat().st_size == e.size):
            data = arc.read(e)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            done += 1
        else:
            skipped += 1
        if row is None or row["size"] != e.size or row["offset"] != e.offset or not extract:
            head = b""
            if target.exists():
                with open(target, "rb") as f:
                    head = f.read(16)
            else:
                arc.file.seek(e.offset)
                head = arc.file.read(16)
            row = {"archive": archive, "entry": e.name, "name": readable(archive, e.name), "size": e.size,
                   "offset": e.offset, "hash": e.hash, "type": entry_type(e.name, head),
                   "converted": (row or {}).get("converted", [])}
            index["entries"][key] = row
    return len(arc.entries), done, skipped


def cmd_extract(args, extract=True):
    index = load_index()
    names = args.archive or ARCHIVES
    for name in names:
        path = Path(args.game) / f"{name}.rcf"
        if not path.exists():
            print(f"{name}: {path} não existe, pulando")
            continue
        total, done, skipped = scan_archive(path, name, index, extract)
        if extract:
            print(f"{name}: {total} entradas, {done} extraídas, {skipped} já existiam")
        else:
            print(f"{name}: {total} entradas indexadas")
    save_index(index)
    print(f"índice: {INDEX_JSON.relative_to(ROOT)} ({len(index['entries'])} entradas)")
    return 0


def cmd_verify(args):
    """Compares the raw files with the archives (all entries, or a sample)."""
    import random
    bad = checked = 0
    for name in args.archive or ARCHIVES:
        arc = rcf.Archive(Path(args.game) / f"{name}.rcf")
        entries = arc.entries
        if args.sample:
            entries = random.Random(1).sample(entries, min(args.sample, len(entries)))
        for e in entries:
            target = raw_path(name, e.name)
            checked += 1
            if not target.exists() or target.read_bytes() != arc.read(e):
                bad += 1
                print(f"DIFERENTE: {name}/{e.name}")
    print(f"{checked} entradas conferidas, {bad} diferentes")
    return 1 if bad else 0


def cmd_status(_args):
    index = load_index()
    by = Counter()
    conv = Counter()
    size = Counter()
    for e in index["entries"].values():
        by[(e["archive"], e["type"])] += 1
        size[e["archive"]] += e["size"]
        if e.get("converted"):
            conv[(e["archive"], e["type"])] += 1
    for (archive, kind), n in sorted(by.items()):
        print(f"{archive:10} {kind:14} {n:6}  convertidas: {conv[(archive, kind)]}")
    for archive, s in sorted(size.items()):
        print(f"{archive:10} {s / 2**20:8.1f} MB")
    return 0


def cmd_all(args):
    """Every stage, each incremental: raw entries, sounds, videos, textures, models."""
    import subprocess
    tools = Path(__file__).resolve().parent
    steps = [[str(tools / "assets.py"), "extract", "--game", args.game],
             [str(tools / "audio.py"), "export"], [str(tools / "audio.py"), "videos"],
             [str(tools / "textures.py"), "export"], [str(tools / "models.py"), "export"]]
    for step in steps:
        print(f"== {Path(step[0]).name} {' '.join(step[1:])}", flush=True)
        if subprocess.run([sys.executable] + step).returncode:
            print("falhou; parando")
            return 1
    return cmd_status(args)


def main(argv):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("extract", "index"):
        s = sub.add_parser(name)
        s.add_argument("--archive", action="append", help="só este .rcf (sem extensão); pode repetir")
        s.add_argument("--game", default=str(DEFAULT_GAME))
    s = sub.add_parser("verify")
    s.add_argument("--archive", action="append")
    s.add_argument("--game", default=str(DEFAULT_GAME))
    s.add_argument("--sample", type=int, default=0, help="só N entradas sorteadas por .rcf (0 = todas)")
    sub.add_parser("status")
    s = sub.add_parser("all", help="extract + audio export/videos + textures + models")
    s.add_argument("--game", default=str(DEFAULT_GAME))
    args = p.parse_args(argv)
    if args.cmd == "all":
        return cmd_all(args)
    if args.cmd == "verify":
        return cmd_verify(args)
    if args.cmd == "extract":
        return cmd_extract(args)
    if args.cmd == "index":
        return cmd_extract(args, extract=False)
    return cmd_status(args)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main(sys.argv[1:]))
