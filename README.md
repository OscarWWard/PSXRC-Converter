# PCSX ReCompressed Converter

This is the companion tool and shared disc reader for the PCSX ReCompressed libretro core. The converter creates and verifies `.psxrc` PlayStation disc images using lossless Zstandard compression for disc data, H.264 for eligible STR movies, and Opus for eligible XA and CDDA audio. The core reads the same virtual disc addresses as the source. See the [PSXRC format specification](FORMAT.md) for the image layout and reconstruction rules.

## Build

Use CMake 3.20 or newer, a C11 compiler, Zstandard and Opus development files, and FFmpeg development files for libavcodec and libavutil. The FFmpeg build needs MDEC and H.264 decoders and the libx264 encoder. Windows testing uses MSYS2 UCRT64. Build on Windows with the compiler's bin directory on `PATH`. CMake stages native runtime DLLs beside `psxrc.exe` so it can run from a regular Windows prompt. Set `PSXRC_RUNTIME_DLL_DIRS` to a semicolon-separated list of additional search directories if the runtime DLLs live outside the linked SDK and toolchain.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DFFMPEG_ROOT=/path/to/ffmpeg-sdk \
  -DLIBCHDR_SOURCE=/path/to/libchdr \
  -DOPUS_SOURCE=/path/to/opus
cmake --build build
ctest --test-dir build --output-on-failure
```

`FFMPEG_ROOT` is optional if your toolchain can locate FFmpeg. `LIBCHDR_SOURCE` enables CD CHD import. Use `../core/deps/libchdr` when the PCSX ReCompressed core is checked out beside the converter as `core`.

Use Opus 1.6.1 at commit `22244de5a79bd1d6d623c32e72bf1954b56235be` with `OPUS_SOURCE` to build the fixed-point version used for cross-platform verification. On 32-bit targets, the build selects Opus's 64-bit arithmetic in a copy of the pinned source. This keeps XA and CDDA reconstruction checksums compatible with images made by the 64-bit converter.

## Convert and verify

### Windows GUI

Launch `psxrc-gui.exe` from the Windows bundle alongside `psxrc.exe` and its codec DLLs. Choose an ISO, CHD, CUE, or BIN and a new `.psxrc` filename. Keep a CUE's referenced track files with it. The GUI passes its settings to the same converter used by the CLI and shows scanning, encoding, and verification in the log.

From this repository on Windows, build the CLI, runtime DLLs, and GUI into `build/windows-bundle/Release` with:

```powershell
cmake --build build --config Release --target psxrc_windows_bundle
```

Keep the executables and DLLs together when moving the bundle. The GUI uses Windows Forms and the .NET Framework. To build just the GUI, run `.\windows\build-gui.ps1` and pass `-OutputPath` to place it in an existing bundle.

- **Disc** sets the lossless Zstandard level independently of media settings.
- **Movie** supports CRF, constant QP, and average bitrate. It also exposes x264 speed presets, tuning, profile, level, reference frames, VBV limits, and the PlayStation frame reconstruction error limit. Toggle movie compression to preserve all movie sectors losslessly.
- **Audio** sets the Opus target bitrate and quality check. Enable XA and CD audio separately. The mono XA target is half the stereo bitrate.

Advanced x264 options use `key=value:key=value` syntax. The supported controls include `aq-mode`, `aq-strength`, `psy`, `psy-rd`, `deblock`, `no-deblock`, `me`, `merange`, `subme`, `trellis`, `partitions`, `weightp`, `8x8dct`, `deadzone-inter`, `deadzone-intra`, `nr`, `mixed-refs`, `cabac`, `fast-pskip`, `chroma-me`, `chroma-qp-offset`, `qcomp`, `qpmin`, `qpmax`, `qpstep`, `ipratio`, `slices`, and `slice-max-size`. For example, use `aq-mode=2:subme=7:trellis=2`.

PSXRC v1 uses 8-bit 4:2:0 video, closed groups of up to four frames, and immediate packet output. B-frames, open GOP, and lookahead stay disabled. The quality, size, and original frame-budget checks can cause a stream to be kept losslessly. CRF or QP zero with the high444 profile applies to the H.264 step. PlayStation frame reconstruction can still change the video.

### Command line

```sh
build/psxrc convert game.cue original.psxrc --preset lossless
build/psxrc convert game.cue game.psxrc
build/psxrc convert game.cue smaller.psxrc --str 12000:4500 --preset balanced
build/psxrc inspect smaller.psxrc
build/psxrc verify smaller.psxrc
build/psxrc extract-bin smaller.psxrc rebuilt.bin
```

- Inputs include raw single-track BIN or ISO, cooked Mode 1 ISO, binary CUE/BIN with Mode 1 or Mode 2 raw data and CDDA tracks, and supported standalone CD CHD. CUE supports INDEX 00, INDEX 01, and PREGAP. Matching `.sub` and `.sbi` files are imported by basename.
- The scanner recognizes regular Mode 2 STR with BS v2 or v3 in Form 1 and Form 2 sectors. To specify the extent, use `--str FIRST_LBA:SECTOR_COUNT` with a zero-based disc LBA. The extent can include interleaved sectors and must start and end on complete STR frames.
- Use `--mdec-hints` to store a 12-byte hint for each H.264 frame. This flag defaults to off. Hinted images require a reader that supports the new flag. The core can derive equivalent hints during playback of existing images.
- The audio scanner recognizes CDDA and regular 4-bit XA at 37.8 or 18.9 kHz. It groups XA by file, channel, coding, and sector cadence. Reconstruction preserves the original sample count in each sector.
- `balanced` is the default. `high` and `compact` trade size against quality. Use `--preset lossless`, `--no-video`, `--no-xa`, `--no-cdda`, or `--keep-lba FIRST:COUNT` to preserve selected data losslessly. The converter checks structure, quality, native frame budgets, and net savings before accepting a stream. It verifies the output through the same reader the core uses and publishes it under a new filename.
- Use `--zstd-level 1..19` to tune the sector hunk compressor independently of media quality. The preset defaults are 8 for lossless, high, and balanced, and 15 for compact. The option retains the same hunk size and reader format. It can increase conversion time.
- `extract-bin` exports a single raw data track for checking in a standard emulator. It exports only the sector data. For an image with subchannels or SBI, play the `.psxrc` directly in the fork.

## Benchmark

Use the same source and movie range to create the baseline CHD and the PSXRC image. The benchmark measures sequential host reads and seeks with fresh reader caches on an OS-warmed image.

```sh
chdman createcd -i game.cue -o game.chd
build/psxrc_bench game.chd smaller.psxrc 12000:4500
```

Pass `--all-sectors` to include any sector in the selected range in the seek samples.

For lossy comparisons, keep the source, Zstandard level, and LBA range identical. Compare lossless output with different movie and audio settings and record the actual STR and audio replacement counts from `inspect`. Use a media range and a separate game-data range. Measure boot time separately through the title screen or first playable checkpoint.

```sh
build/psxrc convert game.chd lossless.psxrc --preset lossless --zstd-level 8
build/psxrc convert game.chd balanced.psxrc --zstd-level 8 --video-crf 20 --audio-kbps 112
build/psxrc convert game.chd compact.psxrc --preset compact --zstd-level 8
build/psxrc inspect compact.psxrc
build/psxrc_bench game.chd lossless.psxrc 12000:4500
build/psxrc_bench game.chd balanced.psxrc 12000:4500
build/psxrc_bench game.chd compact.psxrc 12000:4500
```

[Experimental format specification](FORMAT.md)

The converter and shared reader source are licensed under GPL-2.0-or-later. See [COPYING](COPYING).
