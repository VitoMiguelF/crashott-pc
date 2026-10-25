"""Subtitles for the cutscenes (and the in-game dialogue clips): transcription and data files.

The game has no subtitles and no dialogue text: the text bibles hold menus, HUD and hints only.
Each cutscene's dialogue is one 6-channel sound in the voice archive (english.rcf
vo_nis/6_channel/nisNN_chX: 5.1, the voices in the centre channel), and every in-game line is
its own clip (tutorial/hint001_aku ...). So the English text comes from transcribing the
exported audio with a local speech-to-text model, once, on your machine.

    python tools/subtitles.py transcribe [NAME ...]      # default: all cutscenes (vo_nis)
    python tools/subtitles.py transcribe --dialogue      # + the Aku Aku / Uka Uka / villains clips
    python tools/subtitles.py list                       # sounds with subtitles, durations, lines
    python tools/subtitles.py check pt-br                # translation: missing/extra lines, glyphs

Needs `python tools/assets.py all` (or `audio.py export`) first, and faster-whisper in a venv:

    python -m venv private\\stt\\venv
    private\\stt\\venv\\Scripts\\python -m pip install faster-whisper
    (GPU, optional: ... pip install nvidia-cublas-cu12 "nvidia-cudnn-cu12>=9,<10")

The script re-runs itself with private/stt/venv when faster_whisper isn't importable here.

Files (all derived from the game: they stay on your machine, never publish them):
    private/subtitles/transcripts/<name>.json   raw model output (segments + words)
    private/subtitles/english.txt               the subtitle file the runtime reads (edit it)
    languages/<id>/subtitles.txt                a language pack's translation (same keys)

Subtitle file format (UTF-8):
    [nis01]                                       sound key = exported WAV name without
    sound = english sound\\rsd\\english\\c\\c268899e25ed5279.rsd   folder/_chX; the .rsd it is
    seconds = 103.363                             (archive, entry) and its length
    3.20 6.85 AKU AKU: Crash! Wake up!            start end [SPEAKER:] text (\\n = line break)

Colour tags in the text (runtime/subtitles/subtitle_draw.cpp): {cor=amarelo}AKU AKU:{/cor} Crash!
colours the part between them; a name of COLOR_NAMES or {cor=#RRGGBB}, and they nest. `check`
lists unknown colours and unbalanced tags (the game would show them as text).

A translation only needs the [key] lines and the timed lines (copy english.txt and translate
the text after the times); keys or times it leaves out fall back to nothing (no English mixed in).
"""
import argparse
import csv
import json
import os
import re
import subprocess
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AUDIO = ROOT / "private" / "assets" / "audio"
NAMES_CSV = AUDIO / "names.csv"
OUT = ROOT / "private" / "subtitles"
TRANSCRIPTS = OUT / "transcripts"
ENGLISH = OUT / "english.txt"
VENV_PY = ROOT / "private" / "stt" / "venv" / "Scripts" / "python.exe"

VOICE_DIR = "english/sounds/character/english/"
# In-game dialogue (one line per clip). The enemy folders (labrat, monkey, kooala, voodoo, brat
# girl) are thousands of grunts and taunts; they are left out.
DIALOGUE_FOLDERS = ["tutorial", "nina", "ukauka", "cortex", "ngin", "tiny", "crunch", "other", "credits"]

PROMPT = ("Crash Bandicoot, Aku Aku, Uka Uka, Doctor Neo Cortex, Nina Cortex, N. Gin, Tiny Tiger, "
          "Coco, Crunch, Titans, Mojo, Wumpa Island, Evil Twins, Ratcicle, Spike, Yuktopus, Sludge.")


def key_of(wav):
    """Sound key: nis01, tutorial/hint001_aku ..."""
    rel = wav[len(VOICE_DIR):] if wav.startswith(VOICE_DIR) else wav
    rel = rel[:-4] if rel.lower().endswith(".wav") else rel
    if rel.startswith("vo_nis/"):
        rel = rel.split("/")[-1]
        rel = re.sub(r"_chX$", "", rel)
    return rel


