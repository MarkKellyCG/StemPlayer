#!/usr/bin/env python3
"""
stemsplit.py - split an audio file into 6 stems (drums, bass, other, vocals,
guitar, piano) using Demucs htdemucs_6s, writing one file per stem.

Used by the StemPlayer VST3 plugin, but works fine standalone:

    python stemsplit.py --input song.flac --output song_stems

Protocol (one line each on stdout, flushed) - the plugin parses these:
    STATUS <text>      human readable status
    PROGRESS <0..1>    overall progress
    HB                 heartbeat (once a second)
    ERROR <text>       fatal error (process exits non-zero)
    DONE <folder>      finished OK

Input can be anything libsndfile reads (WAV, FLAC, AIFF, OGG, and MP3 on
libsndfile >= 1.1).  Mono is duplicated to stereo; >2 channels use the first two.

--model fake is a PIPELINE TEST ONLY (splits the audio into six frequency
bands).  It is NOT source separation, needs no PyTorch, and is only there so
you can verify the plugin/helper round trip before installing the ML stack.
"""
import argparse
import json
import math
import os
import sys
import threading
import time

STEMS = ["drums", "bass", "other", "vocals", "guitar", "piano"]
_t0 = time.time()


def emit(kind, text=""):
    try:
        sys.stdout.write(f"{kind} {text}\n" if text != "" else f"{kind}\n")
        sys.stdout.flush()
    except Exception:
        pass


def _start_heartbeat():
    def beat():
        while True:
            time.sleep(1.0)
            emit("HB")
    threading.Thread(target=beat, daemon=True).start()


# --------------------------------------------------------------------------
# audio I/O helpers
# --------------------------------------------------------------------------
def read_audio(path):
    import numpy as np
    import soundfile as sf
    data, sr = sf.read(path, dtype="float32", always_2d=True)  # (n, ch)
    wav = np.ascontiguousarray(data.T)                          # (ch, n)
    if wav.shape[0] == 1:
        wav = np.repeat(wav, 2, axis=0)
    elif wav.shape[0] > 2:
        wav = np.ascontiguousarray(wav[:2])
    return wav, int(sr)


