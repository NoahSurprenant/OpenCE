#!/usr/bin/env python3
"""Skate 3's own sounds for Skate 3 mode, from your copy of the game.

Reads the extracted Skate 3 disc (the folder with default.xex and data/),
decodes the sounds named in MAPPING below with vgmstream-cli, and writes them
as a sound set (port/skate/README.md, "Sound"): 16-bit mono WAV files named
<sound>[_<surface>][-<variant>].wav, plus SOURCES.txt, which says where each
file came from and how sure the mapping is.

    python extract_sounds.py <Skate 3 folder> <skate-data>/sounds

Python 3.8 or later and nothing but its standard library; vgmstream-cli
(https://github.com/vgmstream/vgmstream/releases, with its FFmpeg: Skate 3's
sounds are XMA) does the decoding. The same input gives the same files.

What Skate 3 keeps where (the Xbox 360 disc, data/audio):
- *.big are EA "EB" v3 archives (big-endian): a header, a table of 16-byte
  entries (offset in units of 1 << shift, size, name hash) and a table of
  fixed-width names.
- grains.big: one recording per rolling surface (.grain: a header, a grain
  table, then an EA Audio Core SNR header and its data, XMA).
- audiofiles.big: the sound banks. .abk are EA ABK banks of SNR/SNS streams
  (vgmstream reads them; GRINDS.abk's streams are loops); .bnk are EA "SPLC"
  sample banks, which vgmstream does not read, so this script takes them
  apart: a table of patches (each a few layers, each a choice of samples),
  groups of patches (a sound and its variations), and the samples, each an
  SNR header followed by its data, which vgmstream decodes as a .snr file.
None of it is named for what plays it in the game; MAPPING is the result of
going through the banks by their names, structure and sound (spectra,
envelopes, tonality, loop points), with a confidence for each row.

Never commit or publish what this writes: it is derived from the game.
"""

import argparse
import array
import hashlib
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

# --- the mapping -------------------------------------------------------------
#
# Each row writes one sound (and surface) of the sound set, as one or more
# variants. Sources:
#   ("grain", name)                      grains.big/<name>.grain, cut into a loop
#   ("abk_loops", bank, [subsongs])      streams of an ABK bank that are loops
#   ("abk_stitch", bank, [[subsongs]])   short grains of an ABK bank, joined into
#                                        a loop (one loop per list)
#   ("abk_shots", bank, [subsongs])      one-shots from an ABK bank
#   ("splc", bank, [groups])             each patch of each group, its layers mixed
# Subsongs count from 1 (vgmstream's -s); groups from 0.
MAPPING = [
    # grains.big names each recording by its surface; "hard" are the hard
    # wheels (each surface has a "soft" take too), and the recordings get
    # faster as they go, so the loop is cut from past the middle.
    dict(name="roll", confidence="high", source=("grain", "concrete_smooth_hard"),
         note="grains.big: rolling on smooth concrete (also every surface without its own)"),
    dict(name="roll_metal", confidence="high", source=("grain", "metal_smooth_hard"),
         note="grains.big: rolling on smooth metal"),
    dict(name="roll_wood", confidence="high", source=("grain", "wood_ramp_hard"),
         note="grains.big: rolling on a wooden ramp"),
    dict(name="roll_rough", confidence="medium", source=("grain", "asphalt_rough_hard"),
         note="grains.big: rolling on rough asphalt; Skate 3 has no dirt or grass grain, this is its roughest"),
    # WHEEL_SKID_BANK.abk: the wheels skidding (Skate 3 plays it as grains:
    # its project file names wheelskid_smth_shuff, soft_wheelskid_smooth, ...);
    # its third table is the loudest and longest set.
    dict(name="powerslide", confidence="medium",
         source=("abk_stitch", "WHEEL_SKID_BANK.abk", [list(range(41, 69)), list(range(69, 97))]),
         note="WHEEL_SKID_BANK.abk table 3: wheel-skid grains joined into loops"),
    # GRINDS.abk: 123 grind loops, unnamed. With clear tonal lines (a ringing
    # rail) they are taken for metal; broadband and gritty ones for a
    # concrete ledge, the default.
    dict(name="grind_metal", confidence="medium", source=("abk_loops", "GRINDS.abk", [115, 116, 118, 119]),
         note="GRINDS.abk: loops with the ringing lines of a metal rail"),
    dict(name="grind", confidence="low", source=("abk_loops", "GRINDS.abk", [10, 11, 12]),
         note="GRINDS.abk: broadband, gritty loops (a concrete ledge, by their sound)"),
    # Skate_Collisions.bnk: groups 179-190 are the skateboard's own hits, in
    # threes of a short single hit, a layered hit with a 0.3 s tail and a long
    # crash; 181 has the most variations of the bank (11, seven layers).
    dict(name="land", confidence="medium", source=("splc", "Skate_Collisions.bnk", [181, 184, 187]),
         max_variants=8, note="Skate_Collisions.bnk groups 181, 184, 187: the board's layered landing hits"),
    dict(name="pop", confidence="low", source=("splc", "Skate_Collisions.bnk", [183, 186]),
         note="Skate_Collisions.bnk groups 183, 186: the short single hits of the same set (the tail)"),
    dict(name="board_impact", confidence="low", source=("splc", "Skate_Collisions.bnk", [191, 194, 197]),
         note="Skate_Collisions.bnk groups 191, 194, 197: rattling multiple hits (the board bouncing)"),
    dict(name="bail", confidence="low", source=("splc", "Skate_Collisions.bnk", [171, 173, 175]),
         note="Skate_Collisions.bnk groups 171, 173, 175: low, heavy thuds (a body hitting the ground)"),
    # fstep_skateshoe1_sm.abk: the skater's footsteps (21 tables, by surface
    # and gait); table 7 is short, crisp steps.
    dict(name="step_off", confidence="medium", source=("abk_shots", "fstep_skateshoe1_sm.abk", [45, 46, 47, 48]),
         note="fstep_skateshoe1_sm.abk table 7: skate-shoe footsteps"),
]
# Not mapped, so the built-in set (or its fallbacks) plays them: slide (no
# loop could be told from a grind), grind_start and slide_start (fall back on
# land), grind_end and slide_end (silent), step_on (falls back on
# board_impact), and the surfaces of grind other than metal (fall back on
# grind).

