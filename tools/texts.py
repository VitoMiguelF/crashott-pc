"""Exports the game's texts (menus, HUD, subtitles...) to editable files and builds a mod from them.

    python tools/texts.py scan                       # what text bibles exist and where
    python tools/texts.py export [--out DIR]         # writes DIR/<bible>/<lang>.txt (default private/texts)
    python tools/texts.py build --mod NAME [--src DIR]
    python tools/texts.py lang-init ID [--base E]     # starts languages/ID/texts from the base language
    python tools/texts.py lang-check ID               # checks a language pack (glyphs, keys, lengths)

Workflow: export once, edit the right side of the "0xHASH = text" lines, then build. build writes
only the packages whose texts changed, as mods/NAME/rcf/default/<package path>, which the runtime
serves in place of that entry of default.rcf (runtime/fs/rcf_overlay.cpp). The game folder is
never written.

Texts live in Pure3D text bibles inside the .p3d packages of default.rcf. Pure3D chunks are
little-endian: u32 id, u32 header size (data before the children), u32 total size.
    0x1800D  text bible: name, then one child per language
    0x1800E  language: pascal name (length byte, padded with NULs), u8 language letter,
             u32 count, u32 hash modulo, u32 buffer size, u32 hashes[count], u32 offsets[count]
             (byte offsets into the buffer), then the buffer of NUL-terminated UTF-16LE strings
Strings are looked up by key hash, so the hashes and their order are kept; only the buffer and the
offsets are rebuilt. The same bible is often copied into several packages; build patches all of
them.

Language packs (languages/<id>/, runtime/settings/language_pack.h) are extra languages the runtime
offers in the settings screen: the game runs in the base language (voices included) and the
runtime rebuilds that language's bibles with languages/<id>/texts/<bible>.txt when the game opens
default.rcf, so nothing is generated here. Copies of a bible differ: the same key can hold an
unrelated text in an older copy (keys are name hashes modulo 0x1003F). The common lines follow
the largest copy; a "[package\\x.p3d ...]" line starts lines for the other copies' keys whose
text differs ("[]" goes back to common lines). lang-init writes the files with the base language's
texts to be translated; lang-check compares a pack with the base language.
"""
import argparse
import hashlib
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rcf  # noqa: E402
import buildenv  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_GAME = buildenv.game_folder()  # config/local.toml or the player's settings.toml
ARCHIVE = "default.rcf"
BIBLE, LANGUAGE = 0x1800D, 0x1800E
LANGUAGE_NAMES = {"E": "English", "F": "French", "G": "German", "S": "Spanish", "I": "Italian", "D": "Dutch"}


def walk(data, start, end, path=()):
    """Yields (offset, id, header size, total size, ancestor offsets) for every chunk."""
    off = start
    while off + 12 <= end:
        cid, hsize, tsize = struct.unpack_from("<3I", data, off)
        if tsize < 12 or hsize < 12 or hsize > tsize or off + tsize > end:
            return
        yield off, cid, hsize, tsize, path
        yield from walk(data, off + hsize, off + tsize, path + (off,))
        off += tsize


def pascal(data, pos):
    length = data[pos]
    return data[pos + 1:pos + 1 + length].split(b"\0")[0].decode("latin-1"), pos + 1 + length


def parse_language(data, off):
    _cid, hsize, _tsize = struct.unpack_from("<3I", data, off)
    name, pos = pascal(data, off + 12)
    letter = chr(data[pos])
    count, modulo, bufsize = struct.unpack_from("<3I", data, pos + 1)
    pos += 13
    hashes = struct.unpack_from(f"<{count}I", data, pos)
    offsets = struct.unpack_from(f"<{count}I", data, pos + 4 * count)
    buffer = data[pos + 8 * count:pos + 8 * count + bufsize]
    texts = []
    for o in offsets:
        end = o
        while end + 1 < len(buffer) and buffer[end:end + 2] != b"\0\0":
            end += 2
        texts.append(buffer[o:end].decode("utf-16-le", errors="surrogatepass"))
    return {"name": name, "letter": letter, "modulo": modulo, "hashes": hashes, "texts": texts,
            "fields_at": pos - 13, "header_end": off + hsize}


