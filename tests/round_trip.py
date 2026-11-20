"""Exercise the converter and playback reader with original synthetic STR sectors."""

import pathlib
import math
import struct
import subprocess
import sys
import tempfile


SECTOR = 2352
STR = bytes.fromhex("60 01 01 80")


def run(*arguments, expect=0):
    result = subprocess.run(arguments, capture_output=True, text=True)
    if result.returncode != expect:
        raise AssertionError(f"{arguments}: {result.returncode}\n{result.stdout}\n{result.stderr}")
    return result.stdout


def check_video(root, cli, maker, version):
    original = root / f"source-{version}.bin"
    lossless = root / f"lossless-{version}.psxrc"
    video = root / f"video-{version}.psxrc"
    rebuilt = root / f"rebuilt-{version}.bin"
    fallback = root / f"fallback-{version}.psxrc"
    run(maker, original, str(version))
    source = original.read_bytes()
    count = len(source) // SECTOR
    cue = original.with_suffix(".cue")
    cue.write_text(f'FILE "{original.name}" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
    run(cli, "convert", cue, lossless, "--preset", "lossless")
    run(cli, "extract-bin", lossless, rebuilt)
    assert rebuilt.read_bytes() == source
    tuned = root / f"tuned-{version}.psxrc"
    run(cli, "convert", cue, tuned, "--preset", "lossless", "--zstd-level", "15")
    rebuilt.unlink()
    run(cli, "extract-bin", tuned, rebuilt)
    assert rebuilt.read_bytes() == source
    run(cli, "convert", cue, root / "bad-level.psxrc", "--zstd-level", "20", expect=2)
    run(cli, "convert", cue, lossless, expect=1)
    rebuilt.unlink()
    run(cli, "convert", cue, video, "--preset", "balanced")
    report = run(cli, "inspect", video)
    assert "STR replacement sectors: 0" not in report, report
    assert video.stat().st_size < lossless.stat().st_size
    run(cli, "verify", video)
    run(cli, "extract-bin", video, rebuilt)
    output = rebuilt.read_bytes()
    assert len(output) == len(source)
    for i in range(0, len(source), SECTOR):
        a, b = source[i : i + SECTOR], output[i : i + SECTOR]
        if a[24:28] != STR:
            assert a == b, i // SECTOR
            continue
        assert a[:36] == b[:36] and a[40:44] == b[40:44]
        assert int.from_bytes(b[36:40], "little") <= int.from_bytes(a[36:40], "little")
        assert int.from_bytes(b[44:46], "little") <= int.from_bytes(a[44:46], "little")
        assert b[50:52] == a[50:52]
    damaged = bytearray(video.read_bytes())
    damaged[-1] ^= 8
    corrupt = root / "corrupt.psxrc"
    corrupt.write_bytes(damaged)
    run(cli, "verify", corrupt, expect=1)

    # The last missing STR chunk invalidates the entire selected stream.
    last_video = max(i for i in range(count) if source[i * SECTOR + 24 : i * SECTOR + 28] == STR)
    run(cli, "convert", cue, fallback, "--str", f"0:{last_video}")
    assert "STR replacement sectors: 0" in run(cli, "inspect", fallback)

    if str(version) == "2":
        profiles = (
            ("qp", "--video-qp", "20", "--h264-preset", "fast", "--h264-tune", "film",
             "--h264-profile", "main", "--video-refs", "2", "--h264-extra", "aq-mode=2:trellis=1"),
            ("abr", "--video-kbps", "750", "--video-vbv-rate", "1500",
             "--video-vbv-buffer", "1000", "--h264-level", "3.0"),
        )
        for name, *settings in profiles:
            custom = root / f"{name}.psxrc"
            run(cli, "convert", cue, custom, *settings, "--video-mse", "512", "--mdec-hints")
            report = run(cli, "inspect", custom)
            assert "STR replacement sectors: 0" not in report
            assert "MDEC hints: yes" in report
            run(cli, "verify", custom)
        for setting in ("bframes=2", "subme=invalid"):
            run(cli, "convert", cue, root / "invalid-picture.psxrc", "--h264-extra", setting, expect=2)


def check_unicode_paths(root, cli):
    folder = root / "ディスク test"
    folder.mkdir()
    binary = folder / "トラック.bin"
    cue = folder / "入力.cue"
    image = folder / "圧縮.psxrc"
    binary.write_bytes(bytes(SECTOR * 9))
    cue.write_text('FILE "トラック.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n',
                   encoding="utf-8-sig")
    run(cli, "convert", cue, image, "--preset", "lossless")
    run(cli, "verify", image)


def check_mixed_mode(root, cli):
    (root / "data.bin").write_bytes(bytes(SECTOR * 6))
    (root / "audio.bin").write_bytes(b"\x11" * SECTOR * 5)
    cue = root / "mixed.cue"
    cue.write_text(
        'FILE "data.bin" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n'
        'FILE "audio.bin" BINARY\nTRACK 02 AUDIO\nPREGAP 00:00:03\n'
        "INDEX 00 00:00:00\nINDEX 01 00:00:02\n"
    )
    image = root / "mixed.psxrc"
    cue.with_suffix(".sub").write_bytes(bytes(range(96)) * 14)
    cue.with_suffix(".sbi").write_bytes(b"SBI\0" + bytes.fromhex("00 02 04 01") + bytes(10))
    run(cli, "convert", cue, image)
    report = run(cli, "inspect", image)
    assert "Track 2: CDDA, INDEX 00 6, INDEX 01 11, end 14" in report
    assert "SBI: yes" in report and "Subchannel: yes" in report
    run(cli, "verify", image)
    cooked = root / "cooked.iso"
    cooked.write_bytes(bytes(range(256)) * 24)
    image = root / "cooked.psxrc"
    run(cli, "convert", cooked, image)
    assert "MODE1/2048" in run(cli, "inspect", image)
    run(cli, "verify", image)

    cue.with_suffix(".sbi").write_bytes(b"SBI\0" + bytes.fromhex("00 00 00 01") + bytes(10))
    run(cli, "convert", cue, root / "invalid.psxrc", expect=1)


def check_audio(root, cli, xa_maker):
    cdda = root / "tones.bin"
    pcm = bytearray()
    for i in range(588 * 90):
        t = i / 44100
        pcm.extend(struct.pack("<hh", int(12000 * math.sin(1900 * t + 330 * t * t)),
                               int(12000 * math.sin(1000 * t + 240 * t * t))))
    cdda.write_bytes(pcm)
    cue = root / "tones.cue"
    cue.write_text('FILE "tones.bin" BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n')
    baseline, image = root / "tones-lossless.psxrc", root / "tones.psxrc"
    run(cli, "convert", cue, baseline, "--preset", "lossless")
    run(cli, "convert", cue, image)
    assert image.stat().st_size < baseline.stat().st_size
    assert "Audio replacement sectors: 90" in run(cli, "inspect", image)
    run(cli, "verify", image)
    cdda_no = root / "cdda-no.psxrc"
    run(cli, "convert", cue, cdda_no, "--no-cdda")
    assert cdda_no.read_bytes() == baseline.read_bytes()

    xa = root / "xa.bin"
    run(xa_maker, xa)
    xa_image = root / "xa.psxrc"
    run(cli, "convert", xa, xa_image)
    assert "Audio replacement sectors: 12" in run(cli, "inspect", xa_image)
    run(cli, "verify", xa_image)
    rebuilt = root / "xa-rebuilt.bin"
    run(cli, "extract-bin", xa_image, rebuilt)
    source, output = xa.read_bytes(), rebuilt.read_bytes()
    assert len(output) == len(source)
    for i in range(96):
        a, b = source[i * SECTOR:(i + 1) * SECTOR], output[i * SECTOR:(i + 1) * SECTOR]
        if i % 8:
            assert a == b
        else:
            assert a[:24] == b[:24] and a[2328:2348] == b[2328:2348]
    xa_keep = root / "xa-keep.psxrc"
    run(cli, "convert", xa, xa_keep, "--keep-lba", "32:1")
    assert "Audio replacement sectors: 0" in run(cli, "inspect", xa_keep)


with tempfile.TemporaryDirectory(dir=pathlib.Path(sys.argv[1]).resolve().parent) as directory:
    tmp = pathlib.Path(directory)
    for bs_version in (2, 3):
        check_video(tmp, sys.argv[1], sys.argv[2], bs_version)
    check_mixed_mode(tmp, sys.argv[1])
    check_audio(tmp, sys.argv[1], sys.argv[3])
    check_unicode_paths(tmp, sys.argv[1])