CONFIDENCES = ["low", "medium", "high"]

SOUNDS = ["roll", "grind", "slide", "powerslide", "pop", "land", "grind_start", "grind_end",
          "slide_start", "slide_end", "board_impact", "bail", "step_off", "step_on"]
SURFACES = ["concrete", "metal", "wood", "rough"]
SET_NAME = re.compile(r"^(%s)(_(%s))?(-[0-9]{1,2})?\.wav$" % ("|".join(SOUNDS), "|".join(SURFACES)),
                      re.IGNORECASE)

ONE_SHOT_PEAK_DB = -3.0  # as the built-in set
LOOP_RMS_DB = -17.0      # the built-in loops' level
LOOP_PEAK_DB = -1.0
GRAIN_LOOP_START = 0.55  # of the recording
GRAIN_LOOP_SECONDS = 3.0
LOOP_CROSSFADE_SECONDS = 0.15
STITCH_SECONDS = 2.0
STITCH_CROSSFADE_SECONDS = 0.015


class ConvertError(Exception):
    pass


# --- EA "EB" archives ----------------------------------------------------------

class EbArchive:
    """An EA "EB" v3 .big archive (Skate 3, Xbox 360: big-endian)."""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            head = f.read(0x30)
            if len(head) < 0x30 or head[:2] != b"EB":
                raise ConvertError("%s is not an EA EB archive" % path)
            version, count = struct.unpack(">HI", head[2:8])
            if version != 3:
                raise ConvertError("%s: EB version %d, not 3" % (path, version))
            shift = head[10]
            names_offset, _names_size = struct.unpack(">II", head[12:20])
            name_width = head[20]
            table = f.read(16 * count)
            f.seek(names_offset)
            names = f.read(name_width * count)
        self.entries = {}
        for i in range(count):
            offset, _zero, size, _hash = struct.unpack(">IIII", table[16 * i:16 * i + 16])
            raw = names[name_width * i + 2:name_width * (i + 1)]
            name = raw.split(b"\0")[0].decode("latin-1")
            self.entries[name] = (offset << shift, size)

    def read(self, name):
        if name not in self.entries:
            raise ConvertError("%s has no %s" % (os.path.basename(self.path), name))
        offset, size = self.entries[name]
        with open(self.path, "rb") as f:
            f.seek(offset)
            data = f.read(size)
        if len(data) != size:
            raise ConvertError("%s: %s is cut short" % (self.path, name))
        return data


