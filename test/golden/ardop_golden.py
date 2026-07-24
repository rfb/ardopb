"""
Shared machinery for the ardopcf golden-vector corpus.

This module knows three things, and deliberately nothing else:

  1. What the corpus contains (the case matrix, below).
  2. How to drive ardopcf to produce and consume audio.
  3. How to parse what ardopcf prints.

Both `gen_golden.py` (which writes the corpus) and `test_golden.py` (which
checks a build against it) are thin scripts over this module, so that the
definition of a case exists in exactly one place.

Nothing here imports from `test/python/`.  That directory is upstream's
working test harness and may change; the golden corpus has to be a frozen,
self-describing artifact, so it carries its own copy of the frame tables.
"""

import gzip
import hashlib
import json
import os
import re
import struct
import subprocess
import tempfile

# Repository root, derived from this file's location rather than from the
# current working directory, so the scripts work from anywhere.
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
APATH = os.path.join(REPO_ROOT, "ardopcf")
GOLDEN_DIR = os.path.join(REPO_ROOT, "test", "golden")
AUDIO_DIR = os.path.join(GOLDEN_DIR, "audio")
MANIFEST_PATH = os.path.join(GOLDEN_DIR, "manifest.json")

# Bumped when the manifest schema changes in a way older checkers cannot read.
FORMAT_VERSION = 1

# Every frame is generated at this drive level.  It is part of the corpus
# definition because it scales the emitted samples, and therefore the TX
# hashes.  30 (of 100) matches what test/python/test_wav_io.py uses, and
# leaves headroom for added noise without clipping.
DRIVE_LEVEL = 30

# ardopcf's fixed audio sample rate.
SAMPLE_RATE = 12000


# ---------------------------------------------------------------------------
# The case matrix
# ---------------------------------------------------------------------------

# Non-data frame types, with example values for their required TXFRAME
# parameters.  These frames carry little or no caller-supplied payload, so
# the assertion for them is that the frame type, session ID and the bytes
# ardopcf reconstructs are stable -- not that a payload round-trips.
CONTROL_FRAMES = [
    ("DataNAK", "50"),                      # 0x00-0x1F  Quality(0-100)
    ("BREAK", ""),                          # 0x23
    ("IDLE", ""),                           # 0x24
    ("DISC", ""),                           # 0x29
    ("END", ""),                            # 0x2C
    ("ConRejBusy", ""),                     # 0x2D
    ("ConRejBW", ""),                       # 0x2E
    ("IDFrame", "n0call dm"),               # 0x30  Callsign Gridsquare
    ("ConReq200M", "n0call n0call-1"),      # 0x31  target local
    ("ConReq500M", "n0call n0call-1"),      # 0x32
    ("ConReq1000M", "n0call n0call-1"),     # 0x33
    ("ConReq2000M", "n0call n0call-1"),     # 0x34
    ("ConReq200F", "n0call n0call-1"),      # 0x35
    ("ConReq500F", "n0call n0call-1"),      # 0x36
    ("ConReq1000F", "n0call n0call-1"),     # 0x37
    ("ConReq2000F", "n0call n0call-1"),     # 0x38
    ("ConAck200", "500"),                   # 0x39  ReceivedLeaderLength
    ("ConAck500", "500"),                   # 0x3A
    ("ConAck1000", "500"),                  # 0x3B
    ("ConAck2000", "500"),                  # 0x3C
    ("PingAck", "10 60"),                   # 0x3D  SNR(0-21) Quality(30-100)
    ("Ping", "n0call n0call-1"),            # 0x3E  target local
]