def load_sounds():
    """english voice sounds: key -> row of names.csv."""
    if not NAMES_CSV.exists():
        sys.exit(f"{NAMES_CSV} not found: run python tools/assets.py all (or audio.py export) first")
    sounds = {}
    with open(NAMES_CSV, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r["archive"] == "english" and original(r).startswith(VOICE_DIR):
                sounds[key_of(original(r))] = r
    return sounds


def original(row):
    """The sound's name inside its archive (english/sounds/character/english/...); "wav" is the
    exported file, shared by identical sounds (vozes/english/..., vozes/todos_idiomas/...)."""
    return row.get("original") or row["wav"]


def is_cutscene(row):
    return "/vo_nis/" in original(row)


def is_dialogue(row):
    rel = original(row)[len(VOICE_DIR):]
    return rel.split("/")[0] in DIALOGUE_FOLDERS


def voice_samples(path):
    """Mono float samples + rate: the centre channel of 5.1 (the dialogue), else the mix."""
    import numpy as np
    with wave.open(str(path)) as w:
        ch, rate, width = w.getnchannels(), w.getframerate(), w.getsampwidth()
        data = w.readframes(w.getnframes())
    if width != 2:
        raise ValueError(f"{path}: {width * 8}-bit WAV")
    a = np.frombuffer(data, dtype="<i2").reshape(-1, ch).astype(np.float32) / 32768.0
    return (a[:, 2] if ch == 6 else a.mean(axis=1)), rate


def to_16k(x, rate):
    """Resamples to 16 kHz (windowed-sinc low-pass, then linear interpolation)."""
    import numpy as np
    if rate == 16000:
        return x.astype(np.float32)
    cutoff = 7600.0 / rate
    n = np.arange(-64, 65)
    taps = 2 * cutoff * np.sinc(2 * cutoff * n) * np.hamming(len(n))
    y = np.convolve(x, taps / taps.sum(), mode="same")
    t = np.arange(0, len(x) / rate, 1 / 16000.0)
    return np.interp(t * rate, np.arange(len(x)), y).astype(np.float32)


def speech_regions(x, rate=16000, threshold_db=-42.0, gap=0.45, pad=0.2, min_len=0.25):
    """(start, end) seconds of the stretches above `threshold_db` (20 ms windows), joined across
    gaps shorter than `gap` and padded."""
    import numpy as np
    win = rate // 50
    n = len(x) // win
    if n == 0:
        return [(0.0, len(x) / rate)]
    db = 20 * np.log10(np.sqrt(np.mean(x[:n * win].reshape(n, win) ** 2, axis=1)) + 1e-6)
    regions, start, last = [], None, None
    for i, loud in enumerate(db > threshold_db):
        if loud:
            if start is None:
                start = i
            elif (i - last) * 0.02 > gap:
                regions.append((start, last))
                start = i
            last = i
    if start is not None:
        regions.append((start, last))
    total = len(x) / rate
    return [(max(0.0, a * 0.02 - pad), min(total, (b + 1) * 0.02 + pad)) for a, b in regions
            if (b + 1 - a) * 0.02 >= min_len]


def transcribe(args):
    try:
        import faster_whisper  # noqa: F401
    except ImportError:
        if VENV_PY.exists() and Path(sys.executable).resolve() != VENV_PY.resolve():
            sys.exit(subprocess.call([str(VENV_PY), __file__] + sys.argv[1:]))
        sys.exit("faster-whisper not installed (see the header of this script)")
    # CUDA libraries from the pip nvidia-* wheels (Windows doesn't search site-packages).
    nv = Path(sys.prefix) / "Lib" / "site-packages" / "nvidia"
    if nv.exists():
        for b in nv.glob("*/bin"):
            os.add_dll_directory(str(b))
            os.environ["PATH"] = str(b) + os.pathsep + os.environ["PATH"]
    from faster_whisper import WhisperModel
    import ctranslate2

    sounds = load_sounds()
    if args.names:
        keys = [k for k in sounds if any(re.fullmatch(n.replace("*", ".*"), k) for n in args.names)]
    else:
        keys = [k for k, r in sounds.items() if is_cutscene(r) or (args.dialogue and is_dialogue(r))]
    keys.sort()
    TRANSCRIPTS.mkdir(parents=True, exist_ok=True)
    todo = [k for k in keys if args.force or not (TRANSCRIPTS / (k.replace("/", "__") + ".json")).exists()]
    print(f"{len(keys)} sound(s), {len(todo)} to transcribe")
    if not todo:
        return
    device = "cuda" if ctranslate2.get_cuda_device_count() > 0 and not args.cpu else "cpu"
    model = WhisperModel(args.model, device=device, compute_type="float16" if device == "cuda" else "int8",
                         download_root=str(ROOT / "private" / "stt" / "models"))
    print(f"model {args.model} on {device}")
    for i, k in enumerate(todo, 1):
        row = sounds[k]
        audio = to_16k(*voice_samples(AUDIO / row["wav"]))
        segs = []
        # One pass per stretch of speech: a single pass over a long scene skips lines between
        # far-apart words, and per-stretch times are tighter.
        for a, b in speech_regions(audio):
            segments, _ = model.transcribe(audio[int(a * 16000):int(b * 16000)], language="en", beam_size=5,
                                           word_timestamps=True, initial_prompt=PROMPT, vad_filter=False,
                                           condition_on_previous_text=False)
            for s in segments:
                text = s.text.strip()
                if not text or (s.no_speech_prob > 0.6 and s.avg_logprob < -0.7):
                    continue
                segs.append({"start": round(a + s.start, 2), "end": round(a + s.end, 2), "text": text,
                             "logprob": round(s.avg_logprob, 2), "no_speech": round(s.no_speech_prob, 2),
                             "words": [{"start": round(a + w.start, 2), "end": round(a + w.end, 2), "word": w.word}
                                       for w in (s.words or [])]})
        out = {"key": k, "archive": row["archive"], "entry": row["entry"], "wav": row["wav"],
               "seconds": float(row["seconds"]), "model": args.model, "segments": segs}
        (TRANSCRIPTS / (k.replace("/", "__") + ".json")).write_text(json.dumps(out, indent=1, ensure_ascii=False),
                                                                     encoding="utf-8")
        print(f"[{i}/{len(todo)}] {k}: {len(segs)} segment(s)  " + " | ".join(s["text"] for s in segs)[:150])
    if not ENGLISH.exists():
        print(f"next: python tools/subtitles.py draft  (writes {ENGLISH.relative_to(ROOT)} from the transcripts)")


# --- subtitle file ---------------------------------------------------------------------------

def parse(path):
    """{key: {"sound": str, "seconds": float, "lines": [(start, end, text)]}}; errors listed."""
    data, errors, cur = {}, [], None
    for n, raw in enumerate(Path(path).read_text(encoding="utf-8-sig").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = re.fullmatch(r"\[([^\]]+)\]", line)
        if m:
            cur = data.setdefault(m.group(1), {"sound": "", "seconds": 0.0, "lines": []})
            continue
        if cur is None:
            errors.append(f"{path}:{n}: line before any [key]")
            continue
        m = re.fullmatch(r"(sound|seconds)\s*=\s*(.*)", line)
        if m:
            cur[m.group(1)] = float(m.group(2)) if m.group(1) == "seconds" else m.group(2)
            continue
        m = re.fullmatch(r"(\d+(?:\.\d+)?)\s+(\d+(?:\.\d+)?)\s+(.+)", line)
        if not m:
            errors.append(f"{path}:{n}: expected 'start end text': {raw}")
            continue
        cur["lines"].append((float(m.group(1)), float(m.group(2)), m.group(3)))
    return data, errors


def split_line(seg, max_chars):
    """Splits a long segment (text or time) at word boundaries: at sentence ends once a line has
    a few words, at commas past ~60% of max_chars, anywhere past max_chars."""
    words = seg.get("words") or []
    text = seg["text"]
    if (len(text) <= max_chars and seg["end"] - seg["start"] <= 5.0) or len(words) < 2:
        return [(seg["start"], seg["end"], text)]
    out, cur = [], []
    for i, w in enumerate(words):
        cur.append(w)
        t = "".join(x["word"] for x in cur).strip()
        last = i + 1 == len(words)
        sentence = t.endswith((".", "!", "?")) and len(t) >= 14
        if not last and (sentence or (len(t) >= max_chars * 0.6 and t.endswith(",")) or len(t) >= max_chars):
            out.append((cur[0]["start"], cur[-1]["end"], t))
            cur = []
    if cur:
        t = "".join(x["word"] for x in cur).strip()
        if out and len(t) < 12:
            s, _, prev = out.pop()
            out.append((s, cur[-1]["end"], prev + " " + t))
        else:
            out.append((cur[0]["start"], cur[-1]["end"], t))
    return out


# Whisper's usual inventions on music/noise, and sounds that aren't words.
HALLUCINATIONS = re.compile(r"(thank(s| you) (so much )?for watching|please subscribe|subtitles by|transcribed by|"
                            r"see you (in the )?next|bye[- ]bye\.?$|amara\.org)", re.I)
INTERJECTION = re.compile(r"^(m+|h?m+|hmm+|ga+h+|gr+a+h+|ra+h+|hn+g+|u+h+|u+n+h+|a+h+|o+h+|a+w+|ha(ha)*|ah(ah)*|he+h|heh|ugh+|argh+|o+o+h+|"
                          r"whoa+|wa+h+|o+w+|oo+f+|gr+|ye+a+h+|hu+h+|hi+ya+|hy+a+h*|ho+|hu+|ya+h*|wo+|eh+|"
                          r"woo+(hoo+)?|yay+|aa+|oo+)$", re.I)


def is_noise(text):
    words = [w for w in re.split(r"[\s,.!?…\-]+", text) if w]
    return not words or all(INTERJECTION.match(w) for w in words) or bool(HALLUCINATIONS.search(text))


# Names as the game writes them (the model hears "Duminator", "Cabal"...), and shouted lines the
# model writes in capitals.
SPELLING = [(r"\bDuminator\b", "Doominator"), (r"\bstationary\b", "stationery"), (r"\bUka-Uka\b", "Uka Uka"),
            (r"\bAku-Aku\b", "Aku Aku"), (r"\bNgin\b|\bN\.? ?Gin\b", "N. Gin"), (r"\bWompa\b|\bWumpah?\b", "Wumpa")]
PROPER = ["Crash", "Coco", "Crunch", "Aku Aku", "Uka Uka", "Cortex", "Neo", "Nina", "N. Gin", "Tiny", "Wumpa", "Mojo",
          "Doominator", "Bandicoot", "Doctor", "Dr.", "Evolvore", "Uka", "Aku", "Doomhammer", "Ratcicle", "Spike"]


def fix_text(text):
    for pattern, good in SPELLING:
        text = re.sub(pattern, good, text)
    letters = [c for c in text if c.isalpha()]
    if len(letters) >= 6 and all(c.isupper() for c in letters):
        text = re.sub(r"(^|[.!?]\s+)([a-z])", lambda m: m.group(1) + m.group(2).upper(), text.lower())
        text = re.sub(r"\bi\b", "I", text)
        text = re.sub(r"\bi'", "I'", text)
        for name in PROPER:
            text = re.sub(r"\b" + re.escape(name.lower()) + r"\b", name, text)
    return text


def tidy(lines, total, min_seconds=1.2, gap=0.08):
    """Sorted, without overlaps, each shown at least `min_seconds` when there is room."""
    lines = sorted(lines)
    out = []
    for i, (s, e, t) in enumerate(lines):
        nxt = lines[i + 1][0] if i + 1 < len(lines) else (total or e + min_seconds)
        e = max(e, min(s + min_seconds, nxt - gap))
        e = min(e, nxt - gap) if nxt - gap > s else e
        out.append((round(s, 2), round(max(e, s + 0.3), 2), t))
    return out


def draft(args):
    """Writes the subtitle file from the transcripts (keys already in the file are kept)."""
    existing, _ = parse(ENGLISH) if ENGLISH.exists() else ({}, [])
    files = sorted(TRANSCRIPTS.glob("*.json"))
    blocks = []
    added = skipped = 0
    for f in files:
        t = json.loads(f.read_text(encoding="utf-8"))
        k = t["key"]
        if k in existing and not args.force:
            lines = existing[k]["lines"]
        else:
            lines = []
            for seg in t["segments"]:
                if is_noise(seg["text"]):
                    skipped += 1
                    continue
                lines += [(s, e, fix_text(t)) for s, e, t in split_line(seg, args.max_chars)]
            lines = tidy(lines, t["seconds"])
            added += 1
        if lines:
            blocks.append((k, t["archive"], t["entry"], t["seconds"], lines))
    # Cutscenes first, then the clips by folder.
    blocks.sort(key=lambda b: ("/" in b[0], b[0]))
    with open(ENGLISH, "w", encoding="utf-8", newline="\n") as f:
        f.write("# Subtitles (English), transcribed from the game's voices by tools/subtitles.py.\n"
                "# Game data: keep it on your machine. Format in the header of tools/subtitles.py.\n")
        for k, arch, entry, sec, lines in blocks:
            f.write(f"\n[{k}]\nsound = {arch} {entry}\nseconds = {sec:g}\n")
            for s, e, text in lines:
                f.write(f"{s:.2f} {e:.2f} {text}\n")
    print(f"{ENGLISH.relative_to(ROOT)}: {len(blocks)} sound(s) with text, {added} drafted from transcripts, "
          f"{skipped} non-speech segment(s) left out")


def list_cmd(args):
    data, errors = parse(Path(args.file) if args.file else ENGLISH)
    for e in errors:
        print(e)
    for k, v in data.items():
        print(f"{k:40} {v['seconds']:7.2f} s  {len(v['lines']):3} line(s)")


# Colour tags of the subtitle text (runtime/subtitles/subtitle_draw.cpp, NAMED_COLORS).
BASE_COLORS = ["amarelo", "laranja", "vermelho", "rosa", "roxo", "azul", "ciano", "verde", "cinza", "verdeAzulado",
               "cinzaAzulado"]
COLOR_NAMES = {"branco"} | {b + v for b in BASE_COLORS for v in ("", "Escuro", "Claro")}
TAG = re.compile(r"\{([^{}]{0,31})\}")


def tag_problems(text):
    """Colour tags the game would show as text, or leave open."""
    problems, depth = [], 0
    for m in TAG.finditer(text):
        tag = m.group(1)
        if tag == "/cor":
            if depth == 0:
                problems.append("{/cor} without {cor=...}")
            depth = max(0, depth - 1)
        elif tag.startswith("cor="):
            value = tag[4:].lower()
            if value in {n.lower() for n in COLOR_NAMES} or re.fullmatch(r"#?[0-9a-f]{6}", value):
                depth += 1
            else:
                problems.append(f"unknown colour {{{tag}}} (names: branco, {', '.join(BASE_COLORS)}, "
                                "each also ...Escuro / ...Claro, or #RRGGBB)")
    if depth:
        problems.append("{cor=...} without {/cor} (the colour runs to the end of the line)")
    return problems


def check(args):
    eng, errors = parse(ENGLISH)
    path = ROOT / "languages" / args.lang / "subtitles.txt"
    tr, errors2 = parse(path)
    errors += errors2
    for name, data in (("english", eng), (args.lang, tr)):
        for k, v in data.items():
            for start, _, text in v["lines"]:
                errors += [f"{name}: [{k}] {start:.2f}: {p}" for p in tag_problems(text)]
    for k, v in eng.items():
        if k not in tr:
            errors.append(f"{args.lang}: [{k}] missing")
        elif len(tr[k]["lines"]) != len(v["lines"]):
            errors.append(f"{args.lang}: [{k}] {len(tr[k]['lines'])} line(s), English has {len(v['lines'])}")
    for k in tr:
        if k not in eng:
            errors.append(f"{args.lang}: [{k}] not in English")
    for e in errors:
        print(e)
    print(f"{len(tr)} sound(s), {sum(len(v['lines']) for v in tr.values())} line(s), {len(errors)} problem(s)")
    return 1 if errors else 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("transcribe", help="transcribe voices with faster-whisper")
    t.add_argument("names", nargs="*", help="sound keys (wildcards *), default all cutscenes")
    t.add_argument("--dialogue", action="store_true", help="also the in-game dialogue clips")
    t.add_argument("--model", default="large-v3")
    t.add_argument("--cpu", action="store_true")
    t.add_argument("--force", action="store_true")
    d = sub.add_parser("draft", help="write private/subtitles/english.txt from the transcripts")
    d.add_argument("--max-chars", type=int, default=84)
    d.add_argument("--force", action="store_true", help="redraft keys already in the file")
    li = sub.add_parser("list")
    li.add_argument("file", nargs="?")
    c = sub.add_parser("check")
    c.add_argument("lang")
    args = p.parse_args()
    sys.exit({"transcribe": transcribe, "draft": draft, "list": list_cmd, "check": check}[args.cmd](args) or 0)


if __name__ == "__main__":
    main()