# --- EA "SPLC" sample banks ----------------------------------------------------

class SplcBank:
    """An EA "SPLC" v3 sample bank (.bnk in Skate 3's audiofiles.big).

    0x0c patches, 0x10 groups, 0x18 samples; patches (0x24 bytes: index,
    layer count, ...) from 0x40; then the groups (0x48 bytes: 32 patch slots
    and how many are used); then each patch's layers in order (a block of
    8 bytes, then 0x48 bytes per sample it chooses from, then 4); then the
    sample table (12 bytes each: offset, end, hash), and the samples, each an
    SNR header followed by its data."""

    def __init__(self, data, name="SPLC bank"):
        if data[:4] != b"SPLC" or struct.unpack(">I", data[4:8])[0] != 3:
            raise ConvertError("%s is not an SPLC v3 bank" % name)
        self.data = data
        u32 = lambda o: struct.unpack(">I", data[o:o + 4])[0]
        u16 = lambda o: struct.unpack(">H", data[o:o + 2])[0]
        patches, groups, samples = u32(0x0C), u32(0x10), u32(0x18)
        self.layer_counts = [u16(0x40 + 0x24 * i + 2) for i in range(patches)]
        at = 0x40 + 0x24 * patches - 4
        self.groups = []
        for i in range(groups):
            g = at + 0x48 * i
            used = data[g + 0x44]
            slots = [u16(g + 4 + 2 * k) for k in range(32)]
            self.groups.append([s for s in slots[:used] if s != 0xFFFF])
        at += 0x48 * groups + 4
        self.layers = []  # per layer: the samples it chooses from
        for _ in range(sum(self.layer_counts)):
            if at + 8 > len(data):
                raise ConvertError("%s: the layers run past the end" % name)
            choices = data[at + 4]
            self.layers.append([u16(at + 8 + 0x48 * k) for k in range(choices)])
            at += 8 + 0x48 * choices + 4
        self.first_layer = []
        total = 0
        for count in self.layer_counts:
            self.first_layer.append(total)
            total += count
        # the sample table follows the last layer (whose block has no
        # trailing 4 bytes), and the samples follow it
        table = at - 4
        start = table + 12 * samples
        self.samples = []
        for i in range(samples):
            begin, end, _hash = struct.unpack(">III", data[table + 12 * i:table + 12 * i + 12])
            if not begin <= end <= len(data) - start:
                raise ConvertError("%s: sample %d is out of the bank" % (name, i))
            self.samples.append((start + begin, start + end))
        if samples and (self.samples[0][0] != start or data[start] != 0x03):
            raise ConvertError("%s: the samples are not where the table says" % name)

    def sample(self, index):
        begin, end = self.samples[index]
        return self.data[begin:end]

    def patch_layers(self, patch):
        first = self.first_layer[patch]
        return self.layers[first:first + self.layer_counts[patch]]


# --- decoding --------------------------------------------------------------------

class Decoder:
    def __init__(self, vgmstream, work):
        self.vgmstream = vgmstream
        self.work = work
        self.count = 0

    def version(self):
        try:
            out = subprocess.run([self.vgmstream, "-V"], capture_output=True, text=True).stdout
            return json.loads(out[out.index("{"):]).get("version", "?")
        except (OSError, ValueError):
            return "?"

    def decode(self, path, subsong=None):
        """The file (or its subsong) as mono floats and its rate, played once."""
        self.count += 1
        out = os.path.join(self.work, "decoded-%d.wav" % self.count)
        command = [self.vgmstream, "-i", "-o", out]
        if subsong is not None:
            command += ["-s", str(subsong)]
        command.append(path)
        run = subprocess.run(command, capture_output=True, text=True)
        if run.returncode != 0 or not os.path.exists(out):
            raise ConvertError("vgmstream-cli could not decode %s%s: %s" % (
                os.path.basename(path), "" if subsong is None else " subsong %d" % subsong,
                (run.stderr or run.stdout).strip()[-400:]))
        samples, rate = read_wav(out)
        os.remove(out)
        if not samples:
            raise ConvertError("%s decoded to nothing" % os.path.basename(path))
        return samples, rate

    def write_temp(self, name, data):
        path = os.path.join(self.work, name)
        with open(path, "wb") as f:
            f.write(data)
        return path