def build_language(data, off, texts):
    """Returns the bytes of language chunk `off` with `texts` (same count/order) in its buffer.

    The original buffer is kept as is: keys may share a string and strings need not follow key
    order, so an edited text is written over its own slot when it fits and nothing else points
    there, and appended (repointing only that key) otherwise. Unchanged texts give identical bytes.
    """
    info = parse_language(data, off)
    count = len(info["hashes"])
    pos = info["fields_at"] + 13
    offsets = list(struct.unpack_from(f"<{count}I", data, pos + 4 * count))
    bufsize = struct.unpack_from("<I", data, info["fields_at"] + 9)[0]
    buffer = bytearray(data[pos + 8 * count:pos + 8 * count + bufsize])
    users = {}
    for o in offsets:
        users[o] = users.get(o, 0) + 1
    for i, text in enumerate(texts):
        if text == info["texts"][i]:
            continue
        encoded = text.encode("utf-16-le", errors="surrogatepass") + b"\0\0"
        slot = len(info["texts"][i].encode("utf-16-le", errors="surrogatepass")) + 2
        if users[offsets[i]] == 1 and len(encoded) <= slot:
            buffer[offsets[i]:offsets[i] + slot] = encoded + bytes(slot - len(encoded))
        else:
            users[offsets[i]] -= 1
            if len(buffer) % 2:
                buffer.append(0)
            offsets[i] = len(buffer)
            users[offsets[i]] = 1
            buffer += encoded
    head = bytearray(data[off + 12:info["fields_at"]])  # pascal name
    head.append(ord(info["letter"]))
    head += struct.pack("<3I", len(texts), info["modulo"], len(buffer))
    head += struct.pack(f"<{len(texts)}I", *info["hashes"])
    head += struct.pack(f"<{len(texts)}I", *offsets)
    body = bytes(head) + bytes(buffer)
    size = 12 + len(body)
    return struct.pack("<3I", LANGUAGE, size, size) + body


ESCAPES = {"\\": "\\\\", "\n": "\\n", "\r": "\\r", "\t": "\\t"}


def escape(text):
    out = []
    for ch in text:
        if ch in ESCAPES:
            out.append(ESCAPES[ch])
        elif ord(ch) < 0x20 or 0xD800 <= ord(ch) <= 0xDFFF or 0xE000 <= ord(ch) <= 0xF8FF:
            out.append(f"\\u{ord(ch):04X}")  # control codes, button glyphs (private use area)
        else:
            out.append(ch)
    return "".join(out)


def unescape(text):
    def repl(m):
        s = m.group(0)
        if s.startswith("\\u"):
            return chr(int(s[2:], 16))
        return {"\\\\": "\\", "\\n": "\n", "\\r": "\r", "\\t": "\t"}[s]
    return re.sub(r"\\u[0-9A-Fa-f]{4}|\\[\\nrt]", repl, text)


def scan(game):
    """Finds every language chunk in the archive's packages, grouped by (bible, language)."""
    archive = rcf.Archive(game / ARCHIVE)
    found = {}  # (bible, letter) -> {content hash: [(entry name, chunk offset)]}
    parsed = {}
    for entry in archive.entries:
        if not entry.name.lower().endswith(".p3d"):
            continue
        data = archive.read(entry)
        if b"\x0d\x80\x01\x00" not in data:  # quick filter for the bible chunk id
            continue
        for off, cid, hsize, tsize, path in walk(data, 12, len(data)):
            if cid != LANGUAGE or not path:
                continue
            parent_id = struct.unpack_from("<I", data, path[-1])[0]
            if parent_id != BIBLE:
                continue
            bible, _ = pascal(data, path[-1] + 12)
            info = parse_language(data, off)
            digest = hashlib.sha1(data[off:off + tsize]).hexdigest()[:10]
            found.setdefault((bible, info["letter"]), {}).setdefault(digest, []).append((entry.name, off))
            parsed[digest] = info
    return archive, found, parsed


