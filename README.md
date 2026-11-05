# StemPlayer

A 64-bit **VST3 instrument** that plays six separated stems in sync with your DAW's transport, plus a
small **Python helper** that splits a WAV/FLAC file into those stems using Demucs (`htdemucs_6s`).

```
song.flac ──► stemsplit.py (Demucs) ──► song_stems/{drums,bass,other,vocals,guitar,piano}.wav
                                              │
              DAW transport ──► StemPlayer.vst3 ──► Mix + 6 separate stereo outputs
```

The split is **offline**: it runs once per song (seconds to minutes), then playback is instant and
sample-accurate. The plugin does not process live audio from a track.

## What you get

| Output | Contents |
|---|---|
| **Mix** (always on) | Sum of all stems after per-stem gain, mute/solo and the master gain |
| **Drums / Bass / Other / Vocals / Guitar / Piano** (optional buses) | Each stem on its own stereo output, with its gain applied. Enable them in your host's routing to send each stem to its own track |

Per stem: gain (-60..+12 dB), mute, solo. Global: master gain (Mix only) and **Stem offset** (+/-120 s) to
line the stems up with your timeline. Mute and Solo affect the **Mix** only; the separate outputs always
carry the stem so your DAW's own mute/solo keep working.

### About "effects"
The 6-stem model outputs drums, bass, other, vocals, guitar and piano. There is **no "effects" stem**:
reverbs, risers, synths and anything else end up in **Other**. Guitar and piano separation is the
weakest part of the model, so expect more bleed there.

## Install (Windows)

1. **Plugin:** build it with GitHub Actions (push this repo, download the `StemPlayer-win64` artefact),
   then copy `StemPlayer.vst3` to `C:\Program Files\Common Files\VST3\` and rescan in your DAW.
2. **Helper:** install Python 3.10-3.12 from python.org, then run
   `powershell -ExecutionPolicy Bypass -File Helper\install_helper_windows.ps1`
   (add `-Cuda` on an NVIDIA GPU). It installs to `%APPDATA%\StemPlayer` and downloads the model.
   StemPlayer finds it there automatically; otherwise click **Helper...** and pick `stemsplit.py`.

## Use

1. Put StemPlayer on an **instrument track** and start the track's audio at the project start.
2. Click **Split audio file...** (or drag a WAV/FLAC onto the window). A progress bar shows the split;
   **Cancel** stops it.
3. Stems are saved next to the source as `<song>_stems/` (or in `%APPDATA%\StemPlayer\stems` if that folder isn't
   writable). Splitting the same file again reuses them if they are newer than the source - delete the
   folder to force a re-split.
4. Press play. The plugin follows the host transport, including loops and locates. Use **Stem offset** if
   your audio doesn't start at 0.
5. Already have stems? **Load stems folder...** accepts any folder containing some or all of
   `drums/bass/other/vocals/guitar/piano` as `.wav` or `.flac`. Mono files are duplicated to stereo, and
   any sample rate is resampled to the host rate (windowed sinc).

The project saves the stems **folder path**, not the audio, so keep the folder alongside the project. If the
host's sample rate changes, the stems are reloaded and resampled automatically.

## Helper on its own

```
python stemsplit.py --input song.flac --output song_stems [--device cuda] [--shifts 1] [--format wav|flac] [--bit-depth 16|24|32]
```
Output is written at the source's sample rate (`--output-rate model` keeps Demucs' 44.1 kHz). With 16/24-bit
output, if the stems' peak would clip, all stems are scaled by one common factor and `stems.json` records it.
`--model fake` is a **pipeline test only** (six frequency bands, not separation) and needs no PyTorch.

Protocol on stdout (what the plugin reads): `STATUS <text>`, `PROGRESS <0..1>`, `HB` (heartbeat), `ERROR <text>`, `DONE <folder>`.
Advanced overrides: `STEMPLAYER_PYTHON` (interpreter used for `.py` helpers) and `STEMPLAYER_HELPER_ARGS` (extra arguments).

## Limits

- **Memory:** stems are held in RAM as 32-bit float stereo - about 0.5 GB for a 4-minute song, growing linearly.
  Very long files are refused rather than risking a crash.
- **Speed:** depends heavily on CPU/GPU. CPU is slow; an NVIDIA GPU is much faster.
- **Quality:** it's a machine-learning model: expect some bleed and artefacts.
- **Hosts:** multiple outputs must be enabled in the host. The plugin always works from the Mix output alone.

## Build and test

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release          # downloads JUCE 8.0.4
cmake --build build
# headless tests (POSIX only; they use shell-script helpers):
STEMPLAYER_TEST_HELPER=$PWD/Helper/stemsplit.py STEMPLAYER_PYTHON=$(which python) \
STEMPLAYER_HELPER_ARGS="--model fake" ./build/StemPlayerTests_artefacts/Release/StemPlayerTests
python Tests/test_helper.py        # helper tests, no PyTorch required
```
Use `-DSTEMPLAYER_JUCE_DIR=/path/to/JUCE` to build against a local checkout.

### What has been verified, and what hasn't
- **Verified (Linux, headless) - 138 checks, repeatable:** the resampler (alignment and level at several rate pairs), the
  loader, transport-synced playback, multi-out routing, Mix == sum of stems, mute/solo/gain/master, offset, seeking,
  state restore, sample-rate change, the editor lifecycle (construct, resize, paint, destroy), and the helper job (live
  status, failure reporting, cancel kills and reaps the child), including a real run of `stemsplit.py` with the fake model.
- **Verified with `pluginval` (strictness 10, GUI tests on, VST3 on Linux):** passes, including Steinberg's VST3 validator,
  editor open/automation/open-while-processing, parameter thread safety, state restoration and parameter fuzzing, with all 7
  stereo output buses. This caught a real editor crash during development (fixed, covered by a regression test).
- **Measured:** loading + resampling six 4-minute stems 44.1 kHz -> 48 kHz took 76 s on a *single* core (it uses up to 8 threads,
  so expect much less on a desktop; not measured there). It only happens when the project rate differs from the stems' rate,
  and it runs in the background with a progress bar.
- **Not verified:** loading the plugin in an actual DAW (the dev sandbox has no audio host); the **Windows** build and the GitHub
  Actions workflow (only the Linux build has been run); `install_helper_windows.ps1`; and the helper with **real Demucs
  weights** (PyTorch could not be installed in the sandbox). The chunking/crossfade around the model is tested with a
  stand-in model, so a problem specific to the real model (memory, speed, a version incompatibility) would only show up on a
  real install.

## Licences
JUCE is AGPLv3 / commercial: distributing a closed-source build needs a JUCE licence. Demucs and its code are MIT; check the
licence of the model weights you download. PyTorch is BSD-style. Don't redistribute other vendors' kit data.