# Data frame types: (name, carriers, data bytes per carrier, RS bytes per
# carrier).  Frame capacity is carriers * data bytes per carrier.  Each name
# ends in "E"; the corresponding odd-numbered frame replaces it with "O".
DATA_FRAMES = [
    ("4FSK.200.50S.E", 1, 16, 4),           # 0x48, 0x49
    ("4PSK.200.100S.E", 1, 16, 8),          # 0x42, 0x43
    ("4PSK.200.100.E", 1, 64, 32),          # 0x40, 0x41
    ("8PSK.200.100.E", 1, 108, 36),         # 0x44, 0x45
    ("16QAM.200.100.E", 1, 128, 64),        # 0x46, 0x47

    ("4FSK.500.100S.E", 1, 32, 8),          # 0x4C, 0x4D
    ("4FSK.500.100.E", 1, 64, 16),          # 0x4A, 0x4B
    ("4PSK.500.100.E", 2, 64, 32),          # 0x50, 0x51
    ("8PSK.500.100.E", 2, 108, 36),         # 0x52, 0x53
    ("16QAM.500.100.E", 2, 128, 64),        # 0x54, 0x55

    ("4PSK.1000.100.E", 4, 64, 32),         # 0x60, 0x61
    ("8PSK.1000.100.E", 4, 108, 36),        # 0x62, 0x63
    ("16QAM.1000.100.E", 4, 128, 64),       # 0x64, 0x65

    ("4PSK.2000.100.E", 8, 64, 32),         # 0x70, 0x71
    ("8PSK.2000.100.E", 8, 108, 36),        # 0x72, 0x73
    ("16QAM.2000.100.E", 8, 128, 64),       # 0x74, 0x75

    ("4FSK.2000.600S.E", 1, 200, 50),       # 0x7C, 0x7D  FM only
    ("4FSK.2000.600.E", 1, 600, 150),       # 0x7A, 0x7B  FM only
]

# How full to make each data frame, as a fraction of its capacity.  Partially
# filled frames are zero-padded, which produces a distinctly different audio
# pattern, so both full and partial frames are worth freezing.  Each fill
# level also carries a distinct session ID, so session ID decoding is
# exercised across the matrix without multiplying the number of cases.
FILLS = [
    # (label, fraction of capacity, session ID)
    ("full", 1.0, 0xFF),
    ("partial", 0.8, 0x3C),
    ("minimal", 0.1, 0x01),
]

# Frame types for which the actual audio is committed, not just its hash.
#
# Hashes alone prove a modulator is bit-exact, and cost nothing to store, so
# the hash matrix covers every case.  Real audio is only committed where it
# buys something a hash cannot: letting a receiver be tested before its own
# transmitter is trustworthy, and -- for the degraded copies -- capturing
# behaviour that cannot be regenerated at all.
#
# The set is kept deliberately small.  Regenerating the corpus rewrites
# every file, and git stores a fresh blob for each, so a large frozen set
# costs repository size on every regeneration, not just once.  See README.md.
FROZEN_AUDIO_TYPES = [
    "4FSK.200.50S.E",       # narrowest, most robust FSK
    "4PSK.200.100.E",       # baseline PSK
    "8PSK.200.100.E",
    "16QAM.200.100.E",      # densest constellation, single carrier
    "16QAM.2000.100.E",     # densest constellation, eight carriers
    "4FSK.2000.600.E",      # wideband FSK (FM only)
]
FROZEN_AUDIO_CONTROL = ["IDFrame", "ConReq2000M", "DataNAK"]

# Degraded copies are made only of these, a subset of the above.  Noise is
# incompressible, so a noisy vector costs roughly its full uncompressed size
# where a clean one compresses well; this list is therefore shorter, and
# favours the narrow-bandwidth (shorter, cheaper) frames.  16QAM.2000.100.E
# is included despite its size because the two demodulators disagree about
# it under noise, which is exactly the kind of behaviour worth freezing.
NOISE_AUDIO_TYPES = [
    "4FSK.200.50S.E",
    "4PSK.200.100.E",
    "16QAM.200.100.E",
    "16QAM.2000.100.E",
]

# Signal-to-noise ratios, in dB, at which degraded copies are made.  One
# comfortably above threshold (enforced) and one near it (recorded only).
NOISE_SNR_DB = [20, 10]

# At or above this SNR a decode failure is a regression.  Below it, decode
# outcome is chaotic -- a fraction of a dB decides it -- so the corpus
# records what happened without demanding a rebuild reproduce it exactly.
ASSERT_STRICT_SNR_DB = 15


# ---------------------------------------------------------------------------
# Payload generation
# ---------------------------------------------------------------------------

