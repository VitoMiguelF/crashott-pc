"""Wraps the decrypted XEX image (private/image.bin) in a big-endian PPC64 ELF for Ghidra / REA.

Neither REA's raw-binary mode nor Ghidra's stock loaders read an XEX, but both open an ELF by its
header. The image becomes one loadable section at 0x82000000 with the XEX entry point, and the
names of private/symbols/functions.csv (when present) become function symbols. Ghidra picks
PowerPC:BE:64 (AltiVec, no VMX128: the Xenon-only vector instructions stay undecoded).

    python tools/image_to_elf.py [--image private/image.bin] [--symbols private/symbols/functions.csv]
                                 [--out private/image.elf]

The output is derived from the game: it stays in private/ (gitignored), never in git.
"""

import argparse
import csv
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IMAGE_BASE = 0x82000000
ENTRY_POINT = 0x82264F40  # [loader] entry point of default.xex (title 565507E4)
EM_PPC64 = 21


def read_symbols(path):
    symbols = {}
    if not path.is_file():
        return symbols
    with open(path, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            address = int(row["address"], 16)
            symbols.setdefault(address, row["name"])
    return sorted(symbols.items())


def build(image, symbols, entry):
    shstrtab = b"\0.text\0.symtab\0.strtab\0.shstrtab\0"
    names = {".text": 1, ".symtab": 7, ".strtab": 15, ".shstrtab": 23}

    strtab = bytearray(b"\0")
    symtab = bytearray(24)  # the null symbol
    end = IMAGE_BASE + len(image)
    addresses = [a for a, _ in symbols] + [end]
    for i, (address, name) in enumerate(symbols):
        if not IMAGE_BASE <= address < end:
            continue
        offset = len(strtab)
        strtab += name.encode("utf-8") + b"\0"
        size = addresses[i + 1] - address
        # st_name, st_info (GLOBAL FUNC), st_other, st_shndx (.text = 1), st_value, st_size
        symtab += struct.pack(">IBBHQQ", offset, 0x12, 0, 1, address, size)

    header_size, program_size, section_size = 64, 56, 64
    text_offset = 0x10000  # page-aligned file offset for the PT_LOAD
    symtab_offset = text_offset + len(image)
    strtab_offset = symtab_offset + len(symtab)
    shstrtab_offset = strtab_offset + len(strtab)
    sections_offset = (shstrtab_offset + len(shstrtab) + 7) & ~7

    out = bytearray()
    out += b"\x7fELF" + bytes([2, 2, 1, 0]) + bytes(8)  # ELFCLASS64, ELFDATA2MSB, EV_CURRENT
    out += struct.pack(">HHIQQQIHHHHHH", 2, EM_PPC64, 1, entry, header_size, sections_offset, 0,
                       header_size, program_size, 1, section_size, 5, 4)
    # PT_LOAD, R+W+X: the image holds code and data
    out += struct.pack(">IIQQQQQQ", 1, 7, text_offset, IMAGE_BASE, IMAGE_BASE, len(image), len(image), 0x10000)
    out += bytes(text_offset - len(out))
    out += image + symtab + strtab + shstrtab
    out += bytes(sections_offset - len(out))

    def section(name, kind, flags, address, offset, size, link=0, info=0, align=1, entsize=0):
        return struct.pack(">IIQQQQIIQQ", name, kind, flags, address, offset, size, link, info, align, entsize)

    out += bytes(section_size)
    out += section(names[".text"], 1, 0x7, IMAGE_BASE, text_offset, len(image), align=4)  # PROGBITS, WAX
    out += section(names[".symtab"], 2, 0, 0, symtab_offset, len(symtab), link=3, info=1, align=8, entsize=24)
    out += section(names[".strtab"], 3, 0, 0, strtab_offset, len(strtab))
    out += section(names[".shstrtab"], 3, 0, 0, shstrtab_offset, len(shstrtab))
    return bytes(out), len(symtab) // 24 - 1


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--image", type=Path, default=ROOT / "private" / "image.bin")
    parser.add_argument("--symbols", type=Path, default=ROOT / "private" / "symbols" / "functions.csv")
    parser.add_argument("--out", type=Path, default=ROOT / "private" / "image.elf")
    parser.add_argument("--entry", type=lambda s: int(s, 16), default=ENTRY_POINT)
    args = parser.parse_args()

    image = args.image.read_bytes()
    elf, count = build(image, read_symbols(args.symbols), args.entry)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(elf)
    print(f"{args.out}: image 0x{IMAGE_BASE:08X}-0x{IMAGE_BASE + len(image):08X}, entry 0x{args.entry:08X}, {count} symbol(s)")


if __name__ == "__main__":
    main()