def read_wav(path):
    with wave.open(path, "rb") as w:
        channels, width, rate, frames = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(frames)
    if width != 2:
        raise ConvertError("%s: %d-byte samples, expected 16-bit PCM" % (path, width))
    pcm = array.array("h")
    pcm.frombytes(raw)
    if sys.byteorder == "big":
        pcm.byteswap()
    if channels == 1:
        return [s / 32768.0 for s in pcm], rate
    return [sum(pcm[i:i + channels]) / (32768.0 * channels) for i in range(0, len(pcm), channels)], rate


def write_wav(path, samples, rate):
    pcm = array.array("h", (max(-32768, min(32767, int(round(s * 32767.0)))) for s in samples))
    if sys.byteorder == "big":
        pcm.byteswap()
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.tobytes())


# --- shaping ---------------------------------------------------------------------

def db(value):
    return 10.0 ** (value / 20.0)


def peak(samples):
    return max((abs(s) for s in samples), default=0.0)


def rms(samples):
    return math.sqrt(sum(s * s for s in samples) / len(samples)) if samples else 0.0


def scale(samples, gain):
    return [s * gain for s in samples]


def mix(tracks):
    length = max(len(t) for t in tracks)
    out = [0.0] * length
    for t in tracks:
        for i, s in enumerate(t):
            out[i] += s
    return out


