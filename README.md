# PCSX ReCompressed Converter

Desktop prototype for creating and verifying `.psxrc` images. The core reads the same virtual disc addresses as the source. The converter uses independent Zstandard hunks for disc data, H.264 for eligible STR movies, and Opus for eligible XA and CDDA.

## Build

Use CMake 3.20 or newer, a C11 compiler, Zstandard and Opus development files, and FFmpeg development files for libavcodec and libavutil. The FFmpeg build needs MDEC and H.264 decoders and the libx264 encoder. Windows testing uses MSYS2 UCRT64. Add the codec DLL directories to `PATH` at runtime.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DFFMPEG_ROOT=/path/to/ffmpeg-sdk \
  -DLIBCHDR_SOURCE=/path/to/libchdr \
  -DOPUS_SOURCE=/path/to/opus
cmake --build build
ctest --test-dir build --output-on-failure
```

`FFMPEG_ROOT` is optional if your toolchain can locate FFmpeg. `LIBCHDR_SOURCE` enables CD CHD import. The PCSX ReCompressed checkout includes a tested libchdr tree at `deps/libchdr`.

Use Opus 1.6.1 at commit `22244de5a79bd1d6d623c32e72bf1954b56235be` with `OPUS_SOURCE` to build the fixed-point version used for cross-platform verification. The core and converter must agree on the reconstructed audio checksums.

## Convert and verify

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
- The audio scanner recognizes CDDA and regular 4-bit XA at 37.8 or 18.9 kHz. It groups XA by file, channel, coding, and sector cadence. Reconstruction preserves the original sample count in each sector.
- `balanced` is the default. `high` and `compact` trade size against quality. Use `--preset lossless`, `--no-video`, `--no-xa`, `--no-cdda`, or `--keep-lba FIRST:COUNT` to preserve selected data losslessly. The converter checks structure, quality, native frame budgets, and net savings before accepting a stream. It verifies the output through the same reader the core uses and publishes it under a new filename.
- `extract-bin` exports a single raw data track for checking in a standard emulator. It exports only the sector data. For an image with subchannels or SBI, play the `.psxrc` directly in the fork.

## Benchmark

Use the same source and movie range to create the baseline CHD and the PSXRC image. The benchmark measures sequential host reads and seeks with fresh reader caches on an OS-warmed image.

```sh
chdman createcd -i game.cue -o game.chd
build/psxrc_bench game.chd smaller.psxrc 12000:4500
```

[Experimental format specification](FORMAT.md)

The converter and shared reader source are licensed under GPL-2.0-or-later. See [COPYING](COPYING).