def merged(variants, parsed):
    """One key order and text per hash over every version of a (bible, language): the largest
    version first, then the keys only the others have."""
    order, texts = [], {}
    for digest in sorted(variants, key=lambda d: -len(parsed[d]["texts"])):
        for h, t in zip(parsed[digest]["hashes"], parsed[digest]["texts"]):
            if h not in texts:
                order.append(h)
                texts[h] = t
    return order, texts


def cmd_scan(args):
    _archive, found, parsed = scan(args.game)
    for (bible, letter), variants in sorted(found.items()):
        for digest, places in variants.items():
            print(f"{bible:24} {letter} ({LANGUAGE_NAMES.get(letter, '?'):8}) {len(parsed[digest]['texts']):5} textos  "
                  f"versão {digest}  em {len(places)} pacote(s)")
    return 0


def cmd_export(args):
    _archive, found, parsed = scan(args.game)
    out = Path(args.out)
    total = 0
    for (bible, letter), variants in sorted(found.items()):
        order, texts = merged(variants, parsed)
        packages = sorted({p for places in variants.values() for p, _ in places})
        path = out / bible / f"{letter}.txt"
        path.parent.mkdir(parents=True, exist_ok=True)
        lines = [f"# {bible} / {letter} ({LANGUAGE_NAMES.get(letter, '?')}): {len(order)} textos, "
                 f"{len(variants)} versão(ões) em {len(packages)} pacote(s): {', '.join(packages)}",
                 "# Edite só o texto depois de ' = '. Escapes: \\n quebra de linha, \\\\ barra, \\uXXXX caractere",
                 "# especial (ícones de botão). Não mude os hashes. Linhas apagadas ficam com o texto original.",
                 "# Depois rode: python tools\\texts.py build --mod <nome>",
                 ""]
        lines += [f"{h:#010x} = {escape(texts[h])}" for h in order]
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        total += 1
    print(f"{total} arquivo(s) de texto em {out}")
    return 0


def read_text_file(path):
    """hash -> text for every "0xHASH = text" line."""
    texts = {}
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        key, sep, value = line.partition(" = ")
        try:
            h = int(key.strip(), 16)
        except ValueError:
            h = None
        if not sep or h is None:
            raise SystemExit(f"{path}:{number}: esperado '0xHASH = texto'")
        texts[h] = unescape(value)
    return texts


def cmd_build(args):
    archive, found, parsed = scan(args.game)
    src = Path(args.src)
    edits = {}  # entry name -> {chunk offset: new texts}
    changed_files = 0
    for (bible, letter), variants in sorted(found.items()):
        path = src / bible / f"{letter}.txt"
        if not path.exists():
            continue
        order, baseline = merged(variants, parsed)
        edited = read_text_file(path)
        unknown = [h for h in edited if h not in baseline]
        if unknown:
            raise SystemExit(f"{path}: hash(es) que não existem nesta bíblia: {', '.join(f'{h:#010x}' for h in unknown[:5])}")
        # A line counts as edited when it differs from what export wrote; it then replaces that
        # key in every version, while unedited keys keep each version's own text.
        changes = {h: t for h, t in edited.items() if t != baseline[h]}
        if not changes:
            continue
        changed_files += 1
        touched = 0
        for digest, places in variants.items():
            info = parsed[digest]
            texts = [changes.get(h, t) for h, t in zip(info["hashes"], info["texts"])]
            if texts == list(info["texts"]):
                continue
            for entry_name, off in places:
                edits.setdefault(entry_name, {})[off] = texts
                touched += 1
        print(f"{bible}/{letter}.txt: {len(changes)} texto(s) alterado(s), aplicado(s) em {touched} cópia(s)")

    out_root = Path(args.mods_dir) / args.mod / "rcf" / Path(ARCHIVE).stem
    for entry_name, chunks in sorted(edits.items()):
        data = bytearray(archive.read(archive.find(entry_name)))
        # Rebuild from the last chunk backwards so earlier offsets stay valid.
        for off in sorted(chunks, reverse=True):
            tsize = struct.unpack_from("<I", data, off + 8)[0]
            ancestors = next(path for o, cid, h, t, path in walk(bytes(data), 12, len(data)) if o == off)
            new = build_language(bytes(data), off, chunks[off])
            delta = len(new) - tsize
            data[off:off + tsize] = new
            for a in ancestors:
                struct.pack_into("<I", data, a + 8, struct.unpack_from("<I", data, a + 8)[0] + delta)
            struct.pack_into("<I", data, 8, struct.unpack_from("<I", data, 8)[0] + delta)  # P3D file header
        target = out_root / Path(*entry_name.split("\\"))
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(bytes(data))
        print(f"  -> {target}")
    if not changed_files:
        print("nenhum texto mudou; nada foi gerado")
    else:
        print(f"mod '{args.mod}' pronto: {len(edits)} pacote(s) em {out_root}")
    return 0


