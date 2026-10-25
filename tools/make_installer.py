"""Builds the single-file installer: build\\CrashSetup.exe with everything the install needs appended
to it, compressed (release\\<tag>\\CrashSetup-<tag>.exe; not in git):

    python tools/make_installer.py v1.05                 # -> release\\v1.05\\CrashSetup-v1.05.exe
    python tools/make_installer.py v1.05 --out build\\x.exe
    python tools/make_installer.py test --worktree       # the working tree instead of HEAD (tests)

What goes in (no game file, no recompiled code: the installer builds the game from the player's dump),
only what building and running the game needs (INSTALL / MODDING below; docs, tests, .github and the
development tools stay in git):
- the source at HEAD (git archive) needed to build: CMakeLists.txt, runtime/, launcher/, thirdparty/,
  languages/, the recompiler's config and patch, setup.py and the tools it runs, the licenses;
- tools\\XenonRecomp\\: its source at setup.py's pinned commit with patches/XenonRecomp.patch
  applied, without .git (+ the .crashrecomp_source stamp setup.py checks);
- toolchain\\: llvm-mingw, CMake, Ninja, the embeddable Python (`python tools/toolchain.py fetch`);
- as the "modding" component, installed only with ASSETS FOR MODDING: the asset export
  (tools/assets.py and the scripts it runs, tools/assettool) and numpy / Pillow;
- the third-party files setup.py would otherwise download, so the install works offline: FFmpeg's
  LGPL DLLs and headers, dxcompiler.dll with its licenses, FidelityFX's amd_fidelityfx_vk.dll and
  headers; and, as the "dlss" component the installer leaves out on PCs without an NVIDIA RTX GPU,
  NVIDIA's nvngx_dlss.dll and our CrashDlss.dll (build\\CrashDlss.dll: our code with the NGX static
  library inside, in object code form; no DLSS SDK header or library is shipped).
The optional data (subtitles, HD AI Remaster) stays in the release assets, downloaded by setup.py.

Format (read by launcher/installer_payload.inc):
    [CrashSetup.exe][chunk 0]...[chunk N-1][index][footer]
- chunks: the files concatenated in index order, cut every CHUNK bytes (and where the component
  changes), each compressed on its own with LZMS (the Compression API of Windows 8+, cabinet.dll),
  so the installer decompresses them on all cores and skips the ones it doesn't need;
- index: UTF-8 text, one record per line ("chunk <offset> <size> <raw offset> <raw size>",
  "file <component> <raw offset> <size> <path>"), offsets relative to the end of the exe;
- footer (32 bytes): b"CRSHPAK1", then little-endian u64 index offset, index size and stub size.
"""
import argparse
import ctypes
import hashlib
import io
import os
import struct
import subprocess
import sys
import tarfile
import time
from concurrent.futures import ThreadPoolExecutor
from ctypes import wintypes
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
MAGIC = b"CRSHPAK1"
CHUNK = 32 << 20
COMPRESS_ALGORITHM_LZMS = 5

# (component, path in the install, source) of the third-party files that setup.py downloads otherwise.
THIRDPARTY = [
    ("base", "thirdparty/ffmpeg/bin", ROOT / "thirdparty" / "ffmpeg" / "bin"),
    ("base", "thirdparty/ffmpeg/include", ROOT / "thirdparty" / "ffmpeg" / "include"),
    ("base", "thirdparty/dxc/bin", ROOT / "thirdparty" / "dxc" / "bin"),
    ("base", "thirdparty/fsr/bin", ROOT / "thirdparty" / "fsr" / "bin"),
    ("base", "thirdparty/fsr/include", ROOT / "thirdparty" / "fsr" / "include"),
    ("dlss", "thirdparty/dlss/bin/nvngx_dlss.dll", ROOT / "thirdparty" / "dlss" / "bin" / "nvngx_dlss.dll"),
    ("dlss", "thirdparty/dlss/bin/CrashDlss.dll", ROOT / "build" / "CrashDlss.dll"),
]
SKIP_PARTS = {"__pycache__", "_unpack"}
# What the install needs of the repository: the build (CMake, the runtime, the launcher, the
# recompiler's config and patch), the play folder (languages, licenses) and setup.py's tools.
INSTALL = ("CMakeLists.txt", "LICENSE", "THIRD_PARTY_NOTICES.md", "config/Crash.toml", "config/Crash_switch_tables.toml",
           "launcher/", "languages/", "patches/", "runtime/", "thirdparty/", "tools/setup.py", "tools/buildenv.py",
           "tools/toolchain.py", "tools/make_dist.py", "tools/requirements.txt")
