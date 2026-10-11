"""Tests of extract_sounds.py on archives and banks made here (no game data):
the EB and SPLC readers, the shaping of loops and one-shots, and a whole run
with a stand-in for vgmstream-cli.

    python -m pytest -q port/skate/converter/test_extract_sounds.py
"""

import math
import os
import stat
import struct
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_sounds as es  # noqa: E402


def eb_archive(entries, shift=4, name_width=0x20):
    """An EB v3 archive of (name, bytes) entries."""
    count = len(entries)
    names_offset = 0x30 + 16 * count
    names = b"".join(b"\0\0" + n.encode().ljust(name_width - 2, b"\0") for n, _ in entries)
    data_start = (names_offset + len(names) + (1 << shift) - 1) >> shift << shift
    table, body, at = b"", b"", data_start
    for _, payload in entries:
        table += struct.pack(">IIII", at >> shift, 0, len(payload), 0)
        chunk = payload + b"\0" * (-len(payload) % (1 << shift))
        body += chunk
        at += len(chunk)
    head = b"EB" + struct.pack(">HIHBB", 3, count, 0x10, shift, 0)
    head += struct.pack(">II", names_offset, len(names)) + bytes([name_width, 2]) + struct.pack(">H", 1)
    head += b"\0" * 4 + struct.pack(">I", at)
    head = head.ljust(0x30, b"\0")
    blob = head + table + names
    return blob + b"\0" * (data_start - len(blob)) + body


def splc_bank(groups, patches, samples):
    """An SPLC v3 bank: groups (lists of patch indices), patches (lists of
    layers, each a list of sample indices) and samples (bytes, each starting
    with an SNR header byte 0x03)."""
    out = bytearray(b"SPLC" + struct.pack(">IIIII", 3, 0, len(patches), len(groups), 0))
    out += struct.pack(">I", len(samples)) + b"bank".ljust(0x24, b"\0")
    assert len(out) == 0x40
    for i, layers in enumerate(patches):
        out += struct.pack(">HH", i, len(layers)) + b"\0" * 0x20
    out = out[:-4]  # the groups start 4 bytes into the last patch
    for slots in groups:
        entry = bytearray(b"\0" * 4 + b"\xff" * 64 + bytes([len(slots), 2, 0, 0]))
        for k, s in enumerate(slots):
            entry[4 + 2 * k:6 + 2 * k] = struct.pack(">H", s)
        out += entry
    out += b"\0" * 4
    for layers in patches:
        for choices in layers:
            out += struct.pack(">HHBBH", 1, 0, len(choices), 1, 0)
            for s in choices:
                out += struct.pack(">H", s) + b"\0" * 0x46
            out += b"\0" * 4
    out = out[:-4]  # the last layer has no trailing 4 bytes
    at = 0
    for s in samples:
        out += struct.pack(">III", at, at + len(s), 0)
        at += len(s)
    for s in samples:
        out += s
    return bytes(out)


def test_eb_archives_are_read(tmp_path):
    path = tmp_path / "a.big"
    path.write_bytes(eb_archive([("one.snr", b"\x03abc"), ("two.abk", b"ABKC" * 9)]))
    archive = es.EbArchive(str(path))
    assert sorted(archive.entries) == ["one.snr", "two.abk"]
    assert archive.read("one.snr") == b"\x03abc"
    assert archive.read("two.abk") == b"ABKC" * 9
    with pytest.raises(es.ConvertError):
        archive.read("three")
    (tmp_path / "b.big").write_bytes(b"BIGF" + b"\0" * 60)
    with pytest.raises(es.ConvertError):
        es.EbArchive(str(tmp_path / "b.big"))


def test_splc_banks_are_taken_apart():
    samples = [b"\x03\x00" + bytes([i]) * 10 for i in range(4)]
    data = splc_bank(groups=[[0, 1], [2]], patches=[[[0], [1]], [[2, 3]], [[3]]], samples=samples)
    bank = es.SplcBank(data)
    assert bank.groups == [[0, 1], [2]]
    assert bank.patch_layers(0) == [[0], [1]]
    assert bank.patch_layers(1) == [[2, 3]]
    assert bank.patch_layers(2) == [[3]]
    assert [bank.sample(i) for i in range(4)] == samples
    with pytest.raises(es.ConvertError):
        es.SplcBank(b"SPLD" + data[4:])


def test_set_names():
    for good in ["roll.wav", "roll_metal.wav", "land-8.wav", "grind_start_wood-3.wav", "STEP_OFF.WAV"]:
        assert es.SET_NAME.match(good), good
    for bad in ["SOURCES.txt", "roll_lava.wav", "music.wav", "roll-100.wav", "roll.ogg"]:
        assert not es.SET_NAME.match(bad), bad
    for row in es.MAPPING:
        assert es.SET_NAME.match(row["name"] + ".wav"), row["name"]
        assert row["confidence"] in es.CONFIDENCES