def xorshift32_bytes(seed, count):
    """
    Generate `count` payload bytes from a 32-bit seed.

    This is plain xorshift32, taking the low byte of each state update.  It
    is used instead of Python's `random` module for one reason: the corpus
    has to be reproducible by an implementation written in another language,
    and this generator is four lines in any of them.  The full definition is
    in README.md so the corpus does not depend on this file to be regenerated.
    """
    x = seed & 0xFFFFFFFF
    if x == 0:
        # xorshift32 has a fixed point at zero; no case should use seed 0,
        # but fail safe rather than emitting a constant stream.
        x = 0x9E3779B9
    out = bytearray()
    while len(out) < count:
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        x &= 0xFFFFFFFF
        out.append(x & 0xFF)
    return bytes(out)


def case_seed(case_id):
    """
    Derive a case's payload seed from its ID.

    Seeds are stored explicitly in the manifest, so this is only used when
    generating.  Deriving from the ID keeps a case's payload stable if the
    matrix is reordered or other cases are added or removed.
    """
    digest = hashlib.sha256(case_id.encode("utf-8")).digest()
    return struct.unpack("<I", digest[:4])[0] or 0x9E3779B9


# ---------------------------------------------------------------------------
# WAV handling
#
# ardopcf writes 16-bit mono PCM at 12000 Hz.  These helpers read and write
# exactly that, rather than pulling in a WAV library, so that adding noise
# is transparently specified.
# ---------------------------------------------------------------------------

def wav_read(data):
    """Return (samples, sample_rate) from 16-bit mono PCM WAV bytes."""
    if data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    pos = 12
    rate = None
    channels = None
    bits = None
    while pos + 8 <= len(data):
        chunk_id = data[pos:pos + 4]
        size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if chunk_id == b"fmt ":
            channels = struct.unpack("<H", body[2:4])[0]
            rate = struct.unpack("<I", body[4:8])[0]
            bits = struct.unpack("<H", body[14:16])[0]
        elif chunk_id == b"data":
            if channels != 1 or bits != 16:
                raise ValueError(
                    f"expected 16-bit mono, got {bits}-bit {channels}-channel")
            count = len(body) // 2
            return list(struct.unpack(f"<{count}h", body[:count * 2])), rate
        # Chunks are word-aligned.
        pos += 8 + size + (size & 1)
    raise ValueError("no data chunk")


def wav_write(samples, rate=SAMPLE_RATE):
    """Build 16-bit mono PCM WAV bytes from a sample list."""
    body = struct.pack(f"<{len(samples)}h", *samples)
    fmt = struct.pack("<HHIIHH", 1, 1, rate, rate * 2, 2, 16)
    return (b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(body))
            + b"WAVE"
            + b"fmt " + struct.pack("<I", len(fmt)) + fmt
            + b"data" + struct.pack("<I", len(body)) + body)


def add_awgn(samples, snr_db, seed):
    """
    Add white Gaussian noise at a given SNR, reproducibly.

    Noise power is set relative to the RMS of the non-zero samples, so the
    leading and trailing silence ardopcf writes around a frame does not
    deflate the signal estimate.  Gaussian values come from Box-Muller over
    the same xorshift32 stream used for payloads, so the whole corpus has
    one specified source of randomness.

    The generated WAV is committed and is what a rebuild is checked against;
    this function documents its provenance but is not required to reproduce
    it.
    """
    active = [s for s in samples if s != 0]
    if not active:
        raise ValueError("silent input")
    rms = (sum(float(s) * s for s in active) / len(active)) ** 0.5
    sigma = rms / (10.0 ** (snr_db / 20.0))

    x = seed & 0xFFFFFFFF or 0x9E3779B9

    def next_unit():
        # One uniform value in [0, 1) from 32 bits of xorshift32 state.
        nonlocal x
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        x &= 0xFFFFFFFF
        return x / 4294967296.0

    import math
    out = []
    spare = None
    for s in samples:
        if spare is not None:
            g, spare = spare, None
        else:
            # Box-Muller; guard u1 away from zero before taking its log.
            u1 = next_unit() or 1.0 / 4294967296.0
            u2 = next_unit()
            radius = math.sqrt(-2.0 * math.log(u1))
            g = radius * math.cos(2.0 * math.pi * u2)
            spare = radius * math.sin(2.0 * math.pi * u2)
        v = int(round(s + sigma * g))
        out.append(max(-32768, min(32767, v)))
    return out


