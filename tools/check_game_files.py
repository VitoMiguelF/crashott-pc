#!/usr/bin/env python3
"""Refuses game-derived files in git: the game dump, files extracted or generated from it, builds.

Nothing from Crash of the Titans may be committed (see CONTRIBUTING.md); the .gitignore keeps these
files out, and this check catches what gets past it (git add -f, a new folder, a renamed file).

    python tools/check_game_files.py                 the staged files (the pre-commit hook in .githooks/)
    python tools/check_game_files.py --all           every tracked file (CI)
    python tools/check_game_files.py --range A..B    the files added or changed between two commits

Exit code 0 = clean, 1 = something refused (listed with the reason).
"""

import argparse
import fnmatch
import subprocess
import sys

# Formats of the game itself: executable, archives, videos, sounds, packages, compiled scripts, shaders
GAME_EXTENSIONS = {
    ".xex": "game executable",
    ".rcf": "game archive",
    ".bik": "game video",
    ".rsd": "game sound",
    ".xma": "game sound",
    ".wma": "game sound",
    ".p3d": "game package",
    ".blua": "compiled game script",
    ".bfig": "game fight tree",
    ".updb": "game shader source",
    ".bin": "binary image (image.bin)",
    ".iso": "disc image",
    ".god": "disc image",
    ".dds": "texture (HD packs ship as release assets or mods, not in git)",
}

# Build output
BUILD_EXTENSIONS = {".exe", ".dll", ".lib", ".pdb", ".obj", ".map", ".ilk", ".exp"}

# Folders that only exist on each machine (gitignored): the dump, its derivatives, builds
BLOCKED_PREFIXES = {
    "private/": "game-derived data (xex, image.bin, symbols, assets, subtitles, remaster)",
    "generated/": "C++ recompiled from the game's code",
    "dist/": "the play folder (has the recompiled exe and the user's saves)",
    "build/": "build output",
    "release/": "release assets",
    "tools/XenonRecomp/": "XenonRecomp checkout (only patches/XenonRecomp.patch goes in git)",
    "docs/capturas/": "game captures (only a few chosen ones in docs/screenshots/)",
    "userdata/": "player saves and settings",
}

# Any path segment with one of these names
BLOCKED_SEGMENTS = {
    "game": "a game folder (the dump)",
    "userdata": "player saves and settings",
    "shadercache": "translated game shaders",
    "shaderdump": "translated game shaders",
    "texture_dump": "textures dumped from the game",
    "scripts_dec": "decompiled game scripts",
}

BLOCKED_PATTERNS = {
    "image.bin": "decrypted game image",
    "*/image.bin": "decrypted game image",
    "languages/*/subtitles.txt": "subtitle transcript of the game's voices (release asset)",
}

IMAGE_EXTENSIONS = {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".webp", ".gif"}
IMAGE_FOLDERS = ("docs/screenshots/",)
IMAGE_FILES = ("launcher/crashrecomp.png",)  # our icon (tools/make_icon.py)

# Fixed test saves (tests/userdata_*) and the saves recorded profiles start from (config/recorded/<name>/userdata):
# the only Xbox 360 save containers and userdata folders allowed in git
SAVE_FOLDERS = ("tests/userdata_", "config/recorded/")

MAX_SIZE = 8 * 1024 * 1024


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True, encoding="utf-8").stdout


def extension(path):
    name = path.rsplit("/", 1)[-1]
    return "." + name.rsplit(".", 1)[-1].lower() if "." in name else ""


def reason(path):
    lower = path.lower()
    ext = extension(path)
    if ext in GAME_EXTENSIONS:
        return GAME_EXTENSIONS[ext]
    if ext in BUILD_EXTENSIONS:
        return "build output"
    for prefix, why in BLOCKED_PREFIXES.items():
        if lower.startswith(prefix.lower()):
            return why
    for segment in lower.split("/")[:-1]:
        if segment == "userdata" and lower.startswith(SAVE_FOLDERS):
            continue
        if segment in BLOCKED_SEGMENTS:
            return BLOCKED_SEGMENTS[segment]
    for pattern, why in BLOCKED_PATTERNS.items():
        if fnmatch.fnmatch(lower, pattern):
            return why
    if ext in IMAGE_EXTENSIONS and not lower.startswith(IMAGE_FOLDERS) and lower not in IMAGE_FILES:
        return "image outside docs/screenshots/ (game captures and textures stay out of git)"
    if "/565507e4/" in lower and not lower.startswith(SAVE_FOLDERS):
        return "game save outside the fixed test saves"
    return None


def main():
    parser = argparse.ArgumentParser(description="Refuses game-derived files in git.")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--all", action="store_true", help="check every tracked file")
    group.add_argument("--range", help="check files added/changed in A..B")
    args = parser.parse_args()

    if args.all:
        paths = git("ls-files", "-z").split("\0")
        size_of = lambda p: f":{p}"
    elif args.range:
        paths = git("diff", "--name-only", "-z", "--diff-filter=ACMR", args.range).split("\0")
        head = args.range.split("..")[-1].lstrip(".") or "HEAD"
        size_of = lambda p: f"{head}:{p}"
    else:
        paths = git("diff", "--cached", "--name-only", "-z", "--diff-filter=ACMR").split("\0")
        size_of = lambda p: f":{p}"
    paths = [p for p in paths if p]

    refused = []
    for path in paths:
        why = reason(path)
        if why is None:
            size = int(git("cat-file", "-s", size_of(path)).strip() or 0)
            if size > MAX_SIZE:
                why = f"{size / 1048576:.1f} MB file (over {MAX_SIZE // 1048576} MB: likely game data or a build)"
        if why:
            refused.append((path, why))

    if refused:
        print("check_game_files: these files must not go into git (see CONTRIBUTING.md):", file=sys.stderr)
        for path, why in refused:
            print(f"  {path}: {why}", file=sys.stderr)
        print("Unstage them with: git restore --staged <file>", file=sys.stderr)
        return 1
    print(f"check_game_files: {len(paths)} file(s) ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