def test_loops_wrap_and_are_levelled():
    rate = 1000
    tone = [math.sin(2 * math.pi * 7.3 * i / rate) for i in range(3000)]
    loop = es.wrap(tone, 2000, 100)
    assert len(loop) == 2000
    # the end runs into the start: no jump bigger than a step of the tone
    step = max(abs(tone[i + 1] - tone[i]) for i in range(len(tone) - 1))
    assert abs(loop[0] - tone[2000]) < 0.1
    assert abs(loop[-1] - loop[0]) <= 2 * step
    levelled = es.level_loop(loop)
    assert abs(20 * math.log10(es.rms(levelled)) - es.LOOP_RMS_DB) < 0.01
    with pytest.raises(es.ConvertError):
        es.wrap(tone, 2950, 100)
    joined = es.join([[1.0] * 100, [-1.0] * 100], 10)
    assert len(joined) == 190 and joined[0] == 1.0 and joined[-1] == -1.0


def test_one_shots_start_at_the_hit():
    rate = 10000
    hit = [0.0] * 500 + [0.5 * math.exp(-i / 300) * math.sin(i) for i in range(3000)] + [0.0] * 500
    shot = es.one_shot(hit, rate)
    assert len(shot) < len(hit) - 500
    assert abs(20 * math.log10(es.peak(shot)) - es.ONE_SHOT_PEAK_DB) < 0.01
    assert shot[0] == 0.0 and abs(shot[-1]) < 1e-3
    with pytest.raises(es.ConvertError):
        es.one_shot([0.0] * 100, rate)


def test_wav_files_round_trip(tmp_path):
    path = str(tmp_path / "x.wav")
    es.write_wav(path, [0.0, 0.5, -0.5, 1.5], 22050)
    samples, rate = es.read_wav(path)
    assert rate == 22050
    assert [round(s, 3) for s in samples] == [0.0, 0.5, -0.5, 1.0]


FAKE_VGMSTREAM = r'''#!/usr/bin/env python3
# Stands in for vgmstream-cli: writes a second of a tone for any input
import math, struct, sys, wave
args = sys.argv[1:]
if args == ["-V"]:
    print('{"version": "fake"}')
    sys.exit(0)
out = args[args.index("-o") + 1]
sub = int(args[args.index("-s") + 1]) if "-s" in args else 0
freq = 110.0 * (1 + sub % 7)
with wave.open(out, "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(8000)
    w.writeframes(b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * freq * i / 8000)))
                           for i in range(8000 * 8)))
'''


def test_a_whole_run(tmp_path, monkeypatch, capsys):
    fake = tmp_path / "vgmstream-cli"
    fake.write_text(FAKE_VGMSTREAM)
    fake.chmod(fake.stat().st_mode | stat.S_IEXEC)
    audio = tmp_path / "Skate 3" / "data" / "audio"
    audio.mkdir(parents=True)
    grain = struct.pack(">I", 0x10) + b"\0" * 12 + b"\x03\x00\xac\x44" + b"\0" * 32
    (audio / "grains.big").write_bytes(eb_archive([("metal_smooth_hard.grain", grain)]))
    bank = splc_bank(groups=[[0, 1]], patches=[[[0], [1]], [[1]]], samples=[b"\x03\x00" * 8, b"\x03\x01" * 8])
    (audio / "audiofiles.big").write_bytes(eb_archive([("LOOPS.abk", b"ABKC" * 4), ("HITS.bnk", bank)]))
    monkeypatch.setattr(es, "MAPPING", [
        dict(name="roll_metal", confidence="high", source=("grain", "metal_smooth_hard"), note="grain"),
        dict(name="grind", confidence="low", source=("abk_loops", "LOOPS.abk", [1, 2]), note="loops"),
        dict(name="powerslide", confidence="medium", source=("abk_stitch", "LOOPS.abk", [[3, 4]]), note="joined"),
        dict(name="land", confidence="medium", source=("splc", "HITS.bnk", [0]), note="hits"),
    ])
    out = tmp_path / "skate-data" / "sounds"
    out.mkdir(parents=True)
    (out / "pop.wav").write_bytes(b"an old file of the set")
    (out / "notes.txt").write_text("not the set's")

    assert es.main([str(tmp_path / "Skate 3"), str(out), "--vgmstream", str(fake)]) == 0
    written = sorted(os.listdir(out))
    assert written == ["SOURCES.txt", "grind-2.wav", "grind.wav", "land-2.wav", "land.wav",
                       "notes.txt", "powerslide.wav", "roll_metal.wav"]
    sources = (out / "SOURCES.txt").read_text().splitlines()
    assert sources[3].split("\t")[:3] == ["roll_metal.wav", "roll_metal", "high"]
    assert "group 0 patch 1" in (out / "SOURCES.txt").read_text()
    for name in written:
        if name.endswith(".wav"):
            samples, rate = es.read_wav(str(out / name))
            assert rate == 8000 and es.peak(samples) > 0.1, name
    assert "vgmstream-cli fake" in capsys.readouterr().out
    # the same input gives the same files
    first = {n: (out / n).read_bytes() for n in written if n.endswith(".wav")}
    assert es.main([str(tmp_path / "Skate 3"), str(out), "--vgmstream", str(fake), "--min-confidence", "medium"]) == 0
    assert not (out / "grind.wav").exists()
    assert all((out / n).read_bytes() == b for n, b in first.items() if not n.startswith("grind"))

    assert es.main([str(tmp_path), str(out), "--vgmstream", str(fake)]) == 1
    assert es.main(["--list"]) == 0
