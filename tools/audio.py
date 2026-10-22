"""Exports the game's sounds (.rsd) to WAV, the Bink videos to MP4, and builds audio mods.

    python tools/audio.py export [--threads N]       # .rsd -> WAV, one file per distinct sound
    python tools/audio.py videos [--kbps N]          # .bik -> MP4 + WAV per track
    python tools/audio.py find TEXT                  # search the index
    python tools/audio.py build --mod NAME           # loose WAV/OGG -> audio mod

Run `python tools/assets.py extract` first (the raw entries are read from private/assets/raw).

Output (game data: private/, never publish it):
    private/assets/audio/vozes/<language>/<character>/<name>.wav   voices and cutscene dialogue
                                       (vo_nis: 6 channels, the voice in the centre), one folder
                                       per language (english, french, ... = english.rcf, ...)
    private/assets/audio/vozes/todos_idiomas/...   the voice sounds that are the same in every
                                       language (screams, grunts): one copy for all of them
    private/assets/audio/sons/<folder>/<name>.wav  effects and music (sound1.rcf + sound2.rcf:
                                       music, enemies, props, ambience, nis, ...)
    private/assets/audio/nao_usados/...  crash_old_sounds, crash_temp_sounds, crash_temp_vo:
                                       leftovers whose names say they are old/temporary
    private/assets/audio/indice.csv    file, channels, rate, seconds, the .rcf entries that play it
                                       and their original names
    private/assets/audio/names.csv     one row per .rsd entry: archive, entry, wav (the exported
                                       file), original (the old archive/path name), source path,
                                       channels, rate, samples, seconds
    private/assets/video/<name>.mp4 (+ .trackN.wav)

The .rsd entries are named by hash; their header keeps the path of the original .wav, which is
what the exported files are named after (without the redundant "sounds/character/<language>"
part). Entries with the same audio data (byte for byte) are exported once: the file is named
after the first of them and indice.csv lists all the entries and names that use it.

.rsd format ("RSD6XMA "): 0x800-byte header (u32 LE channels, bits, sample rate at 0x08; the
original path at 0x18), then at 0x800 u32 BE XMA2 format size, seek table size, data size, the
XMA2 format (version 3, stream count, play begin/length, rate, block size 0x10000, samples
encoded, samples in source, block count, per stream channel count), the seek table and the XMA2
packets. Decoding: tools/assettool (C++, FFmpeg's WMA Pro decoder, as runtime/audio/xma.cpp).

Putting sounds back (`build`): there is no open XMA encoder, so the runtime plays the loose file
itself. Put the edited files in mods/NAME/audio/ with the exported paths:
    mods/NAME/audio/vozes/english/crunch/crunchmiss_crunch_12.wav   (or .ogg)
and run `build --mod NAME`. It writes, for every .rcf entry that uses that sound, a stand-in .rsd
(same header, XMA2 packets whose frames are markers: magic, sound id, stream, frame number) to
mods/NAME/rcf/<archive>/<entry>, served by the per-entry .rcf overlay, and
mods/NAME/audio/overrides.txt (id -> file). When the emulated XMA decoder meets a marker frame it
outputs the next 512 samples of the loose file instead (runtime/audio/audio_override.cpp),
converted to the channel count and rate the game asked for. WAV (PCM 8/16/24/32-bit or float) and
Ogg Vorbis are accepted.
"""
import argparse
import csv
import hashlib
import re
import struct
import subprocess
import sys
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import assets  # noqa: E402

ROOT = assets.ROOT
AUDIO = assets.ASSETS / "audio"
VIDEO = assets.ASSETS / "video"
NAMES_CSV = AUDIO / "names.csv"
SOUND_ARCHIVES = assets.LANGUAGES + ["sound1", "sound2"]


def friendly_name(source, entry):
    """Relative path of the exported WAV from the original path stored in the .rsd."""
    s = source.replace("/", "\\")
    low = s.lower()
    for marker in ("\\in_game_art\\sound\\", "\\sound\\"):
        i = low.find(marker)
        if i >= 0:
            s = s[i + len(marker):]
            break
    else:
        s = s.rsplit("\\", 1)[-1] or Path(entry).stem
    s = s.replace("*", "X")
    s = re.sub(r'[<>:"|?]', "_", s)
    if not s.lower().endswith(".wav"):
        s += ".wav"
    return s


INDEX_CSV = AUDIO / "indice.csv"
NAME_FIELDS = ["archive", "entry", "wav", "original", "source", "channels", "rate", "samples", "seconds"]
UNUSED_PREFIXES = ("crash_old_sounds", "crash_temp")
KIND_ORDER = {"sons": 0, "vozes": 1, "outros": 2, "nao_usados": 3}