LANGUAGES_DIR = ROOT / "languages"
# Characters the game draws as button icons and symbols: a translation keeps the same ones.
GLYPHS = set("©®†±²³´µ¶·¸¹º»¼½¾")


def pack_layout(variants, parsed):
    """Base-language texts as a pack lays them out: the common lines (merged order, largest copy
    first) and, per other copy, the keys whose text differs from the common one."""
    order, common = merged(variants, parsed)
    sections = []
    for digest in sorted(variants, key=lambda d: (-len(parsed[d]["texts"]), d))[1:]:
        info = parsed[digest]
        diff = [(h, t) for h, t in zip(info["hashes"], info["texts"]) if common[h] != t]
        if diff:
            sections.append((sorted({p for p, _ in variants[digest]}), diff))
    return order, common, sections


def read_pack_texts(path):
    """Common lines and per-package lines of a pack's texts/<bible>.txt:
    ({hash: (text, line)}, {package: {hash: (text, line)}})."""
    common, by_package = {}, {}
    targets = [common]
    for number, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if stripped.startswith("[") and stripped.endswith("]"):
            names = stripped[1:-1].replace(",", " ").split()
            targets = [by_package.setdefault(n.replace("/", "\\").lower(), {}) for n in names] or [common]
            continue
        key, sep, value = line.partition(" = ")
        try:
            h = int(key.strip(), 16)
        except ValueError:
            h = None
        if not sep or h is None:
            raise SystemExit(f"{path}:{number}: esperado '0xHASH = texto'")
        for target in targets:
            target[h] = (unescape(value), number)
    return common, by_package


FONT_PACKAGE = "package\\b4c85fe7.p3d"  # Titans_Large/Small, the fonts the X360 build draws with
FONT, GLYPH_LIST = 0x22000, 0x22001


def font_characters(archive):
    """Characters every font of FONT_PACKAGE draws (glyphs with an empty texture rectangle, like
    ã Ã Ê Ô there, are only a space). Glyph records: u32 texture, f32 u0 v0 u1 v1, f32 bearings,
    width, advance, u32 code."""
    data = archive.read(archive.find(FONT_PACKAGE))
    sets = []
    for off, cid, _h, tsize, _path in walk(data, 12, len(data)):
        if cid != GLYPH_LIST:
            continue
        count = struct.unpack_from("<I", data, off + 12)[0]
        drawn = set()
        for i in range(count):
            _tex, u0, _v0, u1, _v1, _l, _r, _w, _a, code = struct.unpack_from("<I8fI", data, off + 16 + i * 40)
            if u1 > u0 or code == 0x20:
                drawn.add(chr(code))
        sets.append(drawn)
    return set.intersection(*sets) if sets else set()


def base_texts(found, letter):
    """bible -> variants of the base language."""
    return {bible: variants for (bible, l), variants in found.items() if l == letter}