# ---------------------------------------------------------------------------
# Driving ardopcf
# ---------------------------------------------------------------------------

class ArdopError(RuntimeError):
    pass


def ardop_version():
    """Read the version string ardopcf reports at startup."""
    res = subprocess.run(
        [APATH, "--nologfile", "--decodewav", os.devnull],
        capture_output=True)
    m = re.search(r"ardopcf Version (\S+)", res.stdout.decode("iso-8859-1"))
    return m.group(1) if m else "unknown"


def transmit(frame_type, args="", session_id=None):
    """
    Modulate one frame and return the WAV bytes ardopcf would have played.

    Uses the NOSOUND device, so no audio hardware is touched and no
    real-time delay is incurred.  MYCALL must be set before any command that
    initiates a transmission, otherwise TXFRAME reports a fault.
    """
    cmd_args = args
    if session_id is not None:
        cmd_args = f"{args} 0x{session_id:02x}".strip()
    hostcmds = (f"CONSOLELOG 2;MYCALL N0CALL;DRIVELEVEL {DRIVE_LEVEL};"
                f"TXFRAME {frame_type} {cmd_args};CLOSE")

    with tempfile.TemporaryDirectory(prefix="ardop-golden-") as tmp:
        res = subprocess.run(
            [APATH, "--nologfile", "--logdir", tmp, "--writetxwav",
             "8515", "NOSOUND", "NOSOUND", "--hostcommands", hostcmds],
            capture_output=True)
        stdout = res.stdout.decode("iso-8859-1")
        if res.returncode != 0:
            raise ArdopError(
                f"ardopcf exited {res.returncode} generating {frame_type}:"
                f"\n{stdout}")
        wavs = sorted(f for f in os.listdir(tmp) if f.endswith(".wav"))
        if len(wavs) != 1:
            raise ArdopError(
                f"expected exactly one WAV for {frame_type}, got {wavs}"
                f"\n{stdout}")
        with open(os.path.join(tmp, wavs[0]), "rb") as f:
            return f.read()


def decode(wav_bytes, sdft=False):
    """
    Run one WAV through ardopcf's receive chain and parse the result.

    --decodewav forces RXO mode and substitutes a sample-derived clock for
    the system clock, so this exercises demodulation and framing with no
    audio device and no wall-clock dependence.  It does not exercise the ARQ
    state machine; see README.md.
    """
    with tempfile.TemporaryDirectory(prefix="ardop-golden-") as tmp:
        path = os.path.join(tmp, "in.wav")
        with open(path, "wb") as f:
            f.write(wav_bytes)
        cmd = [APATH, "--nologfile", "--decodewav", path]
        if sdft:
            cmd.append("--sdft")
        cmd += ["--hostcommands", "CONSOLELOG 2"]
        res = subprocess.run(cmd, capture_output=True)
        if res.returncode != 0:
            raise ArdopError(
                f"ardopcf exited {res.returncode} decoding:"
                f"\n{res.stdout.decode('iso-8859-1')}")
        return parse_decode(res.stdout.decode("iso-8859-1"))


