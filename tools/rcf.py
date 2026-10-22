"""Reads Radical "ATG CORE CEMENT LIBRARY" archives (.rcf), as used by Crash of the Titans (X360).

    python tools/rcf.py list <archive.rcf> [pattern]
    python tools/rcf.py extract <archive.rcf> <pattern> <output dir>

Layout (version 2.1.1.1):
    0x00  "ATG CORE CEMENT LIBRARY", zero-padded to 0x20
    0x20  version bytes 02 01 01 01
    0x24  u32 BE directory offset, directory size, names offset, names size, 0, entry count
    dir   per entry: u32 BE name hash, offset, size; sorted by hash (the game binary-searches it)
    names 12-byte header, then per entry in file-offset order: u32 LE alignment (0x800), u32 LE 0,
          u32 LE name length (with NUL), name, 3 padding bytes, u32 LE timestamp (absent after the
          last one)

The game looks entries up by hash; names are only needed to tell which entry is which. Since the
names are listed in data-offset order, the i-th name belongs to the entry with the i-th smallest
offset. runtime/fs/rcf_overlay.cpp relies on the same mapping.
"""
import fnmatch
import struct
import sys
from pathlib import Path

MAGIC = b"ATG CORE CEMENT LIBRARY"


class Entry:
    __slots__ = ("name", "hash", "offset", "size", "index")

    def __init__(self, name, hash_, offset, size, index):
        self.name, self.hash, self.offset, self.size, self.index = name, hash_, offset, size, index


class Archive:
    def __init__(self, path):
        self.path = Path(path)
        self.file = open(self.path, "rb")
        header = self.file.read(0x3C)
        if not header.startswith(MAGIC):
            raise ValueError(f"{self.path}: not an RCF archive")
        self.dir_offset, self.dir_size, self.names_offset, self.names_size, _, count = \
            struct.unpack(">6I", header[0x24:0x3C])
        self.file.seek(self.dir_offset)
        directory = self.file.read(count * 12)
        raw = [struct.unpack_from(">3I", directory, i * 12) + (i,) for i in range(count)]
        names = self._read_names(count)
        by_offset = sorted(raw, key=lambda e: e[1])
        self.entries = [Entry(names[i], h, off, size, index) for i, (h, off, size, index) in enumerate(by_offset)]
        self.by_name = {e.name.lower(): e for e in self.entries}

    def _read_names(self, count):
        self.file.seek(self.names_offset)
        data = self.file.read(self.names_size)
        pos, names = 12, []
        while len(names) < count:
            _alignment, _zero, length = struct.unpack_from("<3I", data, pos)
            pos += 12
            names.append(data[pos:pos + length].split(b"\0")[0].decode("latin-1"))
            pos += length + 3 + 4  # padding, timestamp
        return names

    def find(self, name):
        return self.by_name.get(name.replace("/", "\\").lower())

    def read(self, entry):
        self.file.seek(entry.offset)
        return self.file.read(entry.size)

    def match(self, pattern):
        pattern = pattern.replace("/", "\\").lower()
        return [e for e in self.entries if fnmatch.fnmatchcase(e.name.lower(), pattern)]


def main(argv):
    if len(argv) < 2 or argv[0] not in ("list", "extract"):
        print(__doc__)
        return 1
    archive = Archive(argv[1])
    if argv[0] == "list":
        entries = archive.match(argv[2]) if len(argv) > 2 else archive.entries
        for e in entries:
            print(f"{e.offset:#012x} {e.size:>10}  {e.name}")
        print(f"{len(entries)} de {len(archive.entries)} entradas")
        return 0
    if len(argv) < 4:
        print(__doc__)
        return 1
    out = Path(argv[3])
    entries = archive.match(argv[2])
    for e in entries:
        target = out / Path(*e.name.split("\\"))
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(archive.read(e))
    print(f"{len(entries)} arquivo(s) extraído(s) para {out}")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main(sys.argv[1:]))