def cmd_lang_init(args):
    _archive, found, parsed = scan(args.game)
    folder = LANGUAGES_DIR / args.id
    out = folder / "texts"
    out.mkdir(parents=True, exist_ok=True)
    written = 0
    for bible, variants in sorted(base_texts(found, args.base).items()):
        path = out / f"{bible}.txt"
        if path.exists():
            print(f"{path} já existe; mantido")
            continue
        order, common, sections = pack_layout(variants, parsed)
        lines = [f"# {bible}: textos do pacote de idioma '{args.id}' (base {args.base}, {LANGUAGE_NAMES.get(args.base, '?')}).",
                 "# Traduza só o texto depois de ' = '. Mantenha os ícones de botão (± ² ³ ´ µ ¶ · ¸ ¹ º » ¼ ½ ¾),",
                 "# © ® †, os espaços das pontas e os escapes (\\n quebra de linha, \\\\ barra, \\uXXXX). Não mude",
                 "# os hashes. Chave sem linha = texto original. Confira com: python tools\\texts.py lang-check " + args.id,
                 ""]
        lines += [f"{h:#010x} = {escape(common[h])}" for h in order]
        for packages, diff in sections:
            lines += ["", f"# Outra cópia desta bíblia ({len(diff)} chave(s) com outro texto nela):", f"[{' '.join(packages)}]"]
            lines += [f"{h:#010x} = {escape(t)}" for h, t in diff]
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        written += 1
        print(f"{path}: {len(order)} texto(s), {sum(len(d) for _, d in sections)} em {len(sections)} seção(ões) de outras cópias")
    print(f"{written} arquivo(s) em {out}")
    packages = sorted({p for variants in base_texts(found, args.base).values() for places in variants.values() for p, _ in places})
    print("pacotes com textos (packages do language.toml):")
    for p in packages:
        print(f'    "{p.replace(chr(92), "/")}",')
    return 0