# ASSETS FOR MODDING only: tools/assets.py all, the scripts it runs and numpy / Pillow.
MODDING = ("tools/assets.py", "tools/rcf.py", "tools/audio.py", "tools/textures.py", "tools/models.py", "tools/p3d.py",
           "tools/assettool/", "toolchain/python/Lib/site-packages/numpy", "toolchain/python/Lib/site-packages/PIL",
           "toolchain/python/Lib/site-packages/pillow")
ORDER = {"base": 0, "modding": 1, "dlss": 2}


def component(path):
    """"base", "modding" or None (not installed) for a path of the repository or the toolchain."""
    if path.startswith(MODDING):
        return "modding"
    if path.startswith(("toolchain/", "tools/XenonRecomp/")):
        return None if "/.github/" in path else "base"  # as they are (XenonRecomp's fmt builds its README.md in)
    if path.endswith(("README.md", "NOTES.md", "GLOSSARIO.md")) and path != "thirdparty/ffmpeg/README.md":
        return None  # notes for developers (FFmpeg's README names the LGPL source: it goes to licencas\)
    return "base" if path.startswith(INSTALL) else None


class Lzms:
    """LZMS through cabinet.dll (one compressor per thread)."""
    def __init__(self):
        cab = ctypes.WinDLL("cabinet.dll")
        self.create = cab.CreateCompressor
        self.create.argtypes = [wintypes.DWORD, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
        self.compress = cab.Compress
        self.compress.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
        self.close = cab.CloseCompressor
        self.close.argtypes = [ctypes.c_void_p]

    def __call__(self, data):
        handle = ctypes.c_void_p()
        if not self.create(COMPRESS_ALGORITHM_LZMS, None, ctypes.byref(handle)):
            raise OSError(ctypes.get_last_error(), "CreateCompressor")
        try:
            out = ctypes.create_string_buffer(len(data) + len(data) // 8 + (1 << 16))
            size = ctypes.c_size_t()
            if not self.compress(handle, data, len(data), out, len(out), ctypes.byref(size)):
                raise OSError(ctypes.get_last_error(), "Compress")
            return out.raw[:size.value]
        finally:
            self.close(handle)


def git_files():
    """{path: bytes} of HEAD (git archive, as the release zip has it)."""
    tar = subprocess.run(["git", "archive", "--format=tar", "HEAD"], cwd=ROOT, capture_output=True, check=True).stdout
    files = {}
    with tarfile.open(fileobj=io.BytesIO(tar)) as t:
        for member in t:
            if member.isfile():
                files[member.name] = t.extractfile(member).read()
    return files


def worktree_files():
    """{path: Path} of the files git tracks or would track (not ignored), as they are on disk."""
    names = subprocess.run(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=ROOT, capture_output=True,
                           text=True, check=True).stdout.split("\0")
    return {n: ROOT / n for n in names if n and (ROOT / n).is_file()}


def collect(worktree=False):
    """[(component, path, bytes or Path)] of everything in the payload."""
    if worktree:
        files = worktree_files()
    else:
        status = subprocess.run(["git", "status", "--porcelain", "--untracked-files=no"], cwd=ROOT, capture_output=True, text=True).stdout
        if status.strip():
            print("aviso: há mudanças não commitadas; o instalador leva o HEAD (--worktree leva a árvore de trabalho)", flush=True)
        files = git_files()
    entries = [(component(name), name, data) for name, data in files.items()]
    import make_release  # the XenonRecomp source, checked against setup.py's pin and the patch
    prefix = "x"
    xenon = make_release.xenon_source(prefix)
    for f, name in xenon:
        entries.append((component(name[len(prefix) + 1:]), name[len(prefix) + 1:], f))
    sys.path.insert(0, str(ROOT / "tools"))
    import setup
    patch = read(next(e for e in entries if e[1] == "patches/XenonRecomp.patch"))
    commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT / "tools" / "XenonRecomp", capture_output=True, text=True).stdout.strip()
    if commit != setup.XENON_COMMIT:
        raise SystemExit(f"tools/XenonRecomp está em {commit[:10]}, não em {setup.XENON_COMMIT[:10]}")
    entries.append(("base", "tools/XenonRecomp/.crashrecomp_source", f"{commit} {hashlib.sha256(patch).hexdigest()}\n".encode("ascii")))
    if toolchain.missing():
        raise SystemExit("toolchain\\ incompleta: python tools\\toolchain.py fetch")
    for f in sorted(toolchain.TOOLCHAIN.rglob("*")):
        rel = f.relative_to(toolchain.TOOLCHAIN)
        if f.is_file() and not SKIP_PARTS.intersection(rel.parts):
            entries.append((component(f"toolchain/{rel.as_posix()}"), f"toolchain/{rel.as_posix()}", f))
    for part, target, source in THIRDPARTY:
        if not source.exists():
            raise SystemExit(f"falta {source}: rode tools\\setup.py (FFmpeg, DXC, FSR, DLSS) e compile o CrashDlss "
                             "(cmake --build build --target CrashDlss)")
        if source.is_file():
            entries.append((part, target, source))
        else:
            for f in sorted(source.rglob("*")):
                if f.is_file():
                    entries.append((part, f"{target}/{f.relative_to(source).as_posix()}", f))
    # Similar files next to each other compress better; each component in chunks of its own.
    entries = [e for e in entries if e[0]]
    entries.sort(key=lambda e: (ORDER[e[0]], Path(e[1]).suffix.lower(), e[1]))
    return entries


def read(entry):
    data = entry[2]
    return data if isinstance(data, bytes) else Path(data).read_bytes()


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("tag")
    p.add_argument("--stub", default=str(ROOT / "build" / "CrashSetup.exe"), help="o CrashSetup.exe compilado (padrão: build\\CrashSetup.exe)")
    p.add_argument("--out", help="o exe de saída (padrão: release\\<tag>\\CrashSetup-<tag>.exe)")
    p.add_argument("--worktree", action="store_true", help="leva a árvore de trabalho (arquivos não ignorados) em vez do HEAD: testes")
    args = p.parse_args()
    stub = Path(args.stub)
    if not stub.is_file():
        raise SystemExit(f"{stub} não existe: cmake --build build --target CrashSetup")
    out = Path(args.out) if args.out else ROOT / "release" / args.tag / f"CrashSetup-{args.tag}.exe"
    out.parent.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    entries = collect(args.worktree)

    # The raw stream, cut into chunks: [(component, [(entry index, offset in file, length)])].
    lines = ["CRASHRECOMP-PAYLOAD 1", f"tag {args.tag}"]
    chunks, raw_offset = [], 0
    current, current_size, current_component = [], 0, None
    file_lines = []
    for i, entry in enumerate(entries):
        component, name = entry[0], entry[1]
        if "\n" in name or "\r" in name:
            raise SystemExit(f"nome de arquivo inválido: {name!r}")
        size = len(entry[2]) if isinstance(entry[2], bytes) else Path(entry[2]).stat().st_size
        file_lines.append(f"file {component} {raw_offset} {size} {name}")
        if current and component != current_component:
            chunks.append(current)
            current, current_size = [], 0
        current_component = component
        done = 0
        while True:
            take = min(size - done, CHUNK - current_size)
            if take > 0 or size == 0:
                current.append((i, done, take))
            done += take
            current_size += take
            raw_offset += take
            if current_size >= CHUNK:
                chunks.append(current)
                current, current_size = [], 0
            if done >= size:
                break
    if current:
        chunks.append(current)

    cache = {}

    def chunk_bytes(pieces):
        parts = []
        for i, offset, length in pieces:
            if i not in cache:
                cache.clear()  # files are read in order: keep only the one being cut
                cache[i] = read(entries[i])
            parts.append(cache[i][offset:offset + length])
        return b"".join(parts)

    lzms = Lzms()
    raw_total = raw_offset
    with open(out, "wb") as f:
        f.write(stub.read_bytes())
        stub_size = f.tell()
        offset, raw = 0, 0
        jobs = min(os.cpu_count() or 4, 8)  # each job holds a chunk or two in memory
        with ThreadPoolExecutor(jobs) as pool:
            pending = []
            for n, pieces in enumerate(chunks):
                data = chunk_bytes(pieces)
                pending.append((len(data), pool.submit(lzms, data)))
                # Write in order, keeping a few chunks in flight.
                while len(pending) > jobs * 2 or (n == len(chunks) - 1 and pending):
                    length, future = pending.pop(0)
                    packed = future.result()
                    f.write(packed)
                    lines.append(f"chunk {offset} {len(packed)} {raw} {length}")
                    offset += len(packed)
                    raw += length
                    print(f"\r{raw / raw_total * 100:5.1f}% ({raw >> 20} / {raw_total >> 20} MB)", end="", flush=True)
        print()
        index = ("\n".join(lines + file_lines) + "\n").encode("utf-8")
        index_offset = offset
        f.write(index)
        f.write(MAGIC + struct.pack("<QQQ", index_offset, len(index), stub_size))
    sizes = {}
    for line in file_lines:
        _, component, _, size, _ = line.split(" ", 4)
        sizes[component] = sizes.get(component, 0) + int(size)
    print(f"{out.name}: {out.stat().st_size / 1048576:.1f} MB ({len(entries)} arquivos, "
          + ", ".join(f"{c} {s / 1048576:.0f} MB" for c, s in sizes.items()) + f" descomprimidos; {len(chunks)} blocos) "
          f"em {time.monotonic() - start:.0f} s", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
