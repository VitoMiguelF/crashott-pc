"""Prints the game's frontend fight tree (fighttrees\\Frontend.bfig): every screen (node) and exit with
the number the game uses for it, where each exit goes and the Lua snippets it runs.

    python tools/frontend_tree.py                     # the whole tree
    python tools/frontend_tree.py --node 575          # one node and what is under it (the HUB)
    python tools/frontend_tree.py --find StartLevel   # records whose name or snippets contain the text
    python tools/frontend_tree.py --bfig other.bfig   # another copy (default: the extracted one in private/)

The numbers are the ones sub_820F9BA8 (the native conditions) takes and returns, CRASH_FRONTEND_TRACE
logs ("frontend node 70") and runtime/settings/direct_boot.cpp uses: nodes and exits share one
numbering, in file order, starting at 1 for the tree itself. An exit "-> (children)" picks one of the
exits under it.

Records: [u32 kind][u32 name length][name, NUL-padded to 4][u32 parent][u32 children]..., kinds
0xE056B923 tree, 0x83A93633 node (a screen), 0xE8CB721B condition (a branch of a node),
0xBC27359D exit (target node at +0x18 after the parent); then length-prefixed text: the snippets
(Lua run by the game) and the names of pages, textures and messages the record uses.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BFIG = ROOT / "private" / "assets" / "raw" / "default" / "fighttrees" / "Frontend.bfig"
KINDS = {0xE056B923: "TREE", 0x83A93633: "NODE", 0xE8CB721B: "COND", 0xBC27359D: "EXIT"}
NO_TARGET = 0xFFFFFFFF


def records(data):
    """[(number, kind, name, parent, target or None, [texts])] in file order."""
    found = []
    at = 0
    while at + 8 <= len(data):
        kind = struct.unpack_from(">I", data, at)[0]
        length = struct.unpack_from(">I", data, at + 4)[0]
        if kind in KINDS and 1 <= length <= 80 and re.fullmatch(rb"[ -~]+", data[at + 8:at + 8 + length]):
            fields = at + 8 + ((length + 3) & ~3)
            found.append((at, KINDS[kind], data[at + 8:at + 8 + length].decode(), fields))
            at = fields
            continue
        at += 1
    result = []
    for i, (at, kind, name, fields) in enumerate(found):
        end = found[i + 1][0] if i + 1 < len(found) else len(data)
        parent = struct.unpack_from(">I", data, fields)[0]
        target = struct.unpack_from(">I", data, fields + 0x18)[0] if kind == "EXIT" else None
        result.append((i + 1, kind, name, parent, target, texts(data, fields + 8, end)))
    return result


def texts(data, start, end):
    """Length-prefixed strings between start and end (snippets, page and texture names)."""
    out = []
    at = start
    while at + 4 <= end:
        length = struct.unpack_from(">I", data, at)[0]
        text = data[at + 4:at + 4 + length]
        if 4 <= length <= 4000 and at + 4 + length <= end and re.fullmatch(rb"[\t\r\n -~]+", text.rstrip(b"\0")):
            line = text.rstrip(b"\0").decode().replace("\r\n", "; ").replace("\n", "; ").strip()
            if line.strip("; "):
                out.append(line)
            at += 4 + length
            continue
        at += 1
    return out


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description="Mostra a árvore do frontend (Frontend.bfig) com os números do jogo.")
    parser.add_argument("--bfig", default=str(DEFAULT_BFIG), help="o Frontend.bfig (padrão: o extraído em private/)")
    parser.add_argument("--node", type=int, help="só este nó/saída e o que está abaixo dele")
    parser.add_argument("--find", help="só os registros cujo nome ou snippet contém o texto")
    args = parser.parse_args()
    path = Path(args.bfig)
    if not path.is_file():
        raise SystemExit(f"{path} não existe: extraia o default.rcf com tools/assets.py ou passe --bfig")
    rows = records(path.read_bytes())
    names = {number: name for number, _, name, _, _, _ in rows}
    children = {}
    for number, _, _, parent, _, _ in rows:
        children.setdefault(parent, []).append(number)

    wanted = None
    if args.node is not None:
        wanted, stack = set(), [args.node]
        while stack:
            n = stack.pop()
            if n in wanted:
                continue
            wanted.add(n)
            stack += [c for c in children.get(n, []) if c != n]
    for number, kind, name, parent, target, lines in rows:
        if wanted is not None and number not in wanted:
            continue
        if args.find and args.find.lower() not in (name + " ".join(lines)).lower():
            continue
        where = ""
        if kind == "EXIT":
            where = " -> (children)" if target == NO_TARGET else f" -> {target} {names.get(target, '?')}"
        print(f"{number:5} {kind} {name:36} parent {parent:5}{where}")
        for line in lines:
            print(f"        | {line[:220]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
