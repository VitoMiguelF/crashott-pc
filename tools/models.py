"""Exports the game's 3D models to glTF 2.0 (.glb, opens in Blender) and puts edited vertices back.

    python tools/models.py export                               # every distinct mesh -> .glb
    python tools/models.py find TEXT                            # search meshes/skins by name
    python tools/models.py preview PACKAGE.glb [--out x.png]    # quick software render (PNG)
    python tools/models.py build --mod NAME                     # edited .glb -> mods/NAME/rcf/...
    python tools/models.py build --mod NAME --chain             # on top of mods/NAME/rcf (textures.py build)

Run `python tools/assets.py extract` and `python tools/textures.py export` first (textures that
a package uses but doesn't contain are taken from the texture export).

Output (game data: private/, never publish it):
    private/assets/models/<category>/<package>.glb   the meshes of the package that no more
                                         representative package has: every distinct mesh (same
                                         bytes) is exported once, from the character's own package
                                         rather than the cutscene copies (category as for the
                                         textures: personagens/<who>, fases/<level>, objetos,
                                         efeitos, interface, cinematicas, outros), with textures
                                         embedded, skeletons and skins
    private/assets/models/animacoes/<skeleton>.glb   the character's model with every distinct
                                         animation for that skeleton (gameplay and cutscenes)
    private/assets/models/indice.csv     file, mesh, kind (mesh/skin/anim), skeleton, prim groups,
                                         vertices, triangles, textures, uses, packages
    private/assets/models/models.json    per file: its package and, per mesh, every package that
                                         has the same mesh (for build)

Pure3D layout (little-endian chunks, tools/p3d.py):
    0x10000 mesh: name, u32 version, u32 prim group count
    0x10001 skin: name, u32 version, skeleton name, u32 prim group count
    0x10020 primitive group: u32 version, shader name, u32 primitive type (0 = triangle list, the
            only one used), vertex format flags, vertex count, index count, matrix count, ...
        0x10005 positions (u32 count, float3), 0x10006 normals (float3), 0x10007 uvs (u32 count,
        u32 channel, float2), 0x10008 colours (u32 count, BGRA bytes), 0x1000A indices (u32),
        0x1000B matrix list (4 palette slots per vertex, bytes), 0x1000C weights (float3, the 4th
        is 1 - sum), 0x1000D matrix palette (skeleton joint per slot), 0x10003 bounding box,
        0x10004 bounding sphere
    0x23000 skeleton: name, u32 version, u32 joint count; 0x23001 joint: name, u32 parent, 4x4
            rest pose relative to the parent (row-vector, translation in the last row)
    0x11000 shader: name, version, pddi shader name, ...; 0x11002 texture param "TEX" -> texture
            name (a 0x19000 texture of the same or another package)
Pure3D is left-handed (Direct3D); glTF is right-handed: z is negated. Triangles are clockwise
front faces, as the game's normals say, for meshes and skins alike; mirroring z doesn't change
how they wind on screen, so each triangle's index order is reversed into glTF's counter-clockwise
(a same-topology build doesn't read the indices: it keeps the game's).

Putting models back (`build`): edit the .glb in Blender (move/sculpt vertices, edit UVs; don't
add or delete vertices or faces), export glTF 2.0 with the second UV map kept, and put it in
mods/NAME/models/<same path>.glb. The second UV map (TEXCOORD_1) of every exported vertex
holds (vertex index, prim group index): build uses it to put each edited position, normal and UV
back in its original slot, so the vertex order Blender produces doesn't matter. Topology,
skinning and materials are kept as in the game; bounding boxes/spheres are recomputed. The edit
goes to every package that has the same mesh; the patched packages go to
mods/NAME/rcf/<archive>/<package>, served by the per-entry .rcf overlay.

Another topology (subdivided, remeshed, new faces): remove the second UV map (TEXCOORD_1) of that
mesh. build then rebuilds the mesh's prim groups from the glTF: positions, normals, UVs, colours,
triangles and, for skins, JOINTS_0/WEIGHTS_0 (joints matched by node name to the skeleton, the 4
strongest influences kept). Each primitive goes to the groups of the shader its material is named
after (as exported; or the n-th group when the counts match), split when a group would pass
GROUP_MAX_VERTICES vertices or GROUP_MAX_MATRICES joints. What the glTF lacks (normals, UVs,
colours, weights), the per-vertex normal codes (0x10010) and the blend shapes of cutscene faces
(0x121305: vertex offsets per target) are taken from the nearest point of the original surface.
A shader without a primitive keeps its groups. A mesh whose second UV map is still there takes the
same-topology path above, byte for byte as before.

Textures and models in one mod: run textures.py build --mod NAME, then models.py build --mod NAME
--chain. With --chain a package already in mods/NAME/rcf is the base: its meshes and skins are put
back to the game's first (so running it again gives the same bytes), then the model edits go in;
everything else in it (textures) is kept. Without --chain build starts from the game's packages.
"""
import argparse
import csv
import hashlib
import io
import json
import math
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
import textures  # noqa: E402

ROOT = assets.ROOT
MODELS = assets.ASSETS / "models"
LISTING = MODELS / "indice.csv"
TABLE = MODELS / "models.json"

MESH, SKIN, PRIMGROUP = 0x10000, 0x10001, 0x10020
POSITIONS, NORMALS, UVS, COLOURS, INDICES = 0x10005, 0x10006, 0x10007, 0x10008, 0x1000A
MATRIX_LIST, WEIGHTS, MATRIX_PALETTE = 0x1000B, 0x1000C, 0x1000D
BBOX, BSPHERE = 0x10003, 0x10004
SKELETON, JOINT = 0x23000, 0x23001
SHADER, SHADER_TEXTURE, SHADER_INT = 0x11000, 0x11002, 0x11003
TEXTURE = 0x19000

MIRROR = np.diag([1.0, 1.0, -1.0, 1.0])


# --- Reading Pure3D -------------------------------------------------------------------------------

def array_chunk(chunk, width, dtype="<f4", skip=4):
    count = struct.unpack_from("<I", chunk.header, 0)[0]
    return np.frombuffer(chunk.header, dtype=dtype, count=count * width, offset=skip).reshape(count, width)


class PrimGroup:
    def __init__(self, chunk):
        self.chunk = chunk
        self.shader, pos = p3d.pascal(chunk.header, 4)
        (self.prim_type, self.vertex_flags, self.vertex_count, self.index_count,
         self.matrix_count) = struct.unpack_from("<5I", chunk.header, pos)
        self.positions = self.normals = self.colours = self.matrices = self.weights = self.palette = None
        self.uvs = {}
        self.indices = np.zeros(0, np.uint32)
        for c in chunk.children:
            if c.id == POSITIONS:
                self.positions = array_chunk(c, 3)
            elif c.id == NORMALS:
                self.normals = array_chunk(c, 3)
            elif c.id == UVS:
                channel = struct.unpack_from("<I", c.header, 4)[0]
                self.uvs[channel] = array_chunk(c, 2, skip=8)
            elif c.id == COLOURS:
                self.colours = array_chunk(c, 4, "u1")
            elif c.id == INDICES:
                self.indices = array_chunk(c, 1, "<u4").reshape(-1)
            elif c.id == MATRIX_LIST:
                self.matrices = array_chunk(c, 4, "u1")
            elif c.id == WEIGHTS:
                self.weights = array_chunk(c, 3)
            elif c.id == MATRIX_PALETTE:
                self.palette = array_chunk(c, 1, "<u4").reshape(-1)


class Mesh:
    def __init__(self, chunk):
        self.chunk = chunk
        self.name = chunk.name()
        self.skin = chunk.id == SKIN
        self.skeleton = ""
        if self.skin:
            _name, pos = p3d.pascal(chunk.header, 0)
            self.skeleton, _ = p3d.pascal(chunk.header, pos + 4)
        self.groups = [PrimGroup(c) for c in chunk.children if c.id == PRIMGROUP]


class Skeleton:
    def __init__(self, chunk):
        self.name = chunk.name()
        self.joints = []  # (name, parent, local 4x4 row-vector matrix)
        for j in chunk.find(JOINT):
            name, pos = p3d.pascal(j.header, 0)
            parent = struct.unpack_from("<I", j.header, pos)[0]
            m = np.array(struct.unpack_from("<16f", j.header, pos + 4), dtype=np.float64).reshape(4, 4)
            self.joints.append((name, parent, m))

    def world(self):
        out = []
        for i, (_n, parent, m) in enumerate(self.joints):
            out.append(m @ out[parent] if parent < i else m.copy())
        return out


def shader_textures(root):
    out = {}
    for c in root.children:
        if c.id != SHADER:
            continue
        for p in c.find(SHADER_TEXTURE):
            if p.header[:4] == b"TEX\0":
                out[c.name()] = p3d.pascal(p.header, 4)[0]
                break
    return out


def package_textures(root):
    """{texture name (lowercase): PIL image of the base level} for the textures in a package."""
    out = {}
    for c in root.children:
        if c.id != TEXTURE:
            continue
        imgs = c.find(textures.IMAGE)
        if not imgs:
            continue
        f = textures.image_fields(imgs[0])
        _d, payload = textures.image_data(imgs[0])
        if payload is None:
            continue
        try:
            out[c.name().lower()] = textures.open_stored(payload, textures.stored_kind(payload, f["format"]))
        except Exception:  # noqa: BLE001
            pass
    return out


_GLOBAL_TEXTURES = None