def load_names():
    rows = {}
    if NAMES_CSV.exists():
        with open(NAMES_CSV, newline="", encoding="utf-8") as f:
            for r in csv.DictReader(f):
                rows[(r["archive"], r["entry"])] = r
    return rows


def save_names(rows):
    AUDIO.mkdir(parents=True, exist_ok=True)
    with open(NAMES_CSV, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, NAME_FIELDS, extrasaction="ignore")
        w.writeheader()
        for key in sorted(rows):
            w.writerow(rows[key])


def rsd_head(path):
    """(original path stored in the .rsd, sha1 of its audio data: everything after the header)."""
    data = path.read_bytes()
    return data[0x18:0x800].split(b"\0")[0].decode("latin-1"), hashlib.sha1(data[0x800:]).hexdigest()


def export_place(archive, friendly):
    """(kind, language, path inside it) of a sound in the export, from its friendly name."""
    parts = friendly.replace("\\", "/").split("/")
    low = [p.lower() for p in parts]
    if low[0] == "sounds" and len(parts) > 3 and low[1] == "character":
        return "vozes", archive if archive in assets.LANGUAGES else low[2], "/".join(parts[3:])
    if low[0].startswith(UNUSED_PREFIXES):
        return "nao_usados", None, "/".join(parts)
    if low[0] == "sounds" and len(parts) > 1:
        return "sons", None, "/".join(parts[1:])
    return "outros", None, "/".join(parts)


def plan_export(index):
    """[(exported path, [uses])]: one item per distinct audio data. A use is a dict with
    archive, entry, friendly (the old archive-relative name), source and raw path."""
    sounds = {}
    for archive in SOUND_ARCHIVES:
        entries = [e for e in index["entries"].values() if e["archive"] == archive and e["type"] == "audio"]
        for e in sorted(entries, key=lambda e: e["entry"]):
            raw = assets.raw_path(archive, e["entry"])
            if not raw.exists():
                continue
            source, h = rsd_head(raw)
            sounds.setdefault(h, []).append({"archive": archive, "entry": e["entry"], "source": source, "raw": raw,
                                             "friendly": friendly_name(source, e["entry"]).replace("\\", "/")})
    plan, taken = [], set()
    for _h, uses in sorted(sounds.items(), key=lambda kv: (kv[1][0]["archive"], kv[1][0]["friendly"].lower())):
        places = [(export_place(u["archive"], u["friendly"]), u) for u in uses]
        best = min(KIND_ORDER[k] for (k, _l, _r), _u in places)
        places = [p for p in places if KIND_ORDER[p[0][0]] == best]
        kind = places[0][0][0]
        if kind == "vozes":
            langs = {lang for (_k, lang, _r), _u in places}
            if langs >= set(assets.LANGUAGES):
                folder = "vozes/todos_idiomas"
                rest = min(r for (_k, _l, r), _u in places)
            else:
                lang = min(langs, key=lambda x: assets.LANGUAGES.index(x) if x in assets.LANGUAGES else 99)
                folder = f"vozes/{lang}"
                rest = min(r for (_k, ln, r), _u in places if ln == lang)
        else:
            folder, rest = kind, min(r for (_k, _l, r), _u in places)
        path = f"{folder}/{rest}"
        if path.lower() in taken:  # a different sound made from a .wav of the same name
            path = f"{path[:-4]} ({Path(uses[0]['entry']).stem}).wav"
        taken.add(path.lower())
        plan.append((path, uses))
    return plan


def write_index_csv(plan, names):
    with open(INDEX_CSV, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["arquivo", "canais", "taxa", "segundos", "usos", "entradas", "nomes originais"])
        for path, uses in sorted(plan, key=lambda p: p[0].lower()):
            r = names.get((uses[0]["archive"], uses[0]["entry"]), {})
            w.writerow([path, r.get("channels", ""), r.get("rate", ""), r.get("seconds", ""), len(uses),
                        ";".join(f"{u['archive']}:{u['entry']}" for u in uses),
                        ";".join(sorted({f"{u['archive']}/{u['friendly']}" for u in uses}, key=str.lower))])


