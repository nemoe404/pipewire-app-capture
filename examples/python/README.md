# Python example

`pcm_levels.py` reads raw PCM from stdin, the output of `pwcap capture`, and prints one line per
second: the RMS level and the zero-crossing frequency of that second. At end of input it prints the
total duration.

It uses only the Python standard library and does not open a network connection. Nothing it reads
leaves the process.

## Prerequisites

- `pwcap` installed as described in the top-level README (section Install), with
  `$HOME/.local/bin` on `PATH`.
- Python 3 (the script is checked with `python3 -m py_compile`).
- A running PipeWire session, and `pw-play` (`pipewire-bin`), for the demo tone.

## Demo source

Start the demo tone as in the [shell example](../shell/README.md#demo-source), run from the
repository root:

```bash
python3 tests/pipewire/tools/tones.py generate --freq 440 --seconds 60 --amplitude 0.1 --out tone440.wav
pw-play -P '{ application.name=pwcap-demo }' tone440.wav &
```

## Run

```bash
timeout --preserve-status -s INT 10 pwcap capture --app pwcap-demo | python3 examples/python/pcm_levels.py
```

`timeout` sends SIGINT to `pwcap` after 10 seconds. `pwcap` flushes, closes the pipe and exits 0,
and the script reaches end of input and prints its total.

Sample output from the run on PipeWire 1.6.2, with the 440 Hz tone playing:

```text
t=1s rms=-23.2 dBFS freq=424 Hz
t=2s rms=-23.0 dBFS freq=440 Hz
t=3s rms=-23.0 dBFS freq=440 Hz
t=4s rms=-23.0 dBFS freq=440 Hz
t=5s rms=-23.0 dBFS freq=440 Hz
t=6s rms=-23.0 dBFS freq=440 Hz
t=7s rms=-23.0 dBFS freq=440 Hz
t=8s rms=-23.0 dBFS freq=440 Hz
t=9s rms=-23.0 dBFS freq=440 Hz
t=10s rms=-23.0 dBFS freq=440 Hz
total=10.03 s frames=160427
```

The first second can read a little low while the capture starts.

Each line reports one full second:

- `t=<n>s` is the second, counted from 1.
- `rms=<x.x> dBFS` is the RMS of the mono mix, relative to full scale. 0 dBFS is full scale, and
  a silent second is reported as -120.0.
- `freq=<f> Hz` counts the positive-going zero crossings in that second. For a steady tone this is
  its frequency. For speech or noise it is a rough measure only.

The final line, `total=<s.ss> s frames=<n>`, counts all frames read, including a partial second.

## Ctrl-C

Ctrl-C on the pipeline sends SIGINT to `pwcap` and to this script. The script ignores SIGINT.
`pwcap` stops and closes the pipe, so the script finishes the last second, prints the total and exits
0. Stop the pipeline in one step; do not kill only one side of it.

## Options

| Option | Default | Meaning |
| --- | --- | --- |
| `--rate` | 16000 | sample rate, 16000 or 48000 |
| `--channels` | 1 | 1 or 2; the channels are averaged into one mono mix |
| `--format` | s16le | `s16le` or `f32le` |

The supported pairs are the same as `pwcap`: 16000/1, 48000/1 and 48000/2. Another pair exits 2
before the script reads stdin. To read a 48 kHz stereo capture, use:

```bash
pwcap capture --app pwcap-demo --rate 48000 --channels 2 | python3 examples/python/pcm_levels.py --rate 48000 --channels 2
```

Input that does not end on a whole frame is reported on stderr, with the number of bytes dropped.

## Related

- [Shell example](../shell/README.md): the same capture as one-liners.
- [whisper.cpp example](../whisper/README.md): transcribes the same stream.
