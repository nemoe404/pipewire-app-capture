# SPDX-License-Identifier: Apache-2.0
"""Print per-second levels of raw PCM read from stdin.

Reads the output of `pwcap capture` (16000 Hz mono S16LE by default). For every full second it
prints the RMS level over the mono mix in dBFS (0 dBFS is full scale) and the number of
positive-going zero crossings in that second, which is the frequency of a steady tone. At end of
input it prints the total duration. Standard library only; no network access.
"""

import argparse
import array
import math
import signal
import sys

# The pairs that pwcap accepts, so this script rejects the same inputs.
SUPPORTED_PAIRS = {(16000, 1), (48000, 1), (48000, 2)}
# Samples per array code and full-scale value for each format.
FORMATS = {"s16le": ("h", 32768.0), "f32le": ("f", 1.0)}
READ_BYTES = 4096
SILENCE_DBFS = -120.0


class SecondMeter:
    """Accumulates the RMS and the positive-going zero crossings for each second of mono samples."""

    def __init__(self, rate):
        self.rate = rate
        self.frames = 0  # all frames seen so far
        self.in_second = 0  # frames in the current, incomplete second
        self.seconds = 0  # completed seconds
        self.sum_squares = 0.0
        self.crossings = 0
        self.previous = 0.0

    def feed(self, mono):
        """Consume normalised mono samples and return the report lines for each completed second."""
        lines = []
        for sample in mono:
            # Count a crossing only from a negative value to a non-negative one, so a silent
            # lead-in (all zeros) adds no crossings and each cycle counts once.
            if self.previous < 0.0 <= sample:
                self.crossings += 1
            self.previous = sample
            self.sum_squares += sample * sample
            self.in_second += 1
            self.frames += 1
            if self.in_second == self.rate:
                self.seconds += 1
                rms = math.sqrt(self.sum_squares / self.rate)
                dbfs = 20.0 * math.log10(rms) if rms > 0.0 else SILENCE_DBFS
                dbfs = max(dbfs, SILENCE_DBFS)
                lines.append(
                    f"t={self.seconds}s rms={dbfs:.1f} dBFS freq={self.crossings} Hz"
                )
                self.in_second = 0
                self.sum_squares = 0.0
                self.crossings = 0
        return lines


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Print per-second RMS level and zero-crossing frequency of PCM on stdin."
    )
    parser.add_argument("--rate", type=int, choices=(16000, 48000), default=16000,
                        help="sample rate in Hz (default: 16000)")
    parser.add_argument("--channels", type=int, choices=(1, 2), default=1,
                        help="channel count (default: 1)")
    parser.add_argument("--format", choices=tuple(FORMATS), default="s16le",
                        help="sample format (default: s16le)")
    args = parser.parse_args(argv)
    if (args.rate, args.channels) not in SUPPORTED_PAIRS:
        parser.error(
            f"unsupported pair {args.rate}/{args.channels}; use 16000/1, 48000/1 or 48000/2"
        )
    return args


def main(argv=None):
    # Ctrl-C on the pipeline also reaches pwcap. pwcap stops, flushes what it has and closes the
    # pipe, and this script ends at end of input. Ignoring SIGINT here lets the final lines print.
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    args = parse_args(argv)
    if sys.byteorder != "little":
        print("pcm_levels: this script assumes a little-endian host", file=sys.stderr)
        return 1

    code, full_scale = FORMATS[args.format]
    frame_bytes = array.array(code).itemsize * args.channels
    meter = SecondMeter(args.rate)
    stdin = sys.stdin.buffer
    pending = b""

    while True:
        chunk = stdin.read(READ_BYTES)
        if not chunk:
            break
        data = pending + chunk
        usable = len(data) - len(data) % frame_bytes
        pending = data[usable:]
        if usable == 0:
            continue
        samples = array.array(code)
        samples.frombytes(data[:usable])
        if args.channels == 2:
            mono = [(left + right) * 0.5 for left, right in zip(samples[0::2], samples[1::2])]
        else:
            mono = samples
        for line in meter.feed(sample / full_scale for sample in mono):
            print(line, flush=True)

    if pending:
        print(
            f"pcm_levels: dropped {len(pending)} bytes at end of input: not a whole number of frames",
            file=sys.stderr,
        )
    print(f"total={meter.frames / args.rate:.2f} s frames={meter.frames}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
