#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Tone generator, streaming Goertzel meter and test-page writer for the live tests.

Standard library only. The goertzel subcommand folds samples into running sums and keeps
nothing else, so a recorder can be piped into it without any PCM reaching disk.
"""

import argparse
import math
import struct
import sys
import wave

CHUNK_BYTES = 64 * 1024
FRAME_BYTES_PER_CHANNEL = 2  # s16le


def cmd_generate(args):
    rate = 48000
    frames = int(args.seconds * rate)
    peak = int(args.amplitude * 32767)
    samples = [round(peak * math.sin(2.0 * math.pi * args.freq * n / rate)) for n in range(frames)]
    data = b"".join(struct.pack("<hh", s, s) for s in samples)
    with wave.open(args.out, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(data)
    return 0


class Goertzel:
    """One running Goertzel state: constant memory, samples are never stored."""

    def __init__(self, freq, rate):
        self.coeff = 2.0 * math.cos(2.0 * math.pi * freq / rate)
        self.s1 = 0.0
        self.s2 = 0.0

    def feed(self, mono):
        coeff = self.coeff
        s1 = self.s1
        s2 = self.s2
        for x in mono:
            s0 = x + coeff * s1 - s2
            s2 = s1
            s1 = s0
        self.s1 = s1
        self.s2 = s2

    def power(self):
        return self.s1 * self.s1 + self.s2 * self.s2 - self.coeff * self.s1 * self.s2


def iter_raw(read, channels):
    """Yield (frames, mono samples) from a raw s16le interleaved byte source.

    `read(n)` returns up to n bytes, b"" at EOF. A read on a pipe can end in the middle of
    a frame; the partial frame is carried into the next chunk so alignment never drifts.
    """
    frame_bytes = FRAME_BYTES_PER_CHANNEL * channels
    carry = b""
    while True:
        chunk = read(CHUNK_BYTES)
        if not chunk:
            break
        buf = carry + chunk
        usable = len(buf) - (len(buf) % frame_bytes)
        carry = buf[usable:]
        if usable == 0:
            continue
        n_frames = usable // frame_bytes
        values = struct.unpack("<" + "h" * (n_frames * channels), buf[:usable])
        # Raw loop: average interleaved channels per frame; no stdlib algorithm does this.
        mono = [sum(values[i:i + channels]) / channels for i in range(0, len(values), channels)]
        yield n_frames, mono


def cmd_goertzel(args):
    total_frames = 0
    try:
        if args.inp.lower().endswith(".wav"):
            with wave.open(args.inp, "rb") as w:
                if w.getsampwidth() != 2:
                    raise ValueError("only 16-bit WAV is supported")
                rate, channels = w.getframerate(), w.getnchannels()
                states = [Goertzel(f, rate) for f in args.freq]
                frame_bytes = FRAME_BYTES_PER_CHANNEL * channels
                for n_frames, mono in iter_raw(lambda n: w.readframes(n // frame_bytes), channels):
                    total_frames += n_frames
                    for st in states:
                        st.feed(mono)
        else:
            rate, channels = args.rate, args.channels
            states = [Goertzel(f, rate) for f in args.freq]
            if args.inp == "-":
                read = sys.stdin.buffer.read
                for n_frames, mono in iter_raw(read, channels):
                    total_frames += n_frames
                    for st in states:
                        st.feed(mono)
            else:
                with open(args.inp, "rb") as fh:
                    for n_frames, mono in iter_raw(fh.read, channels):
                        total_frames += n_frames
                        for st in states:
                            st.feed(mono)
    except (OSError, ValueError, wave.Error, EOFError) as exc:
        print(f"goertzel: cannot read input: {exc}", file=sys.stderr)
        return 2
    if total_frames == 0:
        print("goertzel: input holds zero frames", file=sys.stderr)
        return 2
    print(f"duration_s={total_frames / rate:.3f}")
    powers = {}
    for f, st in zip(args.freq, states):
        powers[f] = st.power()
        print(f"freq={f:g} power={powers[f]:.3e}")
    if args.dominant is not None:
        dom = powers.get(args.dominant)
        if dom is None:
            print("goertzel: --dominant must be one of the --freq values", file=sys.stderr)
            return 2
        others = [p for f, p in powers.items() if f != args.dominant]
        ratio = dom / max(max(others, default=0.0), 1e-12)
        print(f"ratio={ratio:.3e}")
        if args.min_ratio is not None and ratio < args.min_ratio:
            return 3
    return 0


def cmd_page(args):
    for value in (args.title, args.src):
        if any(c in value for c in '"<>&'):
            print("page: title and src must not contain a double quote, <, > or &", file=sys.stderr)
            return 2
    html = (
        "<!doctype html>\n"
        f"<title>{args.title}</title>\n"
        f'<audio src="{args.src}" controls loop></audio>\n'
    )
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write(html)
    return 0


def build_parser():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("generate", help="write a 2 ch 48 kHz s16 sine WAV")
    g.add_argument("--freq", type=float, required=True)
    g.add_argument("--seconds", type=float, required=True)
    g.add_argument("--out", required=True)
    g.add_argument("--amplitude", type=float, default=0.3)
    g.set_defaults(func=cmd_generate)

    m = sub.add_parser("goertzel", help="streaming Goertzel power meter")
    m.add_argument("--in", dest="inp", required=True, help="WAV path, raw s16le path, or - for stdin")
    m.add_argument("--freq", type=float, action="append", required=True)
    m.add_argument("--rate", type=int, default=48000)
    m.add_argument("--channels", type=int, default=2)
    m.add_argument("--dominant", type=float, default=None)
    m.add_argument("--min-ratio", type=float, default=None)
    m.set_defaults(func=cmd_goertzel)

    pg = sub.add_parser("page", help="write a local HTML page with one audio element")
    pg.add_argument("--title", required=True)
    pg.add_argument("--src", required=True)
    pg.add_argument("--out", required=True)
    pg.set_defaults(func=cmd_page)
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