def one_shot(samples, rate):
    """Trimmed to the hit (2 ms before it), faded in over 1 ms and out over
    the last 20 ms or so, silence cut off, peak at ONE_SHOT_PEAK_DB."""
    top = peak(samples)
    if top <= 0.0:
        raise ConvertError("a silent one-shot")
    start = next(i for i, s in enumerate(samples) if abs(s) >= top * db(-40))
    start = max(0, start - int(0.002 * rate))
    end = len(samples)
    while end > start + 1 and abs(samples[end - 1]) < top * db(-60):
        end -= 1
    out = samples[start:end]
    fade_in = max(1, int(0.001 * rate))
    for i in range(min(fade_in, len(out))):
        out[i] *= i / fade_in
    fade_out = min(len(out) // 4, int(0.02 * rate))
    for i in range(fade_out):
        out[len(out) - 1 - i] *= 0.5 - 0.5 * math.cos(math.pi * i / fade_out)
    return scale(out, db(ONE_SHOT_PEAK_DB) / peak(out))


def level_loop(samples):
    gain = db(LOOP_RMS_DB) / max(rms(samples), 1e-9)
    gain = min(gain, db(LOOP_PEAK_DB) / max(peak(samples), 1e-9))
    return scale(samples, gain)


def wrap(samples, length, crossfade):
    """samples[:length] as a loop: the crossfade samples after it are faded
    into its start (equal power), so its end runs into its start."""
    if length + crossfade > len(samples):
        raise ConvertError("too short to loop")
    out = samples[:length]
    for i in range(crossfade):
        t = (i + 0.5) / crossfade
        out[i] = out[i] * math.sin(0.5 * math.pi * t) + samples[length + i] * math.cos(0.5 * math.pi * t)
    return out


def join(pieces, crossfade):
    """The pieces one after another, each crossfaded into the next."""
    out = list(pieces[0])
    for piece in pieces[1:]:
        n = min(crossfade, len(out) // 2, len(piece) // 2)
        for i in range(n):
            t = (i + 0.5) / n
            out[len(out) - n + i] = out[len(out) - n + i] * math.cos(0.5 * math.pi * t) \
                + piece[i] * math.sin(0.5 * math.pi * t)
        out.extend(piece[n:])
    return out


# --- the sources ------------------------------------------------------------------

class Game:
    def __init__(self, folder, decoder):
        audio = None
        for candidate in (os.path.join(folder, "data", "audio"), os.path.join(folder, "audio"), folder):
            if os.path.isfile(os.path.join(candidate, "audiofiles.big")):
                audio = candidate
                break
        if audio is None:
            raise ConvertError("%s is not the extracted Skate 3 folder (no data/audio/audiofiles.big)" % folder)
        self.audio = audio
        self.decoder = decoder
        self.archives = {}
        self.files = {}
        self.splc = {}

    def archive(self, name):
        if name not in self.archives:
            self.archives[name] = EbArchive(os.path.join(self.audio, name))
        return self.archives[name]

    def bank_file(self, name):
        """audiofiles.big's entry as a file of its own, for vgmstream."""
        if name not in self.files:
            self.files[name] = self.decoder.write_temp(name, self.archive("audiofiles.big").read(name))
        return self.files[name]

    def grain(self, name):
        data = self.archive("grains.big").read(name + ".grain")
        header = struct.unpack(">I", data[:4])[0]
        if data[header] != 0x03:
            raise ConvertError("%s.grain: no XMA SNR header at 0x%x" % (name, header))
        return self.decoder.decode(self.decoder.write_temp(name + ".snr", data[header:]))

    def splc_bank(self, name):
        if name not in self.splc:
            self.splc[name] = SplcBank(self.archive("audiofiles.big").read(name), name)
        return self.splc[name]

    def splc_sample(self, bank, index):
        return self.decoder.decode(self.decoder.write_temp(
            "%s-%d.snr" % (bank, index), self.splc_bank(bank).sample(index)))


def render(game, row):
    """The row's variants: (samples, rate, what it is) each."""
    kind = row["source"][0]
    out = []
    if kind == "grain":
        samples, rate = game.grain(row["source"][1])
        start = int(len(samples) * GRAIN_LOOP_START)
        loop = wrap(samples[start:], int(GRAIN_LOOP_SECONDS * rate), int(LOOP_CROSSFADE_SECONDS * rate))
        out.append((level_loop(loop), rate, "grains.big/%s.grain, %.2f-%.2f s" % (
            row["source"][1], start / rate, start / rate + GRAIN_LOOP_SECONDS)))
    elif kind == "abk_loops":
        bank = row["source"][1]
        for subsong in row["source"][2]:
            samples, rate = game.decoder.decode(game.bank_file(bank), subsong)
            # the stream loops whole in the game; decoded once, its last
            # 10 ms are faded into its start so it wraps without a click
            seam = int(0.01 * rate)
            loop = wrap(samples, len(samples) - seam, seam)
            out.append((level_loop(loop), rate, "audiofiles.big/%s subsong %d (a loop)" % (bank, subsong)))
    elif kind == "abk_stitch":
        bank = row["source"][1]
        for subsongs in row["source"][2]:
            pieces, seen, rate = [], set(), None
            for subsong in subsongs:
                samples, rate = game.decoder.decode(game.bank_file(bank), subsong)
                key = hashlib.sha1(array.array("f", samples).tobytes()).hexdigest()
                if key not in seen:  # the bank repeats some grains
                    seen.add(key)
                    pieces.append(one_shot(samples, rate))
            crossfade = int(STITCH_CROSSFADE_SECONDS * rate)
            joined = join(pieces, crossfade)
            while len(joined) < (STITCH_SECONDS + LOOP_CROSSFADE_SECONDS) * rate:
                joined = join([joined] + pieces, crossfade)
            length = min(len(joined) - int(LOOP_CROSSFADE_SECONDS * rate), int(STITCH_SECONDS * rate))
            loop = wrap(joined, length, int(LOOP_CROSSFADE_SECONDS * rate))
            out.append((level_loop(loop), rate, "audiofiles.big/%s subsongs %d-%d joined" % (
                bank, subsongs[0], subsongs[-1])))
    elif kind == "abk_shots":
        bank = row["source"][1]
        for subsong in row["source"][2]:
            samples, rate = game.decoder.decode(game.bank_file(bank), subsong)
            out.append((one_shot(samples, rate), rate, "audiofiles.big/%s subsong %d" % (bank, subsong)))
    elif kind == "splc":
        bank_name = row["source"][1]
        bank = game.splc_bank(bank_name)
        for group in row["source"][2]:
            for patch in bank.groups[group]:
                tracks, rate, used = [], None, []
                for layer in bank.patch_layers(patch):
                    if not layer:
                        continue
                    samples, rate = game.splc_sample(bank_name, layer[0])
                    tracks.append(samples)
                    used.append(layer[0])
                if tracks:
                    out.append((one_shot(mix(tracks), rate), rate, "audiofiles.big/%s group %d patch %d (samples %s)" % (
                        bank_name, group, patch, ", ".join(str(s) for s in used))))
    else:
        raise ConvertError("unknown source %r" % (kind,))
    return out[:row.get("max_variants", 6)]


def file_name(name, variant):
    return "%s.wav" % name if variant == 1 else "%s-%d.wav" % (name, variant)


def find_vgmstream(given):
    if given:
        return given
    here = os.path.dirname(os.path.abspath(__file__))
    for name in ("vgmstream-cli.exe", "vgmstream-cli"):
        if os.path.isfile(os.path.join(here, name)):
            return os.path.join(here, name)
        found = shutil.which(name)
        if found:
            return found
    raise ConvertError("vgmstream-cli not found: put it on PATH or beside this script, or pass --vgmstream")


def print_mapping(rows):
    for row in rows:
        print("%-14s %-6s %s" % (row["name"], row["confidence"], row["note"]))


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Writes Skate 3's own sounds as Skate 3 mode's sound set (port/skate/README.md, \"Sound\").")
    parser.add_argument("game", nargs="?", help="the extracted Skate 3 folder (with data/audio)")
    parser.add_argument("out", nargs="?", help="the folder to write: skate-data/sounds beside halo.exe")
    parser.add_argument("--vgmstream", help="vgmstream-cli (default: beside this script, else on PATH)")
    parser.add_argument("--min-confidence", choices=CONFIDENCES, default="low",
                        help="leave the sounds mapped with less confidence to the built-in set (default: low, all)")
    parser.add_argument("--list", action="store_true", help="print the mapping and stop")
    args = parser.parse_args(argv)

    rows = [r for r in MAPPING if CONFIDENCES.index(r["confidence"]) >= CONFIDENCES.index(args.min_confidence)]
    if args.list:
        print_mapping(rows)
        return 0
    if not args.game or not args.out:
        parser.error("the Skate 3 folder and the output folder are needed")

    try:
        vgmstream = find_vgmstream(args.vgmstream)
        os.makedirs(args.out, exist_ok=True)
        work = tempfile.mkdtemp(prefix="skate3-sounds-")
        try:
            decoder = Decoder(vgmstream, work)
            game = Game(args.game, decoder)
            print("extract_sounds: Skate 3 audio in %s, vgmstream-cli %s (%s)" % (
                game.audio, decoder.version(), vgmstream))
            results = []
            for row in rows:
                variants = render(game, row)
                if not variants:
                    raise ConvertError("%s: nothing to write" % row["name"])
                results.append((row, variants))
            # only now, with everything decoded, replace the old set
            for old in sorted(os.listdir(args.out)):
                if SET_NAME.match(old):
                    os.remove(os.path.join(args.out, old))
            lines = [
                "# Skate 3's own sounds, written by port/skate/converter/extract_sounds.py",
                "# from %s; derived from the game: never commit or publish." % os.path.abspath(game.audio),
                "# file\tsound\tconfidence\tsource",
            ]
            for row, variants in results:
                for variant, (samples, rate, what) in enumerate(variants, 1):
                    name = file_name(row["name"], variant)
                    write_wav(os.path.join(args.out, name), samples, rate)
                    lines.append("%s\t%s\t%s\t%s" % (name, row["name"], row["confidence"], what))
                    print("extract_sounds: wrote %-20s %5.2f s %5d Hz  [%s] %s" % (
                        name, len(samples) / rate, rate, row["confidence"], what))
            with open(os.path.join(args.out, "SOURCES.txt"), "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(lines) + "\n")
            written = len(lines) - 3
            print("extract_sounds: %d files for %d sounds in %s" % (written, len(results), os.path.abspath(args.out)))
        finally:
            shutil.rmtree(work, ignore_errors=True)
    except ConvertError as e:
        print("extract_sounds: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
