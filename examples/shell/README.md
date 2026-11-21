# Shell example

Shell one-liners that run against the installed `pwcap` command-line tool. They record one
application's audio as raw PCM (16 kHz, mono, signed 16-bit little-endian) and write it to a
file or a pipe.

Nothing here sends audio anywhere. `pwcap` writes to stdout and the shell redirects it.

## Prerequisites

- `pwcap` installed as described in the top-level README (section Install), with
  `$HOME/.local/bin` on `PATH`. Check with `command -v pwcap`.
- A running PipeWire session, and `pw-play` (`pipewire-bin`).
- `timeout` and `head` (coreutils). `ffmpeg` is optional, only for step S3.

## Demo source

These three lines start a 440 Hz test tone played as an application named `pwcap-demo`, so
there is something to record without a real call. The tone is a quiet 60-second WAV.

```bash
python3 tests/pipewire/tools/tones.py generate --freq 440 --seconds 60 --amplitude 0.1 --out tone440.wav
pw-play -P '{ application.name=pwcap-demo }' tone440.wav &
pwcap list
```

- `generate` writes `tone440.wav` (2 channels, 48 kHz, 16-bit) from the standard library.
- `pw-play` plays it in the background (`&`) with the application name `pwcap-demo`.
- `pwcap list` should show a row with `pwcap-demo` in the `APP` column.

Stop the demo with `kill %1` in the same shell, or by closing the terminal.

## Record ten seconds

```bash
timeout --preserve-status -s INT 10 pwcap capture --app pwcap-demo > ten-seconds.raw
```

This stops the capture after 10 seconds with SIGINT. `--preserve-status` makes `timeout` return
`pwcap`'s own exit status, so the command exits 0 after a clean stop.

```bash
pwcap capture --app pwcap-demo | head -c 320000 > ten-seconds.raw
```

This records exactly 10 seconds: 320000 bytes at 16 kHz mono, 2 bytes per sample. When `head`
has its bytes it closes the pipe, and `pwcap` exits 0 when its reader goes away (the stop
semantics are in the top-level README, section Command-line tool).

Convert the raw file to WAV with ffmpeg (optional):

```bash
ffmpeg -loglevel error -y -f s16le -ar 16000 -ac 1 -i ten-seconds.raw ten-seconds.wav
```

## Use a real application

Replace `pwcap-demo` with the application name from `pwcap list`, for example the value in the
`APP` column for the application you want. `--app` matches `application.name` exactly and is
case-sensitive. `--binary`, `--node` and `--media` select by other properties; see the top-level
README.

If the application has no audio output stream, `pwcap capture` waits for one (reconnect is on
by default). Add `--no-reconnect` to make it exit with `pwcap: NoMatchingSource` instead.