def cmd_export(args):
    exe = assets.build_assettool()
    index = assets.load_index()
    old = {r["wav"]: r for r in load_names().values()}
    plan = plan_export(index)
    names, jobs = {}, []
    for path, uses in plan:
        out = AUDIO / path
        prev = old.get(path)
        if out.exists() and prev and not args.force:
            for u in uses:
                names[(u["archive"], u["entry"])] = dict(prev, archive=u["archive"], entry=u["entry"], wav=path,
                                                         original=f"{u['archive']}/{u['friendly']}", source=u["source"])
            continue
        jobs.append((path, uses, out))
    print(f"{sum(len(u) for _p, u in plan)} sons ({len(plan)} diferentes); {len(jobs)} para converter")
    if jobs:
        listing = AUDIO / "_batch.txt"
        AUDIO.mkdir(parents=True, exist_ok=True)
        listing.write_text("".join(f"{uses[0]['raw']}\t{out}\n" for _p, uses, out in jobs), encoding="utf-8")
        cmd = [str(exe), "rsd2wav", "--batch", str(listing)]
        if args.threads:
            cmd += ["--threads", str(args.threads)]
        result = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
        by_raw = {str(uses[0]["raw"]): (path, uses) for path, uses, _out in jobs}
        ok = failed = 0
        suspicious = []
        for line in result.stdout.splitlines():
            parts = line.split("\t")
            if parts[0] == "ok" and parts[1] in by_raw:
                path, uses = by_raw[parts[1]]
                fields = dict(p.split("=", 1) for p in parts[2:])
                rate, samples = int(fields["rate"]), int(fields["samples"])
                for u in uses:
                    names[(u["archive"], u["entry"])] = {
                        "archive": u["archive"], "entry": u["entry"], "wav": path,
                        "original": f"{u['archive']}/{u['friendly']}", "source": u["source"],
                        "channels": fields["channels"], "rate": rate, "samples": samples,
                        "seconds": f"{samples / rate:.3f}"}
                ok += 1
                if int(fields.get("empty_frames", 0)):
                    suspicious.append(f"{path}: {fields['empty_frames']} quadro(s) XMA sem saída")
            elif parts[0] == "error":
                failed += 1
                print("ERRO:", line)
        listing.unlink()
        for line in suspicious[:20]:
            print("AVISO:", line)
        print(f"{ok} convertido(s), {failed} erro(s), {len(suspicious)} com quadros que não decodificaram")
        if failed or result.returncode not in (0, 1):
            save_names(names)
            return 1
    for path, uses in plan:
        for u in uses:
            assets.record_converted(index, u["archive"], u["entry"], [f"audio/{path}"], replace_prefix="audio/")
    save_names(names)
    write_index_csv(plan, names)
    assets.save_index(index)
    kinds = {}
    for path, _uses in plan:
        top = "/".join(path.split("/")[:2]) if path.startswith("vozes/") else path.split("/")[0]
        kinds[top] = kinds.get(top, 0) + 1
    print("arquivos: " + ", ".join(f"{k} {n}" for k, n in sorted(kinds.items())))
    print(f"índice: {INDEX_CSV.relative_to(ROOT)}; por entrada: {NAMES_CSV.relative_to(ROOT)}")
    return 0


def cmd_videos(args):
    exe = assets.build_assettool()
    index = assets.load_index()
    VIDEO.mkdir(parents=True, exist_ok=True)
    status = 0
    for e in sorted(index["entries"].values(), key=lambda e: e["entry"]):
        if e["type"] != "video":
            continue
        raw = assets.raw_path(e["archive"], e["entry"])
        out = VIDEO / (Path(e["entry"]).stem + ".mp4")
        if out.exists() and out.stat().st_mtime >= raw.stat().st_mtime and not args.force:
            print(f"{out.name}: já existe")
            continue
        print(f"{e['entry']} -> {out.relative_to(ROOT)}")
        result = subprocess.run([str(exe), "bik2mp4", str(raw), str(out), "--kbps", str(args.kbps)])
        if result.returncode:
            status = 1
            continue
        outputs = [f"video/{out.name}"] + [f"video/{p.name}" for p in sorted(VIDEO.glob(out.stem + ".track*.wav"))]
        assets.record_converted(index, e["archive"], e["entry"], outputs, replace_prefix="video/")
    assets.save_index(index)
    return status


def cmd_find(args):
    pattern = args.text.lower()
    n = 0
    by_wav = {}
    for r in load_names().values():
        by_wav.setdefault(r["wav"], []).append(r)
    for wav, rows in sorted(by_wav.items()):
        texts = [wav] + [r["entry"] for r in rows] + [r.get("original", "") for r in rows]
        if any(pattern in t.lower() for t in texts):
            r = rows[0]
            print(f"{wav:80} {r['seconds']:>8} s {r['channels']} ch  {len(rows)} entrada(s): "
                  + ", ".join(f"{x['archive']}:{x['entry']}" for x in rows[:3]) + (" ..." if len(rows) > 3 else ""))
            n += 1
    print(f"{n} arquivo(s)")
    return 0