def resample(wav, sr_in, sr_out):
    """High quality polyphase resample, wav shape (ch, n)."""
    import numpy as np
    from scipy.signal import resample_poly
    if sr_in == sr_out:
        return wav
    g = math.gcd(int(sr_in), int(sr_out))
    out = resample_poly(wav, sr_out // g, sr_in // g, axis=-1)
    return np.ascontiguousarray(out.astype("float32"))


# --------------------------------------------------------------------------
# chunked separation with crossfade (gives real progress + bounded memory)
# --------------------------------------------------------------------------
def plan_chunks(n, chunk, overlap):
    """Return [(start, end), ...] covering [0, n) with consecutive overlaps >= overlap."""
    if n <= chunk:
        return [(0, n)]
    step = chunk - overlap
    spans, s = [], 0
    while True:
        e = min(s + chunk, n)
        spans.append((s, e))
        if e >= n:
            break
        s += step
    if len(spans) > 1 and spans[-1][1] - spans[-1][0] < 2 * overlap:
        spans[-1] = (max(0, n - chunk), n)
    return spans


def separate_chunked(apply_fn, wav, chunk, overlap, on_progress=None):
    """
    apply_fn(np (C,T)) -> np (S,C,T).  Returns np (S,C,n).
    Chunks are linearly cross-faded over their actual overlap, so identical
    content in overlapping chunks reconstructs exactly.
    """
    import numpy as np
    n = wav.shape[1]
    spans = plan_chunks(n, chunk, overlap)
    out = None
    norm = np.zeros(n, dtype="float32")
    for i, (s, e) in enumerate(spans):
        y = apply_fn(np.ascontiguousarray(wav[:, s:e])).astype("float32", copy=False)
        if out is None:
            out = np.zeros((y.shape[0], y.shape[1], n), dtype="float32")
        L = e - s
        w = np.ones(L, dtype="float32")
        if i > 0:                                   # ramp in over overlap with previous
            ov = spans[i - 1][1] - s
            w[:ov] = np.linspace(0.0, 1.0, ov + 2, dtype="float32")[1:-1]
        if i < len(spans) - 1:                      # ramp out over overlap with next
            ov = e - spans[i + 1][0]
            w[L - ov:] = np.linspace(1.0, 0.0, ov + 2, dtype="float32")[1:-1]
        out[:, :, s:e] += y[:, :, :L] * w
        norm[s:e] += w
        if on_progress:
            on_progress((i + 1) / len(spans))
    out /= np.maximum(norm, 1e-6)
    return out


# --------------------------------------------------------------------------
# separation back-ends
# --------------------------------------------------------------------------
def fake_apply_factory(sr):
    """Pipeline-test only: six zero-phase frequency bands. NOT real separation."""
    import numpy as np
    from scipy.signal import butter, sosfiltfilt
    nyq = sr / 2.0
    bands = [(150, 600), (None, 150), (600, 1500), (1500, 4000), (4000, 8000), (8000, None)]
    #        drums        bass        other        vocals        guitar        piano

    def design(lo, hi):
        if lo is None:
            return butter(4, hi / nyq, btype="low", output="sos")
        if hi is None:
            return butter(4, min(lo, nyq * 0.95) / nyq, btype="high", output="sos")
        return butter(4, [lo / nyq, hi / nyq], btype="band", output="sos")

    sos = [design(lo, hi) for (lo, hi) in bands]

    def fn(x):
        return np.stack([sosfiltfilt(s, x, axis=-1) for s in sos]).astype("float32")
    return fn, list(STEMS)


def demucs_apply_factory(model_name, device_pref, shifts, overlap):
    try:
        import torch
    except ImportError:
        raise RuntimeError("PyTorch is not installed. Run the installer (install_helper_windows.ps1) "
                           "or: pip install torch")
    # demucs 4.0.1 pickles a class inside its checkpoints; torch>=2.6 defaults weights_only=True.
    _orig_load = torch.load

    def _load(*a, **k):
        k.setdefault("weights_only", False)
        return _orig_load(*a, **k)
    torch.load = _load

    try:
        from demucs.pretrained import get_model
        from demucs.apply import apply_model
    except ImportError as e:
        raise RuntimeError(f"Demucs is not installed correctly ({e}). Re-run the installer.")

    emit("STATUS", f"Loading model {model_name} (the first run downloads it)")
    model = get_model(model_name)
    model.eval()

    if device_pref == "auto":
        device = "cuda" if torch.cuda.is_available() else "cpu"
    else:
        device = device_pref
    model.to(device)
    emit("STATUS", f"Separating on {device}")

    def fn(x):
        t = torch.from_numpy(x).float()[None]
        with torch.no_grad():
            y = apply_model(model, t, device=device, shifts=shifts, split=True,
                            overlap=overlap, progress=False)[0]
        return y.cpu().numpy()

    return fn, list(model.sources), int(model.samplerate)


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description="Split audio into stems (Demucs htdemucs_6s)")
    ap.add_argument("--input")
    ap.add_argument("--output", help="output folder (created)")
    ap.add_argument("--download-model", action="store_true", help="download and cache the model, then exit")
    ap.add_argument("--model", default="htdemucs_6s", help="htdemucs_6s | fake (pipeline test only)")
    ap.add_argument("--device", default="auto", choices=["auto", "cpu", "cuda", "mps"])
    ap.add_argument("--shifts", type=int, default=1, help="demucs random shifts (more = slower, slightly better)")
    ap.add_argument("--overlap", type=float, default=0.25, help="demucs internal segment overlap")
    ap.add_argument("--chunk", type=float, default=40.0, help="outer chunk length, seconds")
    ap.add_argument("--chunk-overlap", type=float, default=4.0, help="outer chunk crossfade, seconds")
    ap.add_argument("--format", default="wav", choices=["wav", "flac"])
    ap.add_argument("--bit-depth", type=int, default=24, choices=[16, 24, 32])
    ap.add_argument("--output-rate", default="original", choices=["original", "model"])
    ap.add_argument("--progress", action="store_true", help="accepted for compatibility; progress is always printed")
    args = ap.parse_args(argv)
    if not args.download_model and not (args.input and args.output):
        ap.error("--input and --output are required")

    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    _start_heartbeat()
    emit("STATUS", "Starting")

    if args.download_model:
        try:
            demucs_apply_factory(args.model, "cpu", 1, 0.25)
            emit("DONE", "model ready")
            return 0
        except Exception as e:  # noqa
            emit("ERROR", f"{type(e).__name__}: {e}")
            return 1

    try:
        import numpy as np
        import soundfile as sf
    except ImportError as e:
        emit("ERROR", f"Missing Python package: {e}. Run the installer or: pip install numpy scipy soundfile")
        return 2

    try:
        if not os.path.isfile(args.input):
            emit("ERROR", f"Input file not found: {args.input}")
            return 2
        os.makedirs(args.output, exist_ok=True)

        emit("STATUS", "Reading audio")
        wav, sr_in = read_audio(args.input)
        if wav.shape[1] < 1024:
            emit("ERROR", "Input is too short")
            return 2

        if args.model == "fake":
            sr_model = 44100
            fn, names = fake_apply_factory(sr_model)
            emit("STATUS", "PIPELINE TEST MODE: frequency-band split, not real separation")
        else:
            fn, names, sr_model = demucs_apply_factory(args.model, args.device, args.shifts, args.overlap)

        emit("PROGRESS", "0.02")
        x = resample(wav, sr_in, sr_model) if sr_in != sr_model else wav

        ref = x.mean(0)
        mean, std = float(ref.mean()), float(ref.std()) + 1e-8
        xn = (x - mean) / std

        chunk = int(args.chunk * sr_model)
        ovl = int(min(args.chunk_overlap, args.chunk / 4) * sr_model)

        def prog(f):
            emit("PROGRESS", f"{0.03 + 0.87 * f:.4f}")
            emit("STATUS", f"Separating... {int(f * 100)}%")

        y = separate_chunked(fn, xn, chunk, ovl, prog)
        y = y * std + mean                                  # (S, C, n)

        sr_out = sr_in if args.output_rate == "original" else sr_model
        emit("STATUS", "Writing stems")
        stems = []
        for i in range(len(names)):
            s = y[i]
            if sr_out != sr_model:
                s = resample(s, sr_model, sr_out)
            stems.append(s)

        peak = max(float(np.max(np.abs(s))) for s in stems)
        gain = 1.0
        if args.bit_depth != 32 and peak > 0.999:
            gain = 0.999 / peak
            emit("STATUS", f"Peak {peak:.2f} > 1.0: all stems scaled by {gain:.3f} to avoid clipping")

        ext = "flac" if args.format == "flac" else "wav"
        subtype = {16: "PCM_16", 24: "PCM_24", 32: "FLOAT"}[args.bit_depth]
        if args.format == "flac" and subtype == "FLOAT":
            subtype = "PCM_24"
        for i, (name, s) in enumerate(zip(names, stems)):
            sf.write(os.path.join(args.output, f"{name}.{ext}"), (s * gain).T, sr_out,
                     subtype=subtype, format=args.format.upper())
            emit("PROGRESS", f"{0.90 + 0.09 * (i + 1) / len(names):.4f}")

        with open(os.path.join(args.output, "stems.json"), "w", encoding="utf-8") as f:
            json.dump({"source": os.path.basename(args.input), "model": args.model,
                       "sample_rate": sr_out, "format": ext, "bit_depth": args.bit_depth,
                       "gain_applied": gain, "stems": names,
                       "seconds": round(time.time() - _t0, 1)}, f, indent=2)
        emit("PROGRESS", "1.0")
        emit("DONE", args.output)
        return 0
    except Exception as e:  # noqa
        import traceback
        traceback.print_exc(file=sys.stderr)
        emit("ERROR", f"{type(e).__name__}: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
