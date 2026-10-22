"""Pure3D (.p3d) chunk trees: parse, edit and write back byte-for-byte.

Chunks are little-endian: u32 id, u32 header size (12 + the chunk's own data, before the
children), u32 total size (header + children). The file is one root chunk (id 0xFF443350,
"P3D\\xff"). Nothing in a chunk points at file offsets, so a chunk can grow or shrink as long as
its ancestors' sizes are updated, which serialize() does.

    import p3d
    root = p3d.parse(data)
    for chunk, parents in root.walk(): ...
    data2 = root.serialize()          # == data when nothing was changed
"""
import struct

ROOT_ID = 0xFF443350

# Chunk ids seen in Crash of the Titans (names from Radical's Pure3D, as far as known).
NAMES = {
    0xFF443350: "root", 0x19000: "texture", 0x19001: "image", 0x19002: "image data", 0x19005: "sprite",
    0x11000: "shader", 0x11002: "shader texture param", 0x11003: "shader int param",
    0x11004: "shader float param", 0x11005: "shader colour param",
    0x10000: "mesh", 0x10001: "skin", 0x10020: "primitive group", 0x10002: "old primitive group",
    0x10003: "bounding box", 0x10004: "bounding sphere", 0x10005: "positions", 0x10006: "normals",
    0x10007: "uvs", 0x10008: "colours", 0x1000A: "indices", 0x1000B: "matrix list",
    0x1000C: "weights", 0x1000D: "matrix palette", 0x10010: "normal codes",
    0x10011: "vertex shader name", 0x23000: "skeleton", 0x23001: "skeleton joint",
    0x121000: "animation", 0x1800D: "text bible", 0x1800E: "text language",
}


class Chunk:
    __slots__ = ("id", "header", "children", "trailing")

    def __init__(self, cid, header=b"", children=None, trailing=b""):
        self.id = cid
        self.header = header          # bytes after the 12-byte chunk header, before the children
        self.children = children or []
        self.trailing = trailing      # bytes after the last child that don't parse as chunks

    def size(self):
        return 12 + len(self.header) + sum(c.size() for c in self.children) + len(self.trailing)

    def serialize(self, out=None):
        top = out is None
        if top:
            out = bytearray()
        hsize = 12 + len(self.header)
        out += struct.pack("<3I", self.id, hsize, self.size())
        out += self.header
        for c in self.children:
            c.serialize(out)
        out += self.trailing
        return bytes(out) if top else out

    def walk(self, parents=()):
        """Yields (chunk, parents) depth-first, the chunk itself first."""
        yield self, parents
        for c in self.children:
            yield from c.walk(parents + (self,))

    def find(self, cid):
        return [c for c in self.children if c.id == cid]

    def name(self):
        """Pascal-string name at the start of the header (most named chunks), or ''."""
        if not self.header:
            return ""
        n = self.header[0]
        return self.header[1:1 + n].split(b"\0")[0].decode("latin-1")

    def __repr__(self):
        return f"<Chunk {self.id:#x} {NAMES.get(self.id, '')} {self.name()!r} {len(self.children)} children>"


def _parse(data, offset, end):
    cid, hsize, tsize = struct.unpack_from("<3I", data, offset)
    chunk = Chunk(cid, bytes(data[offset + 12:offset + hsize]))
    pos, stop = offset + hsize, offset + tsize
    while pos + 12 <= stop:
        c_id, c_h, c_t = struct.unpack_from("<3I", data, pos)
        if c_t < 12 or c_h < 12 or c_h > c_t or pos + c_t > stop:
            break
        chunk.children.append(_parse(data, pos, stop))
        pos += c_t
    chunk.trailing = bytes(data[pos:stop])
    return chunk


def parse(data):
    cid, hsize, tsize = struct.unpack_from("<3I", data, 0)
    if cid != ROOT_ID or tsize > len(data) or hsize < 12:
        raise ValueError("not a little-endian Pure3D file")
    root = _parse(data, 0, tsize)
    if tsize != len(data):
        root.trailing += bytes(data[tsize:])  # never seen; kept so serialize() round-trips
    return root


def pascal(data, pos):
    """(string, next position) of a length-prefixed string."""
    n = data[pos]
    return data[pos + 1:pos + 1 + n].split(b"\0")[0].decode("latin-1"), pos + 1 + n