# --- Building audio mods -------------------------------------------------------------------------

PACKET_BYTES = 2048
FRAMES_PER_PACKET = 63          # 6-bit frame count in the packet header
MARKER_FRAME_BITS = 128
MARKER_MAGIC = 0x43525756       # "CRWV"; runtime/audio/audio_override.cpp


def sound_id(archive, entry):
    """FNV-1a of "archive/entry" (lowercase); only has to be unique, the runtime reads it from
    overrides.txt."""
    h = 0x811C9DC5
    for b in f"{archive}/{entry}".lower().encode("latin-1"):
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def audio_length(path):
    """(channels, rate, samples) of a WAV or Ogg Vorbis file."""
    data = path.read_bytes()
    if data[:4] == b"RIFF" and data[8:12] == b"WAVE":
        pos, fmt, size = 12, None, None
        while pos + 8 <= len(data):
            cid, n = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
            if cid == b"fmt ":
                fmt = struct.unpack_from("<HHIIHH", data, pos + 8)
            elif cid == b"data":
                size = min(n, len(data) - pos - 8)
                break
            pos += 8 + n + (n & 1)
        if not fmt or size is None:
            raise ValueError("WAV sem fmt/data")
        _tag, channels, rate, _bps, align, _bits = fmt
        return channels, rate, size // align
    if data[:4] == b"OggS":
        # Identification header in the first page; length = granule position of the last page.
        ident = data.find(b"\x01vorbis")
        if ident < 0:
            raise ValueError("Ogg sem Vorbis")
        channels, rate = data[ident + 11], struct.unpack_from("<I", data, ident + 12)[0]
        last = data.rfind(b"OggS")
        granule = struct.unpack_from("<q", data, last + 6)[0]
        return channels, rate, max(0, granule)
    raise ValueError("formato não suportado (use WAV ou Ogg Vorbis)")


PACKETS_PER_BLOCK = 0x10000 // PACKET_BYTES