def parse_decode(stdout):
    """
    Extract the decode outcome from ardopcf's console output.

    ardopcf prints three different shapes depending on the frame:

      short control frames (BREAK, DISC, IDLE, ...) carry no payload and no
        Reed-Solomon block, so they get only `[DecodeFrame] Frame: BREAK`
      control frames with content (IDFrame, ConReq, ...) and all data frames
        additionally get `Decode PASS, Quality=..., RS fixed ...`
      anything carrying bytes also gets a `... bytes of data as hex values:`
        dump

    The one line common to every successful decode is

        [RXO xx] <frame type> frame received OK.  frameLen = N

    so that is used as the success signal, and the richer lines are treated
    as optional detail.  Keying off the `Decode PASS` line instead would
    silently classify all seven short control frames as failures.

    Returns a dict with `decoded` always present.
    """
    result = {"decoded": False}

    m = re.search(r"\[RXO ([0-9A-F]{2})\] (\S+) frame received OK\."
                  r"\s+frameLen = (\d+)", stdout)
    if m is None:
        # Record why, so a near-threshold case is still informative.
        if "[Frame Type Decode Fail]" in stdout:
            result["failure"] = "frame_type"
        elif "Decode FAIL" in stdout:
            result["failure"] = "data"
        else:
            result["failure"] = "no_frame"
        return result

    result["decoded"] = True
    result["session_id"] = m.group(1)
    result["frame_type"] = m.group(2)
    result["frame_len"] = int(m.group(3))

    # Quality and RS counts, where the frame type has them.  These are
    # Tier 2 -- recorded for drift detection, not enforced by default.
    m = re.search(
        r"\[DecodeFrame\] Frame: \S+ Decode PASS,\s+Quality=\s*(\d+),"
        r"\s+RS fixed (\d+) \(of (\d+) max\)\.",
        stdout)
    if m is not None:
        result["quality"] = int(m.group(1))
        result["rs_fixed"] = int(m.group(2))
        result["rs_max"] = int(m.group(3))

    # ardopcf currently prints the whole dump on one line, but the character
    # class admits newlines so a future change to wrap it cannot silently
    # truncate the payload here.  The dump is terminated by the next log
    # line, which always begins with '[' after leading whitespace.
    m = re.search(r"\[RXO [0-9A-F]{2}\] \d+ bytes of data as hex values:"
                  r"\s*\n([0-9A-F \n]+)", stdout)
    if m is not None:
        result["payload"] = re.sub(r"\s", "", m.group(1)).lower()
    return result


# ---------------------------------------------------------------------------
# Corpus enumeration and storage
# ---------------------------------------------------------------------------

def enumerate_cases():
    """
    Yield every case in the corpus as a dict, without generating any audio.

    This is the single definition of what the corpus contains.  Both the
    generator and the checker walk it, so they cannot disagree about which
    cases exist.
    """
    for name, params in CONTROL_FRAMES:
        case_id = f"control/{name}"
        yield {
            "id": case_id,
            "kind": "control",
            "frame_type": name,
            "txframe_args": params,
            "session_id": None,
            "freeze_audio": name in FROZEN_AUDIO_CONTROL,
            "noise_audio": False,
        }

    for name, carriers, data_per_carrier, _rs in DATA_FRAMES:
        capacity = carriers * data_per_carrier
        for parity in ("E", "O"):
            frame_type = name[:-1] + parity
            for label, fraction, session_id in FILLS:
                case_id = f"data/{frame_type}/{label}"
                # Freeze audio only for the "E" form at full fill: the "O"
                # form differs only in its frame-type byte, and the point of
                # the frozen set is demodulator coverage, not exhaustiveness.
                representative = (parity == "E" and label == "full")
                yield {
                    "id": case_id,
                    "kind": "data",
                    "frame_type": frame_type,
                    "capacity": capacity,
                    "payload_len": round(fraction * capacity),
                    "payload_seed": case_seed(case_id),
                    "session_id": session_id,
                    "freeze_audio": representative and name in FROZEN_AUDIO_TYPES,
                    "noise_audio": representative and name in NOISE_AUDIO_TYPES,
                }


def audio_name(case_id, snr_db=None):
    """Filename for a case's frozen audio, relative to `audio/`."""
    stem = case_id.replace("/", "_")
    if snr_db is not None:
        stem += f"_snr{snr_db}db"
    return stem + ".wav.gz"


def audio_write(rel_name, wav_bytes):
    os.makedirs(AUDIO_DIR, exist_ok=True)
    path = os.path.join(AUDIO_DIR, rel_name)
    # mtime=0 keeps the gzip container byte-stable across regenerations, so
    # an unchanged vector produces no git diff.
    with gzip.GzipFile(path, "wb", compresslevel=9, mtime=0) as f:
        f.write(wav_bytes)


def audio_read(rel_name):
    with gzip.open(os.path.join(AUDIO_DIR, rel_name), "rb") as f:
        return f.read()


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def load_manifest():
    with open(MANIFEST_PATH, "r", encoding="utf-8") as f:
        manifest = json.load(f)
    if manifest.get("format_version") != FORMAT_VERSION:
        raise ArdopError(
            f"manifest format_version {manifest.get('format_version')},"
            f" this tool expects {FORMAT_VERSION}")
    return manifest


def save_manifest(manifest):
    with open(MANIFEST_PATH, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
        f.write("\n")
