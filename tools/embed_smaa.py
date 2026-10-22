"""Wraps thirdparty/smaa/SMAA.hlsl in C++ raw string literals (thirdparty/smaa/SMAA.hlsl.inc),
so runtime/gpu/smaa.cpp can compile SMAA at run time without shipping the .hlsl next to the exe.

    python tools/embed_smaa.py

Non-ASCII characters (only in comments) become '?'; the literal is split in 12 KB pieces.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "thirdparty" / "smaa" / "SMAA.hlsl"
OUTPUT = ROOT / "thirdparty" / "smaa" / "SMAA.hlsl.inc"
DELIMITER = "smaa_src"


def main():
    text = SOURCE.read_bytes().decode("latin-1")
    text = "".join(c if ord(c) < 128 else "?" for c in text)
    assert f'){DELIMITER}"' not in text
    pieces = [text[i:i + 12000] for i in range(0, len(text), 12000)]
    out = ["// Generated from SMAA.hlsl (same folder, MIT license, see LICENSE.txt) by tools/embed_smaa.py:",
           "// the HLSL source of SMAA as a string, compiled at run time by runtime/gpu/smaa.cpp.",
           "static const char SMAA_HLSL_SOURCE[] ="]
    out += [f'R"{DELIMITER}({piece}){DELIMITER}"' for piece in pieces]
    out.append(";")
    OUTPUT.write_text("\n".join(out) + "\n", encoding="ascii", newline="\n")
    print(f"{OUTPUT}: {len(text)} characters in {len(pieces)} pieces")


if __name__ == "__main__":
    main()