def marker_packets(sound, streams, frames):
    """XMA2 data for `streams` streams of `frames` marker frames each, laid out as the game's own
    files are: 64 KB blocks, stream s starting at packet s of every block, each stream's packets
    chained through the header's skip field. A block holds (32 // streams) packets per stream;
    leftover slots are empty packets outside every chain. Returns (data, blocks)."""
    per_block = PACKETS_PER_BLOCK // streams
    packets_per_stream = -(-frames // FRAMES_PER_PACKET)
    blocks = -(-packets_per_stream // per_block)
    positions = [[] for _ in range(streams)]  # file packet index of each stream packet
    for blk in range(blocks):
        count = min(per_block, packets_per_stream - blk * per_block)
        for k in range(count):
            for st in range(streams):
                positions[st].append(blk * PACKETS_PER_BLOCK + k * streams + st)
    total = max(pos[-1] for pos in positions) + 1
    data = bytearray(b"\xff" * (total * PACKET_BYTES))
    for i in range(total):  # filler packets: no frames
        struct.pack_into(">I", data, i * PACKET_BYTES, 1 << 8)
    for st in range(streams):
        frame_no = 0
        for n, at in enumerate(positions[st]):
            nxt = positions[st][n + 1] if n + 1 < len(positions[st]) else at + 1
            count = min(FRAMES_PER_PACKET, frames - frame_no)
            base = at * PACKET_BYTES
            struct.pack_into(">I", data, base, (count << 26) | (1 << 8) | (nxt - at - 1))
            for f in range(count):
                # 15-bit length, magic, sound id, stream, 24-bit frame number, zero padding
                value = (MARKER_FRAME_BITS << 113) | (MARKER_MAGIC << 81) | (sound << 49) | (st << 41) | \
                        ((frame_no + f) << 17)
                off = base + 4 + f * MARKER_FRAME_BITS // 8
                data[off:off + 16] = value.to_bytes(16, "big")
            frame_no += count
    return bytes(data), blocks


def build_marker_rsd(original, sound, rate_in, samples):
    """Stand-in .rsd: the original header, XMA2 format and seek table updated for the new length,
    marker packets."""
    fmt_size = struct.unpack_from(">I", original, 0x800)[0]
    fmt = bytearray(original[0x80C:0x80C + fmt_size])
    streams = fmt[1]
    rate = struct.unpack_from("<I", original, 0x10)[0]
    # Lengths are counted at the .rsd's rate; the runtime converts the loose file to it.
    length = samples if rate_in == rate else int(round(samples * rate / rate_in))
    frames = max(1, -(-length // 512))
    data, blocks = marker_packets(sound, streams, frames)
    play_begin, play_length = struct.unpack_from(">2I", fmt, 4)
    encoded_orig = struct.unpack_from(">I", fmt, 20)[0]
    encoded = frames * 512
    # Second field (play/loop end): kept at the same distance from the end as in the original.
    struct.pack_into(">I", fmt, 8, max(play_begin, encoded - max(0, encoded_orig - play_length)))
    struct.pack_into(">I", fmt, 20, encoded)
    struct.pack_into(">I", fmt, 24, length)
    struct.pack_into(">I", fmt, 28, blocks)
    frames_per_block = (PACKETS_PER_BLOCK // streams) * FRAMES_PER_PACKET
    seek = b"".join(struct.pack(">I", min(frames, (b + 1) * frames_per_block) * 512) for b in range(blocks))
    out = bytearray(original[:0x800])
    out += struct.pack(">3I", fmt_size, len(seek), len(data))
    out += fmt + seek + data
    return bytes(out)


def cmd_build(args):
    mod = Path(args.mods_dir) / args.mod
    src = mod / "audio"
    if not src.is_dir():
        print(f"{src} não existe: coloque os WAV/OGG editados lá (mesmos nomes de private/assets/audio)")
        return 1
    names = load_names()
    by_wav = {}  # exported file -> every entry that plays it
    for r in names.values():
        by_wav.setdefault(r["wav"].lower(), []).append(r)
    by_entry = {f"{r['archive']}/{r['entry']}".lower().replace("\\", "/"): [r] for r in names.values()}
    manifest = []
    built = 0
    for f in sorted(src.rglob("*")):
        if f.suffix.lower() not in (".wav", ".ogg") or not f.is_file():
            continue
        rel = f.relative_to(src).as_posix()
        stem = rel[: -len(f.suffix)]
        rows = by_wav.get((stem + ".wav").lower()) or by_entry.get((stem + ".rsd").lower())
        if rows is None:
            print(f"AVISO: {rel}: não corresponde a nenhum som exportado (veja audio.py find)")
            continue
        try:
            channels, rate, samples = audio_length(f)
        except ValueError as e:
            print(f"AVISO: {rel}: {e}")
            continue
        note = ""
        orig_seconds = float(rows[0]["seconds"])
        if samples / rate > orig_seconds + 0.05:
            note = f" (mais longo que o original, {orig_seconds:.2f} s: o jogo pode cortar)"
        print(f"{rel}: {channels} ch {rate} Hz {samples / rate:.2f} s -> {len(rows)} entrada(s){note}")
        for row in sorted(rows, key=lambda r: (r["archive"], r["entry"])):
            archive, entry = row["archive"], row["entry"]
            original = assets.raw_path(archive, entry).read_bytes()
            sid = sound_id(archive, entry)
            stand_in = build_marker_rsd(original, sid, rate, samples)
            target = mod / "rcf" / archive / Path(*entry.split("\\"))
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(stand_in)
            rsd_channels, rsd_rate = struct.unpack_from("<I", original, 8)[0], struct.unpack_from("<I", original, 0x10)[0]
            manifest.append(f"{sid:08x}\t{rel}\t{archive}/{entry}\t{rsd_channels}\t{rsd_rate}")
            print(f"    {archive}:{entry} ({row.get('original') or row['wav']})")
        built += 1
    (src / "overrides.txt").write_text(
        "# id\tarquivo (relativo a esta pasta)\tentrada do .rcf\tcanais\ttaxa  -- gerado por tools/audio.py build\n" +
        "".join(m + "\n" for m in manifest), encoding="utf-8")
    print(f"{built} som(ns) no mod {args.mod}: {mod}")
    return 0


def main(argv):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("export")
    s.add_argument("--threads", type=int, default=0)
    s.add_argument("--force", action="store_true")
    s = sub.add_parser("videos")
    s.add_argument("--kbps", type=int, default=8000)
    s.add_argument("--force", action="store_true")
    s = sub.add_parser("find")
    s.add_argument("text")
    s = sub.add_parser("build")
    s.add_argument("--mod", required=True)
    s.add_argument("--mods-dir", default=str(ROOT / "dist" / "mods"))
    args = p.parse_args(argv)
    return {"export": cmd_export, "videos": cmd_videos, "find": cmd_find, "build": cmd_build}[args.cmd](args)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main(sys.argv[1:]))