def cmd_lang_check(args):
    import tomllib
    _archive, found, parsed = scan(args.game)
    folder = LANGUAGES_DIR / args.id
    config = tomllib.loads((folder / "language.toml").read_text(encoding="utf-8"))
    letter = {"english": "E", "french": "F", "german": "G", "spanish": "S", "italian": "I", "dutch": "D"}.get(config.get("base", "english"), "E")
    bibles = base_texts(found, letter)
    problems, warnings = [], []

    wanted = sorted({p.lower() for variants in bibles.values() for places in variants.values() for p, _ in places})
    listed = sorted(p.replace("/", "\\").lower() for p in config.get("packages", []))
    for p in sorted(set(wanted) - set(listed)):
        problems.append(f"language.toml: falta o pacote {p} em packages (os textos dele ficam no idioma base)")
    for p in sorted(set(listed) - set(wanted)):
        warnings.append(f"language.toml: {p} não tem textos do idioma base")

    font = font_characters(_archive)
    for recipe in config.get("glyphs", []):  # "ã = a + ñ - n @ å": composed by the runtime
        parts, _, donor = recipe.replace(" ", "").partition("@")
        if len(parts) == 7 and set(parts[2::2]) <= font:
            font.add(parts[0])
            font.discard(donor)  # its cell may now hold the new glyph

    total = translated = same = 0
    long_lines = []
    for bible, variants in sorted(bibles.items()):
        path = folder / "texts" / f"{bible}.txt"
        order, common, sections = pack_layout(variants, parsed)
        if not path.exists():
            problems.append(f"{path}: não existe ({len(order)} texto(s) ficam no idioma base)")
            continue
        pack_common, pack_sections = read_pack_texts(path)
        expected = [(None, common, order)] + [(packages, dict(diff), [h for h, _ in diff]) for packages, diff in sections]
        for h in pack_common:
            if h not in common:
                problems.append(f"{path}:{pack_common[h][1]}: {h:#010x} não existe nesta bíblia")
        for package, texts in pack_sections.items():
            section = next((dict(diff) for packages, diff in sections if package in packages), None)
            if section is None:
                problems.append(f"{path}: seção [{package}] não corresponde a uma cópia com textos diferentes")
                continue
            for h, (_t, number) in texts.items():
                if h not in section:
                    problems.append(f"{path}:{number}: {h:#010x} não tem texto diferente em {package} (use a linha comum)")
        for packages, source, keys in expected:
            for h in keys:
                total += 1
                english = source[h]
                entry = pack_common.get(h) if packages is None else next(
                    (pack_sections[p][h] for p in packages if p in pack_sections and h in pack_sections[p]), None)
                if entry is None:
                    if packages is None or not english.strip():
                        if english.strip():
                            warnings.append(f"{path}: {h:#010x} sem tradução")
                    else:
                        warnings.append(f"{path}: {h:#010x} sem tradução em [{' '.join(packages)}] (usa a linha comum)")
                    continue
                text, number = entry
                where = f"{path.name}:{number} {h:#010x}"
                translated += 1
                if text == english and any(c.isalpha() for c in english):
                    same += 1
                if sorted(c for c in text if c in GLYPHS) != sorted(c for c in english if c in GLYPHS):
                    problems.append(f"{where}: ícones/símbolos diferentes do original ({''.join(sorted(c for c in english if c in GLYPHS))!r})")
                if [c for c in text if ord(c) < 0x20 and c != "\n"] != [c for c in english if ord(c) < 0x20 and c != "\n"]:
                    problems.append(f"{where}: códigos de controle diferentes do original")
                lead = len(english) - len(english.lstrip(" ")), len(english) - len(english.rstrip(" "))
                if english.strip() and (len(text) - len(text.lstrip(" ")), len(text) - len(text.rstrip(" "))) != lead:
                    warnings.append(f"{where}: espaços nas pontas diferentes do original")
                if text.count("\n") != english.count("\n"):
                    warnings.append(f"{where}: {text.count(chr(10))} quebra(s) de linha, o original tem {english.count(chr(10))}")
                missing = sorted({c for c in text if c not in font and c not in "\n" and c not in english})
                if missing:
                    problems.append(f"{where}: caractere(s) que a fonte do jogo não desenha: {''.join(missing)} "
                                    "(use outro ou componha em glyphs no language.toml)")
                if len(english) <= 40 and len(text) > len(english) * 1.35 + 2 and english.strip():
                    long_lines.append((len(text) / max(1, len(english)), f"{where}: {len(english)} -> {len(text)} caracteres"))

    for p in problems:
        print("ERRO  " + p)
    for w in warnings[:args.max_warnings]:
        print("aviso " + w)
    if len(warnings) > args.max_warnings:
        print(f"aviso ... e mais {len(warnings) - args.max_warnings} aviso(s)")
    if args.long:
        print("Textos curtos que ficaram bem mais longos (conferir na tela):")
        for _ratio, line in sorted(long_lines, reverse=True)[:args.long]:
            print("      " + line)
    print(f"{translated}/{total} texto(s) com linha no pacote; {same} igual(is) ao original; "
          f"{len(problems)} erro(s), {len(warnings)} aviso(s), {len(long_lines)} texto(s) curto(s) bem mais longo(s)")
    return 1 if problems else 0


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description="Exporta e reimporta os textos do jogo.")
    parser.add_argument("--game", type=Path, default=DEFAULT_GAME, help="pasta do jogo (somente leitura)")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("scan")
    p = sub.add_parser("export")
    p.add_argument("--out", default=str(ROOT / "private" / "texts"))
    p = sub.add_parser("build")
    p.add_argument("--mod", required=True)
    p.add_argument("--src", default=str(ROOT / "private" / "texts"))
    p.add_argument("--mods-dir", default=str(ROOT / "dist" / "mods"), help="pasta de mods onde gravar (padrão: dist/mods, a do jogo)")
    p = sub.add_parser("lang-init", help="cria languages/ID/texts a partir do idioma base")
    p.add_argument("id")
    p.add_argument("--base", default="E", help="letra do idioma base (padrão E, inglês)")
    p = sub.add_parser("lang-check", help="confere um pacote de idioma")
    p.add_argument("id")
    p.add_argument("--long", type=int, default=0, help="lista os N textos curtos que mais cresceram")
    p.add_argument("--max-warnings", type=int, default=40)
    args = parser.parse_args()
    return {"scan": cmd_scan, "export": cmd_export, "build": cmd_build,
            "lang-init": cmd_lang_init, "lang-check": cmd_lang_check}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