def global_texture(name):
    """Base level of a texture from any package (via the texture export), or None."""
    global _GLOBAL_TEXTURES
    if _GLOBAL_TEXTURES is None:
        _GLOBAL_TEXTURES = textures.texture_files()
    path = _GLOBAL_TEXTURES.get(name.lower())
    if path is None:
        return None
    try:
        img = Image.open(path)
        img.load()
        return img
    except Exception:  # noqa: BLE001
        return None


# The matrix list packs the 4 palette slots of a vertex as one u32 with the first influence in the
# top byte, so in the little-endian file the bytes are slot 3, 2, 1, 0 (checked against the
# skeleton: the main joint is then the one next to the vertex).
MATRIX_ORDER = [3, 2, 1, 0]


# --- Animations ------------------------------------------------------------------------------------------

ANIMATION, ANIM_GROUP_LIST, ANIM_GROUP = 0x121000, 0x121002, 0x121001
CH_VECTOR1, CH_VECTOR2, CH_VECTOR3, CH_QUATERNION, CH_COMPRESSED_QUAT = 0x121102, 0x121103, 0x121104, 0x121105, 0x121112


def _keys(header, pos):
    n = struct.unpack_from("<I", header, pos)[0]
    frames = np.frombuffer(header, "<u2", n, pos + 4).astype(np.float64)
    return n, frames, pos + 4 + 2 * n


def read_channel(chunk):
    """(param, frames, values) of a transform channel; values are float3 (TRAN) or quaternions
    (x, y, z, w; ROT). None for other channel kinds."""
    h = chunk.header
    param = h[4:8].rstrip(b"\0").decode("latin-1")
    if chunk.id == CH_VECTOR3:
        n, frames, pos = _keys(h, 8)
        return param, frames, np.frombuffer(h, "<f4", n * 3, pos).reshape(n, 3).astype(np.float64)
    if chunk.id in (CH_VECTOR1, CH_VECTOR2):
        # u16 mapping (CH_VECTOR1: the animated axis; CH_VECTOR2: the constant axis), float3
        # constants, then the keys of the animated axis/axes.
        mapping = struct.unpack_from("<H", h, 8)[0]
        const = np.array(struct.unpack_from("<3f", h, 10), np.float64)
        width = 1 if chunk.id == CH_VECTOR1 else 2
        n, frames, pos = _keys(h, 22)
        vals = np.frombuffer(h, "<f4", n * width, pos).reshape(n, width).astype(np.float64)
        out = np.tile(const, (n, 1))
        axes = [mapping] if width == 1 else [a for a in range(3) if a != mapping]
        for k, a in enumerate(axes):
            out[:, a] = vals[:, k]
        return param, frames, out
    if chunk.id == CH_QUATERNION:
        n, frames, pos = _keys(h, 8)
        q = np.frombuffer(h, "<f4", n * 4, pos).reshape(n, 4).astype(np.float64)  # w, x, y, z
        return param, frames, q[:, [1, 2, 3, 0]]
    if chunk.id == CH_COMPRESSED_QUAT:
        # 3 x int16 (x, y, z) / 32767; w = +sqrt(1 - x² - y² - z²).
        n, frames, pos = _keys(h, 8)
        xyz = np.frombuffer(h, "<i2", n * 3, pos).reshape(n, 3).astype(np.float64) / 32767.0
        w = np.sqrt(np.clip(1.0 - (xyz ** 2).sum(1), 0.0, 1.0))
        return param, frames, np.c_[xyz, w]
    return None


class Animation:
    def __init__(self, chunk):
        _version = struct.unpack_from("<I", chunk.header, 0)[0]
        self.name, pos = p3d.pascal(chunk.header, 4)
        self.kind = chunk.header[pos:pos + 4].decode("latin-1")
        self.frames, self.fps = struct.unpack_from("<2f", chunk.header, pos + 4)
        self.fps = self.fps or 30.0
        self.groups = {}  # joint name -> {"TRAN": (frames, values), "ROT": (frames, quats)}
        for gl in chunk.find(ANIM_GROUP_LIST):
            for g in gl.find(ANIM_GROUP):
                joint, _ = p3d.pascal(g.header, 4)
                channels = {}
                for c in g.children:
                    ch = read_channel(c)
                    if ch and ch[0] in ("TRAN", "ROT"):
                        channels[ch[0]] = (ch[1], ch[2])
                if channels:
                    self.groups[joint] = channels


def quat_to_matrix(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def sample(frames, values, t, quat=False):
    if len(frames) == 1 or t <= frames[0]:
        return values[0]
    if t >= frames[-1]:
        return values[-1]
    k = int(np.searchsorted(frames, t, side="right")) - 1
    a = (t - frames[k]) / max(frames[k + 1] - frames[k], 1e-9)
    v0, v1 = values[k], values[k + 1]
    if quat and np.dot(v0, v1) < 0:
        v1 = -v1
    v = v0 * (1 - a) + v1 * a
    return v / np.linalg.norm(v) if quat else v


def posed_locals(skeleton, anim, frame, conjugate):
    """Local row-vector matrices of the skeleton at `frame` of `anim` (rest pose where the
    animation has no channel)."""
    out = []
    for name, _parent, rest in skeleton.joints:
        m = rest.copy()
        ch = anim.groups.get(name, {})
        if "ROT" in ch:
            q = sample(*ch["ROT"], frame, quat=True)
            r = quat_to_matrix(q)
            m[:3, :3] = r if conjugate else r.T
        if "TRAN" in ch:
            m[3, :3] = sample(*ch["TRAN"], frame)
        out.append(m)
    return out


# --- glTF writing ---------------------------------------------------------------------------------

class Gltf:
    def __init__(self):
        self.doc = {"asset": {"version": "2.0", "generator": "CrashRecomp tools/models.py"},
                    "scene": 0, "scenes": [{"nodes": []}], "nodes": [], "meshes": [], "buffers": [],
                    "bufferViews": [], "accessors": [], "materials": [], "textures": [], "images": [],
                    "samplers": [{"wrapS": 10497, "wrapT": 10497}], "skins": []}
        self.bin = bytearray()

    def view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        v = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            v["target"] = target
        self.bin += data
        self.doc["bufferViews"].append(v)
        return len(self.doc["bufferViews"]) - 1

    def accessor(self, array, kind, component, normalized=False, target=None, minmax=False):
        array = np.ascontiguousarray(array)
        a = {"bufferView": self.view(array.tobytes(), target), "componentType": component,
             "count": int(array.shape[0]), "type": kind}
        if normalized:
            a["normalized"] = True
        if minmax:
            a["min"] = [float(x) for x in array.min(0)]
            a["max"] = [float(x) for x in array.max(0)]
        self.doc["accessors"].append(a)
        return len(self.doc["accessors"]) - 1

    def image(self, img, name):
        if img.mode not in ("RGB", "RGBA"):
            img = img.convert("RGBA")
        out = io.BytesIO()
        img.save(out, "PNG")
        self.doc["images"].append({"bufferView": self.view(out.getvalue()), "mimeType": "image/png", "name": name})
        self.doc["textures"].append({"sampler": 0, "source": len(self.doc["images"]) - 1})
        return len(self.doc["textures"]) - 1

    def node(self, n, root=True):
        self.doc["nodes"].append(n)
        i = len(self.doc["nodes"]) - 1
        if root:
            self.doc["scenes"][0]["nodes"].append(i)
        return i

    def glb(self):
        doc = {k: v for k, v in self.doc.items() if v != []}
        doc["buffers"] = [{"byteLength": len(self.bin)}]
        js = json.dumps(doc, separators=(",", ":")).encode()
        js += b" " * (-len(js) % 4)
        binary = bytes(self.bin) + b"\0" * (-len(self.bin) % 4)
        out = struct.pack("<3I", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(binary))
        out += struct.pack("<2I", len(js), 0x4E4F534A) + js
        out += struct.pack("<2I", len(binary), 0x004E4942) + binary
        return out


def to_gltf_matrix(m):
    """Pure3D row-vector matrix -> glTF matrix array (column-major, column-vector, z mirrored)."""
    return [float(x) for x in (MIRROR @ m @ MIRROR).reshape(-1)]


def quat_from_matrix(m):
    """(x, y, z, w) of a column-vector rotation matrix."""
    t = m[0, 0] + m[1, 1] + m[2, 2]
    if t > 0:
        s = math.sqrt(t + 1.0) * 2
        q = [(m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, 0.25 * s]
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = math.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        q = [0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s, (m[2, 1] - m[1, 2]) / s]
    elif m[1, 1] > m[2, 2]:
        s = math.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        q = [(m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s, (m[0, 2] - m[2, 0]) / s]
    else:
        s = math.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        q = [(m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s, (m[1, 0] - m[0, 1]) / s]
    q = np.array(q)
    return q / np.linalg.norm(q)


def to_gltf_trs(m):
    """Pure3D row-vector matrix -> glTF node translation/rotation/scale (z mirrored). Joints are
    written as TRS because glTF only animates TRS nodes."""
    c = (MIRROR @ m @ MIRROR).T  # column-vector
    scale = np.linalg.norm(c[:3, :3], axis=0)
    rot = quat_from_matrix(c[:3, :3] / np.maximum(scale, 1e-12))
    out = {"translation": [float(x) for x in c[:3, 3]], "rotation": [float(x) for x in rot]}
    if np.abs(scale - 1).max() > 1e-5:
        out["scale"] = [float(x) for x in scale]
    return out


def match_skeleton(anim, skeletons):
    """The skeleton (of `skeletons`) whose joints the animation's groups name, or None."""
    groups = set(anim.groups)
    best, best_cover = None, 0.0
    for sk in skeletons:
        names = {n for n, _p, _m in sk.joints}
        cover = len(groups & names) / max(len(groups), 1)
        if cover > best_cover:
            best, best_cover = sk, cover
    return best if best_cover >= 0.6 else None


def add_animation(g, anim, sk, nodes):
    """glTF animation (LINEAR) of a Pure3D PTRN animation on the joint nodes of `sk`."""
    samplers, channels = [], []
    index = {n: i for i, (n, _p, _m) in enumerate(sk.joints)}
    for joint, ch in anim.groups.items():
        if joint not in index:
            continue
        node = nodes[index[joint]]
        for param, path in (("TRAN", "translation"), ("ROT", "rotation")):
            if param not in ch:
                continue
            frames, values = ch[param]
            times = (frames / anim.fps).astype(np.float32).reshape(-1, 1)
            if param == "TRAN":
                out = (values * [1, 1, -1]).astype(np.float32)
                kind = "VEC3"
            else:
                # Pure3D's quaternion is the column-vector rotation (its row-vector matrix is the
                # transpose); mirroring z turns (x, y, z, w) into (-x, -y, z, w).
                out = (values * [-1, -1, 1, 1]).astype(np.float32)
                for k in range(1, len(out)):  # shortest path between keys
                    if np.dot(out[k - 1], out[k]) < 0:
                        out[k] = -out[k]
                kind = "VEC4"
            samplers.append({"input": g.accessor(times, "SCALAR", 5126, minmax=True),
                             "output": g.accessor(out, kind, 5126), "interpolation": "LINEAR"})
            channels.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": path}})
    if channels:
        name = anim.name[5:] if anim.name.startswith("PTRN_") else anim.name
        g.doc.setdefault("animations", []).append({"name": name, "samplers": samplers, "channels": channels})
        return True
    return False


def package_animations(root):
    return [a for a in (Animation(c) for c in root.children if c.id == ANIMATION) if a.kind == "PTRN" and a.groups]


def export_package(archive, entry, data, animations=None, only_skeleton=None, only_meshes=None):
    """(.glb bytes, listing rows) of a package's meshes (those whose hash is in `only_meshes`, if
    given), their skeletons and `animations` (Animation objects, from any package) that match
    them. `only_skeleton` keeps just the skins of that skeleton (models for animations)."""
    root = p3d.parse(data)
    meshes = [Mesh(c) for c in root.children if c.id in (MESH, SKIN)]
    if only_meshes is not None:
        meshes = [m for m in meshes if mesh_hash(m.chunk) in only_meshes]
    if only_skeleton:
        meshes = [m for m in meshes if m.skin and m.skeleton == only_skeleton]
    if not meshes:
        return None, []
    skeletons = {c.name(): Skeleton(c) for c in root.children if c.id == SKELETON}
    shaders = shader_textures(root)
    local_textures = package_textures(root)
    g = Gltf()
    materials, texture_ids, rows = {}, {}, []

    def material(shader):
        if shader in materials:
            return materials[shader]
        m = {"name": shader, "pbrMetallicRoughness": {"metallicFactor": 0.0, "roughnessFactor": 1.0}}
        tex_name = shaders.get(shader)
        if tex_name:
            key = tex_name.lower()
            if key not in texture_ids:
                img = local_textures.get(key) or global_texture(tex_name)
                texture_ids[key] = g.image(img, tex_name) if img is not None else None
                if img is not None and img.mode in ("RGBA", "P", "LA"):
                    alpha = img.convert("RGBA").getchannel("A")
                    texture_ids[key + "#alpha"] = alpha.getextrema()[0] < 250
            t = texture_ids.get(key)
            if t is not None:
                m["pbrMetallicRoughness"]["baseColorTexture"] = {"index": t}
                if texture_ids.get(key + "#alpha"):
                    m["alphaMode"] = "MASK"
                    m["alphaCutoff"] = 0.5
                    m["doubleSided"] = True
        g.doc["materials"].append(m)
        materials[shader] = len(g.doc["materials"]) - 1
        return materials[shader]

    joint_nodes = {}

    def skeleton_nodes(sk):
        if sk.name in joint_nodes:
            return joint_nodes[sk.name]
        nodes = []
        for i, (name, parent, m) in enumerate(sk.joints):
            nodes.append(g.node(dict({"name": name}, **to_gltf_trs(m)), root=parent >= i))
        for i, (_name, parent, _m) in enumerate(sk.joints):
            if parent < i:
                g.doc["nodes"][nodes[parent]].setdefault("children", []).append(nodes[i])
        joint_nodes[sk.name] = nodes
        return nodes

    for mesh in meshes:
        sk = skeletons.get(mesh.skeleton) if mesh.skin else None
        world = sk.world() if sk else None
        prims, verts, tris = [], 0, 0
        used_textures = set()
        for gi, grp in enumerate(mesh.groups):
            if grp.positions is None or len(grp.indices) == 0 or grp.prim_type != 0:
                continue
            n = len(grp.positions)
            pos = grp.positions.astype(np.float32) * np.array([1, 1, -1], np.float32)
            attrs = {"POSITION": g.accessor(pos, "VEC3", 5126, target=34962, minmax=True)}
            if grp.normals is not None and len(grp.normals) == n:
                nrm = grp.normals.astype(np.float32) * np.array([1, 1, -1], np.float32)
                length = np.linalg.norm(nrm, axis=1, keepdims=True)
                nrm = np.where(length > 1e-6, nrm / np.maximum(length, 1e-6), np.array([0, 1, 0], np.float32))
                attrs["NORMAL"] = g.accessor(nrm.astype(np.float32), "VEC3", 5126, target=34962)
            if 0 in grp.uvs and len(grp.uvs[0]) == n:
                attrs["TEXCOORD_0"] = g.accessor(grp.uvs[0].astype(np.float32) * np.array([1, -1], np.float32),
                                                 "VEC2", 5126, target=34962)
            # Slot back-reference for `build`: (vertex index, prim group index).
            ref = np.stack([np.arange(n, dtype=np.float32), np.full(n, gi, np.float32)], 1)
            attrs["TEXCOORD_1"] = g.accessor(ref, "VEC2", 5126, target=34962)
            if grp.colours is not None and len(grp.colours) == n:
                rgba = grp.colours[:, [2, 1, 0, 3]].astype(np.uint8)
                attrs["COLOR_0"] = g.accessor(rgba, "VEC4", 5121, normalized=True, target=34962)
            if sk and grp.matrices is not None and grp.weights is not None and grp.palette is not None:
                slots = np.minimum(grp.matrices[:, MATRIX_ORDER], len(grp.palette) - 1)
                joints = grp.palette[slots].astype(np.uint16)
                joints = np.minimum(joints, len(sk.joints) - 1)
                w = np.zeros((n, 4), np.float32)
                w[:, :3] = grp.weights
                w[:, 3] = 1.0 - w[:, :3].sum(1)
                w = np.clip(w, 0, 1)
                w /= np.maximum(w.sum(1, keepdims=True), 1e-6)
                attrs["JOINTS_0"] = g.accessor(joints, "VEC4", 5123, target=34962)
                attrs["WEIGHTS_0"] = g.accessor(w.astype(np.float32), "VEC4", 5126, target=34962)
            idx = grp.indices.astype(np.uint32)
            if idx.max(initial=0) >= n or len(idx) % 3:
                continue
            # Clockwise front faces (the game's normals agree) -> glTF's counter-clockwise ones.
            idx = idx.reshape(-1, 3)[:, [0, 2, 1]].reshape(-1)
            prim = {"attributes": attrs, "indices": g.accessor(idx, "SCALAR", 5125, target=34963),
                    "material": material(grp.shader), "mode": 4}
            prims.append(prim)
            verts += n
            tris += len(idx) // 3
            if shaders.get(grp.shader):
                used_textures.add(shaders[grp.shader])
        if not prims:
            continue
        g.doc["meshes"].append({"name": mesh.name, "primitives": prims})
        node = {"name": mesh.name, "mesh": len(g.doc["meshes"]) - 1}
        if sk:
            nodes = skeleton_nodes(sk)
            ibm = np.array([np.linalg.inv(MIRROR @ w @ MIRROR).reshape(-1) for w in world], np.float32)
            g.doc["skins"].append({"name": sk.name, "joints": nodes,
                                   "inverseBindMatrices": g.accessor(ibm, "MAT4", 5126)})
            node["skin"] = len(g.doc["skins"]) - 1
        g.node(node)
        rows.append({"mesh": mesh.name, "kind": "skin" if mesh.skin else "mesh", "skeleton": mesh.skeleton,
                     "groups": len(prims), "vertices": verts, "triangles": tris,
                     "textures": ";".join(sorted(used_textures))})
    if not g.doc["meshes"]:
        return None, []
    animations = list(animations or [])
    used = [sk for sk in skeletons.values() if sk.name in joint_nodes]
    animated = 0
    for anim in animations:
        sk = match_skeleton(anim, used)
        if sk and add_animation(g, anim, sk, joint_nodes[sk.name]):
            animated += 1
    if animated:
        for r in rows:
            r["animations"] = animated
    return g.glb(), rows


def mesh_hash(chunk):
    return hashlib.sha1(chunk.serialize()).hexdigest()


def package_rank(key):
    archive, entry = key.split("/", 1)
    return assets.category(archive, entry)[1], len(entry), assets.readable(archive, entry).lower()


def skeleton_library(entries):
    """{skeleton name: (archive, entry, Skeleton)}: for each skeleton, a package that has skins
    for it (the character's own package/Characters_* first)."""
    lib = {}
    def rank(e):
        name = assets.readable(e["archive"], e["entry"]).lower()
        return (0 if "\\characters_" in name else 1 if name.startswith("package") else 2, name)
    for e in sorted(entries, key=rank):
        data = assets.raw_path(e["archive"], e["entry"]).read_bytes()
        if b"\x00\x30\x02\x00" not in data:  # no skeleton chunk id (0x23000) anywhere
            continue
        root = p3d.parse(data)
        skinned = {Mesh(c).skeleton for c in root.children if c.id == SKIN}
        for c in root.children:
            if c.id == SKELETON and c.name() in skinned and c.name() not in lib:
                lib[c.name()] = (e["archive"], e["entry"], Skeleton(c))
    return lib


def load_table():
    if TABLE.exists():
        table = json.loads(TABLE.read_text(encoding="utf-8"))
        if table.get("version") == 2:
            return table
    return {"version": 2, "files": {}}


def unique_paths(items):
    """{key: relative .glb path} for (key, folder, stem) items, a suffix telling apart equal names."""
    out, taken = {}, set()
    for key, folder, stem, hint in items:
        name = f"{folder}/{stem}"
        if name.lower() in taken:
            name = f"{folder}/{stem} ({hint})"
        k = 2
        while name.lower() in taken:
            name, k = f"{folder}/{stem} ({hint} {k})", k + 1
        taken.add(name.lower())
        out[key] = name + ".glb"
    return out


def cmd_export(_args):
    """One .glb per package that "owns" meshes: every distinct mesh (same bytes = same mesh) goes
    to the most representative package that has it (assets.category: the character's own package
    before the cutscene copies), so no mesh is exported twice. Animations: one .glb per skeleton
    with the character's model and every distinct animation for it."""
    index = assets.load_index()
    every = sorted((e for e in index["entries"].values() if e["type"] == "pure3d"), key=lambda e: e["entry"])
    for e in every:
        e["converted"] = [c for c in e.get("converted", []) if not c.startswith("models/")]
    uses = defaultdict(list)    # mesh hash -> [(package key, mesh name)]
    anims = {}                  # animation hash -> (package key, Animation)
    anim_count = 0
    for i, e in enumerate(every):
        key = f"{e['archive']}/{e['entry']}"
        data = assets.raw_path(e["archive"], e["entry"]).read_bytes()
        has_anim = b"PTRN" in data
        if b"\x00\x00\x01\x00" not in data and b"\x01\x00\x01\x00" not in data and not has_anim:
            continue  # no mesh/skin chunk id anywhere
        for c in p3d.parse(data).children:
            if c.id in (MESH, SKIN):
                uses[mesh_hash(c)].append((key, c.name()))
            elif c.id == ANIMATION and has_anim:
                a = Animation(c)
                if a.kind == "PTRN" and a.groups:
                    anim_count += 1
                    anims.setdefault(mesh_hash(c), (key, a))
        if (i + 1) % 300 == 0:
            print(f"  {i + 1}/{len(every)} pacotes...")
    owned = defaultdict(set)
    for h, us in uses.items():
        owned[min({k for k, _n in us}, key=package_rank)].add(h)
    items = []
    for key in sorted(owned, key=package_rank):
        archive, entry = key.split("/", 1)
        path = assets.readable(archive, entry).split("\\")
        hint = path[-2] if len(path) > 2 else Path(entry).stem
        items.append((key, assets.category(archive, entry)[0], safe_name(Path(path[-1]).stem), hint))
    paths = unique_paths(items)
    table = {"version": 2, "files": {}, "animations": {}}
    rows_all = []
    for n, key in enumerate(sorted(owned, key=lambda k: paths[k])):
        archive, entry = key.split("/", 1)
        glb, rows = export_package(archive, entry, assets.raw_path(archive, entry).read_bytes(),
                                   only_meshes=owned[key])
        if glb is None:
            continue
        rel = paths[key]
        out = MODELS / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(glb)
        meshes = {}
        for h in owned[key]:
            pkgs = sorted({k for k, _n in uses[h]})
            meshes[h] = {"name": uses[h][0][1], "uses": pkgs}
        table["files"][rel] = {"package": key, "meshes": meshes}
        by_name = defaultdict(list)
        for h, m in meshes.items():
            by_name[m["name"]].append(m)
        for r in rows:
            m = by_name.get(r["mesh"], [{"uses": [key]}])[0]
            r.update(file=rel, uses=len(m["uses"]),
                     packages=";".join(assets.readable(*k.split("/", 1)) for k in m["uses"]))
        rows_all += rows
        for k in {k for h in owned[key] for k, _n in uses[h]}:
            assets.record_converted(index, *k.split("/", 1), [f"models/{rel}"])
        if (n + 1) % 100 == 0:
            print(f"  {n + 1}/{len(owned)} arquivos...")

    print(f"{anim_count} animações ({len(anims)} diferentes): procurando os esqueletos...")
    lib = skeleton_library(every)
    groups, lost = defaultdict(list), 0
    for h, (key, a) in sorted(anims.items(), key=lambda kv: (kv[1][1].name.lower(), kv[1][0])):
        sk = match_skeleton(a, [v[2] for v in lib.values()])
        if sk is None:
            lost += 1
            continue
        groups[sk.name].append((key, a))
    for sk_name, group in sorted(groups.items()):
        names = defaultdict(int)
        for _key, a in group:
            names[a.name.lower()] += 1
        renamed = []
        for key, a in group:
            if names[a.name.lower()] > 1:  # same name, different animation: say where it is from
                a.name = f"{a.name} ({Path(assets.readable(*key.split('/', 1))).stem})"
            renamed.append(a)
        archive, model_entry, _sk = lib[sk_name]
        glb, _rows = export_package(archive, model_entry, assets.raw_path(archive, model_entry).read_bytes(),
                                    animations=renamed, only_skeleton=sk_name)
        if glb is None:
            continue
        rel = f"animacoes/{safe_name(sk_name)}.glb"
        (MODELS / rel).parent.mkdir(parents=True, exist_ok=True)
        (MODELS / rel).write_bytes(glb)
        table["animations"][rel] = {"skeleton": sk_name, "model": f"{archive}/{model_entry}",
                                    "animations": sorted({key for key, _a in group})}
        rows_all.append({"file": rel, "mesh": sk_name, "kind": "anim", "skeleton": sk_name, "groups": 0,
                         "vertices": 0, "triangles": 0, "textures": f"modelo de {assets.readable(archive, model_entry)}",
                         "animations": len(group), "uses": len(group),
                         "packages": ";".join(sorted({assets.readable(*key.split("/", 1)) for key, _a in group}))})
        for key in {key for key, _a in group}:
            assets.record_converted(index, *key.split("/", 1), [f"models/{rel}"])
    TABLE.write_text(json.dumps(table, indent=0, ensure_ascii=False), encoding="utf-8")
    with open(LISTING, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, ["file", "mesh", "kind", "skeleton", "groups", "vertices", "triangles", "textures",
                               "animations", "uses", "packages"], extrasaction="ignore")
        w.writerow({"file": "arquivo", "mesh": "malha", "kind": "tipo", "skeleton": "esqueleto", "groups": "grupos",
                    "vertices": "vertices", "triangles": "triangulos", "textures": "texturas",
                    "animations": "animacoes", "uses": "usos", "packages": "pacotes"})
        w.writerows(sorted(rows_all, key=lambda r: (r["file"].lower(), r["mesh"].lower())))
    assets.save_index(index)
    meshes = sum(1 for r in rows_all if r["kind"] != "anim")
    print(f"{sum(len(u) for u in uses.values())} malhas ({len(uses)} diferentes) -> {len(table['files'])} .glb com "
          f"{meshes} malhas; {len(groups)} .glb de animações ({sum(len(g) for g in groups.values())} animações"
          + (f", {lost} sem esqueleto conhecido" if lost else "") + f"); índice: {LISTING.relative_to(ROOT)}")
    return 0


def safe_name(name):
    return re.sub(r'[<>:"/\\|?*]', "_", name)


def cmd_find(args):
    pattern = args.text.lower()
    n = 0
    if LISTING.exists():
        with open(LISTING, newline="", encoding="utf-8") as f:
            for r in csv.DictReader(f):
                if any(pattern in (r[k] or "").lower() for k in ("malha", "arquivo", "pacotes", "texturas")):
                    extra = f"  {r['animacoes']} anim." if r.get("animacoes") else ""
                    print(f"{r['tipo']:4} {r['malha']:40} {r['vertices']:>6} v {r['triangulos']:>6} t  "
                          f"{r['usos']:>3} uso(s)  models/{r['arquivo']}{extra}")
                    n += 1
    print(f"{n} malha(s)")
    return 0


# --- glTF reading (for build and preview) -------------------------------------------------------------

def read_gltf(path):
    data = Path(path).read_bytes()
    if data[:4] == b"glTF":
        length = struct.unpack_from("<I", data, 12)[0]
        doc = json.loads(data[20:20 + length])
        pos = 20 + length
        blen = struct.unpack_from("<I", data, pos)[0]
        buffers = [data[pos + 8:pos + 8 + blen]]
    else:
        import base64
        doc = json.loads(data)
        buffers = []
        for b in doc.get("buffers", []):
            uri = b.get("uri", "")
            if uri.startswith("data:"):
                buffers.append(base64.b64decode(uri.split(",", 1)[1]))
            else:
                buffers.append((Path(path).parent / uri).read_bytes())
    return doc, buffers


COMPONENTS = {5120: ("i1", 127), 5121: ("u1", 255), 5122: ("<i2", 32767), 5123: ("<u2", 65535),
              5125: ("<u4", None), 5126: ("<f4", None)}
WIDTHS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def read_accessor(doc, buffers, index):
    a = doc["accessors"][index]
    dtype, scale = COMPONENTS[a["componentType"]]
    width = WIDTHS[a["type"]]
    out = np.zeros((a["count"], width), np.float64)
    if "bufferView" in a:
        v = doc["bufferViews"][a["bufferView"]]
        buf = buffers[v.get("buffer", 0)]
        item = np.dtype(dtype).itemsize * width
        stride = v.get("byteStride", item)
        start = v.get("byteOffset", 0) + a.get("byteOffset", 0)
        raw = np.frombuffer(buf, np.uint8, count=stride * (a["count"] - 1) + item if a["count"] else 0, offset=start)
        rows = np.lib.stride_tricks.as_strided(raw, (a["count"], item), (stride, 1)) if a["count"] else raw
        out = np.frombuffer(np.ascontiguousarray(rows).tobytes(), dtype).reshape(a["count"], width).astype(np.float64)
        if a.get("normalized") and scale:
            out /= scale
    return out


# --- build (way back) ------------------------------------------------------------------------------------

def set_array_chunk(chunk, array, skip=4):
    chunk.header = chunk.header[:skip] + np.ascontiguousarray(array, dtype="<f4").tobytes()


def set_bounds(chunks, positions):
    lo, hi = positions.min(0), positions.max(0)
    centre = (lo + hi) / 2
    radius = float(np.linalg.norm(positions - centre, axis=1).max())
    for c in chunks:
        if c.id == BBOX:
            c.header = struct.pack("<6f", *lo, *hi)
        elif c.id == BSPHERE:
            c.header = struct.pack("<4f", *centre, radius)


# --- build with another topology (replace_geometry) ---------------------------------------------------------

NORMAL_CODES = 0x10010      # u32 count + 1 byte per vertex: the normal's entry in a table of 256 directions
# Blend shapes (cutscene faces, waves): 0x121305 set > 0x121306 target (u32 0, target, prim group) >
# offsets: u32 0, fourcc, u32 count, count x (u32 vertex of that group, offset).
BLEND_SHAPES, BLEND_TARGET = 0x121305, 0x121306
BLEND_WIDTH = {0x10F00: 3, 0x10F01: 2}  # "POS" float3, "UV0"/"UV1" float2 (0x10F02 "CLR0": not carried)
GROUP_MAX_VERTICES = 65535  # 16-bit indices, to be safe (the game's largest group has 12 216 vertices)
GROUP_MAX_MATRICES = 51     # the largest matrix palette in the game's data (skinning is done on the CPU)


def copy_chunk(c):
    return p3d.Chunk(c.id, c.header, [copy_chunk(k) for k in c.children], c.trailing)


def same_topology(doc, buffers, gm, groups):
    """True when every primitive still has the export's TEXCOORD_1 slot references (whole numbers)
    and there are no more triangles than in the game: build then puts each vertex back in its slot.
    Otherwise (second UV map removed, subdivided, remeshed) the geometry is replaced."""
    tris = 0
    for prim in gm.get("primitives", []):
        attrs = prim["attributes"]
        if "TEXCOORD_1" not in attrs or "POSITION" not in attrs:
            return False
        ref = read_accessor(doc, buffers, attrs["TEXCOORD_1"])
        if np.abs(ref - np.rint(ref)).max(initial=0) > 1e-3:
            return False
        tris += (doc["accessors"][prim["indices"]]["count"] if "indices" in prim else len(ref)) // 3
    return tris <= sum(len(PrimGroup(g).indices) // 3 for g in groups)


def closest_on_triangles(points, a, b, c):
    """(triangle, barycentric weights (n, 3)) of the point of the triangles a, b, c (m, 3) nearest
    to each of `points` (n, 3)."""
    ab, ac = b - a, c - a
    d00, d01, d11 = (ab * ab).sum(1), (ab * ac).sum(1), (ac * ac).sum(1)
    den = d00 * d11 - d01 * d01
    tri = np.zeros(len(points), np.int64)
    bary = np.zeros((len(points), 3))
    step = max(1, 400_000 // max(len(a), 1))
    for s in range(0, len(points), step):
        p = points[s:s + step, None, :]
        ap = p - a
        d20, d21 = (ap * ab).sum(2), (ap * ac).sum(2)
        with np.errstate(divide="ignore", invalid="ignore"):
            v = (d11 * d20 - d01 * d21) / den
            w = (d00 * d21 - d01 * d20) / den
        u = 1 - v - w
        inside = (u >= 0) & (v >= 0) & (w >= 0) & (den > 1e-30)
        weights = np.where(inside[..., None], np.stack([u, v, w], -1), 0.0)
        dist = np.where(inside, ((ap - v[..., None] * ab - w[..., None] * ac) ** 2).sum(2), np.inf)
        for e0, e1, i0, i1 in ((a, b, 0, 1), (b, c, 1, 2), (c, a, 2, 0)):
            ed = e1 - e0
            t = np.clip(((p - e0) * ed).sum(2) / np.maximum((ed * ed).sum(1), 1e-30), 0, 1)
            de = ((p - e0 - t[..., None] * ed) ** 2).sum(2)
            better = de < dist
            dist = np.where(better, de, dist)
            edge = np.zeros_like(weights)
            edge[..., i0], edge[..., i1] = 1 - t, t
            weights = np.where(better[..., None], edge, weights)
        k = dist.argmin(1)
        tri[s:s + step] = k
        bary[s:s + step] = weights[np.arange(len(k)), k]
    return tri, bary


class Source:
    """A shader's original prim groups as one vertex set, to carry attributes the glTF doesn't
    have (and blend shapes) over to new vertices: each takes the values at the nearest point of the
    original surface."""

    def __init__(self, groups):
        self.groups = groups
        self.offsets = np.cumsum([0] + [len(g.positions) for g in groups])
        self.pos = np.concatenate([g.positions for g in groups]).astype(np.float64)
        self.tris = np.concatenate([g.indices.reshape(-1, 3).astype(np.int64) + o
                                    for g, o in zip(groups, self.offsets)])
        self._near = None

    def near(self, points):
        if self._near is None:
            p = self.pos[self.tris]
            self._near = closest_on_triangles(points, p[:, 0], p[:, 1], p[:, 2])
        return self._near

    def carry(self, points, values):
        """`values` (one row per original vertex) at the new vertices."""
        if values is None:
            return None
        tri, bary = self.near(points)
        corners = values[self.tris[tri]].astype(np.float64)
        return (corners * bary[..., None]).sum(1)

    def stack(self, get):
        parts = [get(g) for g in self.groups]
        return None if any(p is None for p in parts) else np.concatenate(parts)


def game_influences(grp):
    """(skeleton joints (n, 4), weights (n, 4)) of a skin prim group, strongest first as in the file."""
    slots = np.minimum(grp.matrices[:, MATRIX_ORDER], len(grp.palette) - 1)
    w = np.zeros((len(grp.positions), 4))
    w[:, :3] = grp.weights
    w[:, 3] = 1.0 - w[:, :3].sum(1)
    return grp.palette[slots].astype(np.int64), np.clip(w, 0, 1)


def top_influences(joints, weights, count=4):
    """The `count` strongest (joint, weight) pairs per vertex, equal joints summed, normalized."""
    j = joints.astype(np.int64).copy()
    w = np.where(weights > 1e-6, weights, 0.0).astype(np.float64)
    for a in range(j.shape[1]):
        for b in range(a + 1, j.shape[1]):
            same = j[:, b] == j[:, a]
            w[:, a] += np.where(same, w[:, b], 0)
            w[:, b] = np.where(same, 0, w[:, b])
    order = np.lexsort((j, -w))[:, :count]
    j, w = np.take_along_axis(j, order, 1), np.take_along_axis(w, order, 1)
    if j.shape[1] < count:
        j = np.pad(j, ((0, 0), (0, count - j.shape[1])))
        w = np.pad(w, ((0, 0), (0, count - w.shape[1])))
    empty = w.sum(1) <= 0
    w[empty, 0] = 1.0
    j = np.where(w > 0, j, 0)
    return j, w / w.sum(1, keepdims=True)


def split_triangles(tris, joints, weights):
    """Triangle lists (in order) whose vertices fit one prim group: at most GROUP_MAX_VERTICES
    vertices and, for skins, GROUP_MAX_MATRICES joints."""
    if (len(np.unique(tris)) <= GROUP_MAX_VERTICES
            and (joints is None or len(np.unique(joints[weights > 0])) <= GROUP_MAX_MATRICES)):
        return [tris]
    out, cur, verts, used = [], [], set(), set()
    for t in tris:
        tv = set(int(v) for v in t) - verts
        tj = set() if joints is None else {int(j) for v in tv for j, w in zip(joints[v], weights[v]) if w > 0} - used
        if cur and (len(verts) + len(tv) > GROUP_MAX_VERTICES or len(used) + len(tj) > GROUP_MAX_MATRICES):
            out.append(np.array(cur))
            cur, verts, used = [], set(), set()
            tv = set(int(v) for v in t)
            tj = set() if joints is None else {int(j) for v in tv for j, w in zip(joints[v], weights[v]) if w > 0}
        cur.append(t)
        verts |= tv
        used |= tj
    if cur:
        out.append(np.array(cur))
    return out


def build_group(template, pos, nrm, codes, uvs, col, joints, weights, tris):
    """A copy of the prim group chunk `template` holding the given vertices (game space) and triangles."""
    grp = copy_chunk(template)
    n = len(pos)
    palette = None
    if joints is not None:
        palette = sorted({int(j) for j, w in zip(joints.reshape(-1), weights.reshape(-1)) if w > 0} or {0})
        lut = np.zeros(max(palette) + 1, np.int64)
        lut[palette] = np.arange(len(palette))
        slots = lut[np.clip(joints, 0, len(lut) - 1)] * (weights > 0)
    _shader, p = p3d.pascal(grp.header, 4)
    head = bytearray(grp.header)
    struct.pack_into("<3I", head, p + 8, n, tris.size, len(palette) if palette is not None else
                     struct.unpack_from("<I", head, p + 16)[0])
    grp.header = bytes(head)
    for c in grp.children:
        if c.id in (POSITIONS, NORMALS):
            c.header = struct.pack("<I", n) + np.ascontiguousarray(pos if c.id == POSITIONS else nrm, "<f4").tobytes()
        elif c.id == NORMAL_CODES:
            c.header = struct.pack("<I", n) + codes.astype(np.uint8).tobytes()
        elif c.id == UVS:
            channel = struct.unpack_from("<I", c.header, 4)[0]
            c.header = struct.pack("<2I", n, channel) + np.ascontiguousarray(uvs[channel], "<f4").tobytes()
        elif c.id == COLOURS:
            c.header = struct.pack("<I", n) + col.astype(np.uint8).tobytes()
        elif c.id == INDICES:
            c.header = struct.pack("<I", tris.size) + tris.astype("<u4").tobytes()
        elif c.id == MATRIX_LIST and palette is not None:
            packed = np.zeros((n, 4), np.uint8)
            packed[:, MATRIX_ORDER] = slots
            c.header = struct.pack("<I", n) + packed.tobytes()
        elif c.id == WEIGHTS and palette is not None:
            c.header = struct.pack("<I", n) + np.ascontiguousarray(weights[:, :3], "<f4").tobytes()
        elif c.id == MATRIX_PALETTE and palette is not None:
            c.header = struct.pack("<I", len(palette)) + np.array(palette, "<u4").tobytes()
    set_bounds(grp.children, pos)
    return grp


def replace_geometry(root, target, doc, buffers, gm, report):
    """Rebuilds the prim groups of the mesh/skin chunk `target` from the glTF mesh `gm`, whatever its
    topology. Each primitive goes to the prim groups of its material's shader (material name = shader
    name, as exported; or the n-th group when the counts match), split where a group would get too
    many vertices or joints. Attributes the glTF lacks (normals, UVs, colours, skin weights) and the
    blend shapes (cutscene faces) are carried over from the nearest point of the original surface.
    Shaders without a primitive keep their groups. True when the mesh changed."""
    mesh = Mesh(target)
    name = target.name()
    pg_chunks = [c for c in target.children if c.id == PRIMGROUP]
    by_shader = defaultdict(list)
    for gi, g in enumerate(mesh.groups):
        by_shader[g.shader.lower()].append(gi)
    prims = defaultdict(list)
    for k, prim in enumerate(gm.get("primitives", [])):
        if "POSITION" not in prim["attributes"] or prim.get("mode", 4) != 4:
            report.append(f"{name}: primitiva {k} sem POSITION ou não é lista de triângulos: ignorada")
            continue
        mat = doc["materials"][prim["material"]].get("name", "") if "material" in prim else ""
        shader = re.sub(r"\.\d{3}$", "", mat).lower()
        if shader not in by_shader:
            if len(gm["primitives"]) != len(mesh.groups):
                report.append(f"{name}: material {mat!r} não é um shader da malha ({', '.join(g.shader for g in mesh.groups)}): "
                              "malha não substituída")
                return False
            shader = mesh.groups[k].shader.lower()
        prims[shader].append(prim)
    if not prims:
        return False

    joint_of = None
    if mesh.skin:
        sk = next((Skeleton(c) for c in root.children if c.id == SKELETON and c.name() == mesh.skeleton), None)
        mi = doc["meshes"].index(gm)
        skin = next((doc["skins"][nd["skin"]] for nd in doc.get("nodes", []) if nd.get("mesh") == mi and "skin" in nd), None)
        if sk is not None and skin is not None:
            names = {n.lower(): i for i, (n, _p, _m) in enumerate(sk.joints)}
            joint_of = np.array([names.get(re.sub(r"\.\d{3}$", "", doc["nodes"][j].get("name", "")).lower(), -1)
                                 for j in skin["joints"]], np.int64)

    # Normal codes: the code of the nearest original normal (the game's table isn't stored anywhere).
    code_n = [g.normals for g in mesh.groups if g.normals is not None and any(c.id == NORMAL_CODES for c in g.chunk.children)]
    code_v = [np.frombuffer(c.header, np.uint8, struct.unpack_from("<I", c.header, 0)[0], 4)
              for g in mesh.groups for c in g.chunk.children if c.id == NORMAL_CODES and g.normals is not None]
    code_n = np.concatenate(code_n).astype(np.float64) if code_n else None
    if code_n is not None:
        code_n /= np.maximum(np.linalg.norm(code_n, axis=1, keepdims=True), 1e-9)
        code_v = np.concatenate(code_v)

    def codes_of(nrm):
        out = np.zeros(len(nrm), np.uint8)
        for s in range(0, len(nrm), 2048):
            out[s:s + 2048] = code_v[np.argmax(nrm[s:s + 2048] @ code_n.T, 1)]
        return out

    new_groups = {}  # shader -> [prim group chunks]
    offsets = []     # (blend shape offsets chunk, new header)
    carried = set()
    for shader, plist in prims.items():
        src = Source([mesh.groups[gi] for gi in by_shader[shader]])
        template = mesh.groups[by_shader[shader][0]]
        P, N, UV, C, J, W, T = [], [], [], [], [], [], []
        off = 0
        for prim in plist:
            attrs = prim["attributes"]
            p = read_accessor(doc, buffers, attrs["POSITION"]) * np.array([1, 1, -1])
            idx = (read_accessor(doc, buffers, prim["indices"]).astype(np.int64).reshape(-1)
                   if "indices" in prim else np.arange(len(p)))
            T.append(idx.reshape(-1, 3)[:, [0, 2, 1]] + off)  # glTF counter-clockwise -> clockwise
            P.append(p)
            N.append(read_accessor(doc, buffers, attrs["NORMAL"]) * np.array([1, 1, -1]) if "NORMAL" in attrs else None)
            UV.append(read_accessor(doc, buffers, attrs["TEXCOORD_0"]) * np.array([1, -1]) if "TEXCOORD_0" in attrs else None)
            C.append(read_accessor(doc, buffers, attrs["COLOR_0"]) if "COLOR_0" in attrs else None)
            if joint_of is not None and "JOINTS_0" in attrs and "WEIGHTS_0" in attrs:
                js = [read_accessor(doc, buffers, attrs[f"JOINTS_{k}"]).astype(np.int64)
                      for k in range(4) if f"JOINTS_{k}" in attrs and f"WEIGHTS_{k}" in attrs]
                ws = [read_accessor(doc, buffers, attrs[f"WEIGHTS_{k}"]) for k in range(len(js))]
                j, w = np.concatenate(js, 1), np.concatenate(ws, 1)
                j = joint_of[np.clip(j, 0, len(joint_of) - 1)]
                w = np.where(j >= 0, w, 0)
                J.append(j)
                W.append(w)
            else:
                J.append(None)
                W.append(None)
            off += len(p)
        pos = np.concatenate(P)
        tris = np.concatenate(T)
        if tris.size and (tris.min() < 0 or tris.max() >= len(pos)):
            report.append(f"{name}: índices fora dos vértices: malha não substituída")
            return False

        def gather(parts, label, fallback):
            if all(x is not None for x in parts):
                return np.concatenate(parts)
            carried.add(label)
            whole = fallback()
            if whole is None:
                return None
            got = [x if x is not None else whole[o:o + len(pp)]
                   for x, pp, o in zip(parts, P, np.cumsum([0] + [len(pp) for pp in P]))]
            return np.concatenate(got)

        nrm = None
        if template.normals is not None:
            nrm = gather(N, "normais", lambda: src.carry(pos, src.stack(lambda g: g.normals)))
            nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-9)
        uvs = {}
        for channel in template.uvs:
            if channel == 0:
                uvs[0] = gather(UV, "UVs", lambda: src.carry(pos, src.stack(lambda g: g.uvs.get(0))))
            else:
                carried.add(f"UV {channel}")
                uvs[channel] = src.carry(pos, src.stack(lambda g, ch=channel: g.uvs.get(ch)))
        col = None
        if template.colours is not None:
            rgba = gather([None if c is None else c[:, [2, 1, 0, 3]] * 255 if c.shape[1] == 4 else
                           np.concatenate([c[:, [2, 1, 0]] * 255, np.full((len(c), 1), 255.0)], 1) for c in C],
                          "cores", lambda: src.carry(pos, src.stack(lambda g: g.colours)))
            col = np.clip(np.rint(rgba), 0, 255)
        joints = weights = None
        if template.palette is not None:
            if all(j is not None for j in J):
                joints, weights = top_influences(np.concatenate(J), np.concatenate(W))
            else:
                carried.add("pesos do skin")
                infl = [game_influences(g) for g in src.groups]
                oj, ow = np.concatenate([i[0] for i in infl]), np.concatenate([i[1] for i in infl])
                tri, bary = src.near(pos)
                corners = src.tris[tri]
                joints, weights = top_influences(oj[corners].reshape(len(pos), -1),
                                                 (ow[corners] * bary[..., None]).reshape(len(pos), -1))
        codes = codes_of(nrm) if nrm is not None and code_n is not None else None

        blend = [(s, t) for s in target.children if s.id == BLEND_SHAPES for t in s.children
                 if t.id == BLEND_TARGET and struct.unpack_from("<I", t.header, 8)[0] in by_shader[shader]]
        pieces = split_triangles(tris, joints, weights)
        if blend and (len(pieces) > 1 or len(by_shader[shader]) > 1):
            report.append(f"{name}: o shader {template.shader} tem blend shapes e precisaria de mais de um grupo: "
                          "malha não substituída")
            return False
        groups = []
        for piece in pieces:
            used = np.unique(piece)
            remap = np.full(len(pos), -1, np.int64)
            remap[used] = np.arange(len(used))
            groups.append(build_group(template.chunk, pos[used], None if nrm is None else nrm[used],
                                      None if codes is None else codes[used], {ch: u[used] for ch, u in uvs.items()},
                                      None if col is None else col[used],
                                      None if joints is None else joints[used], None if weights is None else weights[used],
                                      remap[piece].reshape(-1)))
        for _s, t in blend:  # one original group, one new group: carry each target's offsets over
            carried.add("blend shapes")
            for o in t.children:
                if o.id not in BLEND_WIDTH:
                    report.append(f"{name}: blend shape de tipo {o.header[4:8]!r} ({o.id:#x}): malha não substituída")
                    return False
                count = struct.unpack_from("<I", o.header, 8)[0]
                rows = np.frombuffer(o.header, np.dtype([("i", "<u4"), ("d", "<f4", BLEND_WIDTH[o.id])]), count, 12)
                dense = np.zeros((len(src.pos), BLEND_WIDTH[o.id]))
                dense[rows["i"]] = rows["d"]
                moved = src.carry(pos, dense)
                keep = np.nonzero(np.abs(moved).max(1) > 1e-7)[0]
                out = np.zeros(len(keep), rows.dtype)
                out["i"], out["d"] = keep, moved[keep]
                offsets.append((o, o.header[:8] + struct.pack("<I", len(keep)) + out.tobytes()))
        new_groups[shader] = groups

    for o, header in offsets:  # only now: nothing above may leave the mesh half replaced
        o.header = header

    # Mesh children: the new groups where the shader's first group was, the others kept.
    children, new_index = [], {}
    for c in target.children:
        if c.id != PRIMGROUP:
            children.append(c)
            continue
        gi = pg_chunks.index(c)
        shader = mesh.groups[gi].shader.lower()
        if shader not in new_groups:
            new_index[gi] = sum(k.id == PRIMGROUP for k in children)
            children.append(c)
        elif by_shader[shader][0] == gi:
            new_index[gi] = sum(k.id == PRIMGROUP for k in children)
            children += new_groups[shader]
    target.children = children
    for s in target.children:
        if s.id == BLEND_SHAPES:
            for t in s.children:
                if t.id == BLEND_TARGET:
                    gi = struct.unpack_from("<I", t.header, 8)[0]
                    t.header = t.header[:8] + struct.pack("<I", new_index.get(gi, gi)) + t.header[12:]
    count = sum(c.id == PRIMGROUP for c in children)
    head = bytearray(target.header)
    _n, pos_ = p3d.pascal(head, 0)
    if mesh.skin:
        _s, pos_ = p3d.pascal(head, pos_ + 4)
        struct.pack_into("<I", head, pos_, count)
    else:
        struct.pack_into("<I", head, pos_ + 4, count)
    target.header = bytes(head)
    before = (len(mesh.groups), sum(len(g.positions) for g in mesh.groups), sum(len(g.indices) // 3 for g in mesh.groups))
    after = [PrimGroup(c) for c in children if c.id == PRIMGROUP]
    report.append(f"{name}: geometria substituída: {before[0]} grupo(s), {before[1]} vértices, {before[2]} triângulos -> "
                  f"{len(after)} grupo(s), {sum(len(g.positions) for g in after)} vértices, "
                  f"{sum(len(g.indices) // 3 for g in after)} triângulos"
                  + (f" ({', '.join(sorted(carried))} trazidos da malha original)" if carried else ""))
    return True


REPLACED = {}  # (glTF, mesh index, mesh hash, skeleton hash) -> (ok, header, children, report lines)


def apply_gltf(data, gltf_path, only=None):
    """Package bytes with the vertices of the edited glTF put back (only to the meshes named in
    `only`, if given), and a report."""
    root = p3d.parse(data)
    meshes = {c.name().lower(): c for c in root.children if c.id in (MESH, SKIN)
              and (only is None or c.name().lower() in only)}
    doc, buffers = read_gltf(gltf_path)
    report = []
    changed_meshes = set()
    for gm in doc.get("meshes", []):
        name = re.sub(r"\.\d{3}$", "", gm.get("name", "")).lower()
        target = meshes.get(name)
        if target is None:
            # Blender names meshes after objects only sometimes: try the nodes using this mesh.
            mi = doc["meshes"].index(gm)
            for nd in doc.get("nodes", []):
                if nd.get("mesh") == mi:
                    target = meshes.get(re.sub(r"\.\d{3}$", "", nd.get("name", "")).lower())
                    if target:
                        break
        if target is None:
            report.append(f"malha {gm.get('name')!r} não existe no pacote (ignorada)")
            continue
        groups = [c for c in target.children if c.id == PRIMGROUP]
        if not same_topology(doc, buffers, gm, groups):
            # The same mesh (same bytes) in many packages: replaced once, then copied.
            sk = [c for c in root.children if c.id == SKELETON and target.id == SKIN and c.name() == Mesh(target).skeleton]
            key = (str(gltf_path), doc["meshes"].index(gm), mesh_hash(target), mesh_hash(sk[0]) if sk else "")
            if key not in REPLACED:
                lines = []
                ok = replace_geometry(root, target, doc, buffers, gm, lines)
                REPLACED[key] = (ok, target.header, [copy_chunk(c) for c in target.children], lines)
            ok, header, children, lines = REPLACED[key]
            report += lines
            if ok:
                target.header, target.children = header, [copy_chunk(c) for c in children]
                changed_meshes.add(target.name().lower())
            continue
        state = {}
        for prim in gm.get("primitives", []):
            attrs = prim["attributes"]
            if "TEXCOORD_1" not in attrs or "POSITION" not in attrs:
                report.append(f"{target.name()}: primitiva sem TEXCOORD_1 (o 2º mapa UV com os índices): ignorada")
                continue
            ref = np.rint(read_accessor(doc, buffers, attrs["TEXCOORD_1"])).astype(np.int64)
            pos = read_accessor(doc, buffers, attrs["POSITION"]) * np.array([1, 1, -1])
            nrm = read_accessor(doc, buffers, attrs["NORMAL"]) * np.array([1, 1, -1]) if "NORMAL" in attrs else None
            uv = read_accessor(doc, buffers, attrs["TEXCOORD_0"]) * np.array([1, -1]) if "TEXCOORD_0" in attrs else None
            for gi in np.unique(ref[:, 1]):
                if gi < 0 or gi >= len(groups):
                    continue
                grp = PrimGroup(groups[gi])
                st = state.setdefault(int(gi), {"grp": grp, "pos": grp.positions.astype(np.float64).copy(),
                                                 "nrm": None if grp.normals is None else grp.normals.astype(np.float64).copy(),
                                                 "uv": None if 0 not in grp.uvs else grp.uvs[0].astype(np.float64).copy(),
                                                 "seen": np.zeros(len(grp.positions), bool)})
                sel = ref[:, 1] == gi
                vi = ref[sel, 0]
                ok = (vi >= 0) & (vi < len(st["pos"]))
                vi = vi[ok]
                st["pos"][vi] = pos[sel][ok]
                if nrm is not None and st["nrm"] is not None:
                    # Normals were exported normalized: an unedited one keeps its original bytes.
                    orig = grp.normals[vi].astype(np.float64)
                    unit = orig / np.maximum(np.linalg.norm(orig, axis=1, keepdims=True), 1e-9)
                    edited = nrm[sel][ok]
                    keep = np.abs(edited - unit).max(1) < 1e-5
                    st["nrm"][vi] = np.where(keep[:, None], orig, edited)
                if uv is not None and st["uv"] is not None:
                    st["uv"][vi] = uv[sel][ok]
                st["seen"][vi] = True
        for gi, st in state.items():
            grp = st["grp"]
            moved = float(np.abs(st["pos"] - grp.positions).max())
            if moved < 1e-6 and (st["uv"] is None or float(np.abs(st["uv"] - grp.uvs[0]).max()) < 1e-6):
                continue
            for c in grp.chunk.children:
                if c.id == POSITIONS:
                    set_array_chunk(c, st["pos"])
                elif c.id == NORMALS and st["nrm"] is not None:
                    set_array_chunk(c, st["nrm"])
                elif c.id == UVS and struct.unpack_from("<I", c.header, 4)[0] == 0 and st["uv"] is not None:
                    set_array_chunk(c, st["uv"], skip=8)
            set_bounds(grp.chunk.children, st["pos"])
            missing = int((~st["seen"]).sum())
            report.append(f"{target.name()} grupo {gi}: {len(st['pos'])} vértices, deslocamento máx. {moved:.4f}"
                          + (f", {missing} sem correspondente (mantidos)" if missing else ""))
            changed_meshes.add(target.name().lower())
    for name in changed_meshes:
        mesh = meshes[name]
        allpos = np.concatenate([PrimGroup(c).positions for c in mesh.children if c.id == PRIMGROUP])
        set_bounds(mesh.children, allpos)
    return (root.serialize() if changed_meshes else None), report


def chain_base(path, raw):
    """The package already in the mod (textures.py build, say) with its meshes and skins put back to the
    game's, so this build's model edits start from the original geometry and the rest is kept."""
    base = p3d.parse(path.read_bytes())
    game = defaultdict(list)
    for c in p3d.parse(raw).children:
        if c.id in (MESH, SKIN):
            game[(c.id, c.name())].append(c)
    seen = defaultdict(int)
    for i, c in enumerate(base.children):
        if c.id in (MESH, SKIN):
            k = (c.id, c.name())
            if seen[k] < len(game[k]):
                base.children[i] = game[k][seen[k]]
            seen[k] += 1
    return base.serialize()


def cmd_build(args):
    mod = Path(args.mods_dir) / args.mod
    src = mod / "models"
    if not src.is_dir():
        print(f"{src} não existe: coloque os .glb editados lá (mesmo caminho de private/assets/models)")
        return 1
    files = {rel.lower(): (rel, f) for rel, f in load_table()["files"].items()}
    current = {}  # package key -> patched bytes
    edited = 0
    for f in sorted(list(src.rglob("*.glb")) + list(src.rglob("*.gltf"))):
        rel = f.relative_to(src).with_suffix(".glb").as_posix()
        if rel.lower() not in files:
            print(f"AVISO: {rel}: não é um arquivo exportado (veja models.py find)")
            continue
        rel, entry = files[rel.lower()]
        # Every package that has one of this file's meshes (same bytes), and which meshes.
        targets = defaultdict(set)
        for m in entry["meshes"].values():
            for key in m["uses"]:
                targets[key].add(m["name"].lower())
        changed = 0
        for key in sorted(targets, key=lambda k: k != entry["package"]):
            data = current.get(key)
            if data is None:
                archive, name = key.split("/", 1)
                data = assets.raw_path(archive, name).read_bytes()
                existing = mod / "rcf" / archive / Path(*name.split("\\"))
                if getattr(args, "chain", False) and existing.exists():
                    data = chain_base(existing, data)
            root = p3d.parse(data)
            own = {h: m for h, m in entry["meshes"].items()}
            names = {c.name().lower() for c in root.children
                     if c.id in (MESH, SKIN) and c.name().lower() in targets[key] and mesh_hash(c) in own}
            new, report = apply_gltf(data, f, only=names)
            if key == entry["package"]:
                for line in report:
                    print(f"  {line}")
            if new is not None:
                current[key] = new
                changed += 1
        print(f"{rel}: {changed} pacote(s) mudam" if changed else f"{rel}: nada mudou")
        edited += bool(changed)
    for key, data in sorted(current.items()):
        archive, entry = key.split("/", 1)
        target = mod / "rcf" / archive / Path(*entry.split("\\"))
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    print(f"{edited} modelo(s) editado(s) -> {len(current)} pacote(s) em {mod / 'rcf'}")
    return 0


# --- preview (software render) -------------------------------------------------------------------------

def cmd_preview(args):
    doc, buffers = read_gltf(args.glb)
    images = {}
    tris, uvs, cols, texs = [], [], [], []
    for ni, nd in enumerate(doc.get("nodes", [])):
        if "mesh" not in nd:
            continue
        for prim in doc["meshes"][nd["mesh"]]["primitives"]:
            pos = read_accessor(doc, buffers, prim["attributes"]["POSITION"])
            idx = read_accessor(doc, buffers, prim["indices"]).astype(np.int64).reshape(-1, 3)
            uv = read_accessor(doc, buffers, prim["attributes"]["TEXCOORD_0"]) if "TEXCOORD_0" in prim["attributes"] else None
            col = read_accessor(doc, buffers, prim["attributes"]["COLOR_0"]) if "COLOR_0" in prim["attributes"] else None
            tex = None
            mat = doc["materials"][prim["material"]] if "material" in prim else {}
            bct = mat.get("pbrMetallicRoughness", {}).get("baseColorTexture")
            if bct is not None:
                src = doc["textures"][bct["index"]]["source"]
                if src not in images:
                    v = doc["bufferViews"][doc["images"][src]["bufferView"]]
                    b = buffers[v.get("buffer", 0)][v.get("byteOffset", 0):v.get("byteOffset", 0) + v["byteLength"]]
                    images[src] = np.asarray(Image.open(io.BytesIO(b)).convert("RGBA")).astype(np.float32) / 255
                tex = images[src]
            tris.append(pos[idx])
            uvs.append(uv[idx] if uv is not None else None)
            cols.append(col[idx][:, :, :3] if col is not None else None)
            texs.append(tex)
    if not tris:
        print("sem malhas")
        return 1
    allp = np.concatenate([t.reshape(-1, 3) for t in tris])
    centre = (allp.min(0) + allp.max(0)) / 2
    size = float((allp.max(0) - allp.min(0)).max()) or 1.0
    W = H = args.size
    yaw, pitch = math.radians(args.yaw), math.radians(args.pitch)
    rot = np.array([[math.cos(yaw), 0, math.sin(yaw)], [0, 1, 0], [-math.sin(yaw), 0, math.cos(yaw)]])
    rot = np.array([[1, 0, 0], [0, math.cos(pitch), -math.sin(pitch)], [0, math.sin(pitch), math.cos(pitch)]]) @ rot
    img = np.ones((H, W, 3), np.float32) * 0.18
    zbuf = np.full((H, W), np.inf, np.float32)
    light = np.array([0.4, 0.7, 0.6])
    light /= np.linalg.norm(light)
    for t, uv, col, tex in zip(tris, uvs, cols, texs):
        p = (t - centre) @ rot.T
        sx = (p[..., 0] / size * 0.9 + 0.5) * W
        sy = (0.5 - p[..., 1] / size * 0.9) * H
        sz = -p[..., 2]
        fn = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])
        shade = 0.35 + 0.65 * np.abs(fn @ light) / np.maximum(np.linalg.norm(fn, axis=1), 1e-9)
        for k in range(len(t)):
            x0, y0 = int(max(0, math.floor(sx[k].min()))), int(max(0, math.floor(sy[k].min())))
            x1, y1 = int(min(W - 1, math.ceil(sx[k].max()))), int(min(H - 1, math.ceil(sy[k].max())))
            if x1 < x0 or y1 < y0:
                continue
            xs, ys = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            (ax, bx, cx), (ay, by, cy) = sx[k], sy[k]
            den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
            if abs(den) < 1e-12:
                continue
            l0 = ((by - cy) * (xs - cx) + (cx - bx) * (ys - cy)) / den
            l1 = ((cy - ay) * (xs - cx) + (ax - cx) * (ys - cy)) / den
            l2 = 1 - l0 - l1
            inside = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
            if not inside.any():
                continue
            z = l0 * sz[k, 0] + l1 * sz[k, 1] + l2 * sz[k, 2]
            sub = zbuf[y0:y1 + 1, x0:x1 + 1]
            draw = inside & (z < sub)
            if not draw.any():
                continue
            colour = np.ones(draw.shape + (3,), np.float32) * 0.8
            if tex is not None and uv is not None:
                u = l0 * uv[k, 0, 0] + l1 * uv[k, 1, 0] + l2 * uv[k, 2, 0]
                v = l0 * uv[k, 0, 1] + l1 * uv[k, 1, 1] + l2 * uv[k, 2, 1]
                th, tw = tex.shape[:2]
                tx = (np.mod(u, 1.0) * tw).astype(int).clip(0, tw - 1)
                ty = (np.mod(v, 1.0) * th).astype(int).clip(0, th - 1)
                texel = tex[ty, tx]
                draw &= texel[..., 3] > 0.3
                colour = texel[..., :3]
            elif col is not None:
                colour = (l0[..., None] * col[k, 0] + l1[..., None] * col[k, 1] + l2[..., None] * col[k, 2])
            sub[draw] = z[draw]
            img[y0:y1 + 1, x0:x1 + 1][draw] = colour[draw] * shade[k]
    out = Path(args.out or Path(args.glb).with_suffix(".png"))
    Image.fromarray((img.clip(0, 1) * 255).astype(np.uint8)).save(out)
    print(f"{out} ({sum(len(t) for t in tris)} triângulos)")
    return 0


def main(argv):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("export")
    s = sub.add_parser("find")
    s.add_argument("text")
    s = sub.add_parser("preview")
    s.add_argument("glb")
    s.add_argument("--out")
    s.add_argument("--size", type=int, default=768)
    s.add_argument("--yaw", type=float, default=30)
    s.add_argument("--pitch", type=float, default=15)
    s = sub.add_parser("build")
    s.add_argument("--mod", required=True)
    s.add_argument("--mods-dir", default=str(ROOT / "dist" / "mods"))
    s.add_argument("--chain", action="store_true",
                   help="start from the packages already in mods/NAME/rcf (e.g. from textures.py build) instead of the game's")
    args = p.parse_args(argv)
    return {"export": cmd_export, "find": cmd_find, "preview": cmd_preview, "build": cmd_build}[args.cmd](args)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main(sys.argv[1:]))
