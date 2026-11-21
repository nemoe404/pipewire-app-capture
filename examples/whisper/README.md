# whisper.cpp example

`transcribe.py` reads the PCM from `pwcap capture` and prints a transcript with `whisper.cpp`. It
cuts the stream into chunks (10 seconds by default), writes each chunk to a temporary WAV file, runs
`whisper-cli` on it, and deletes the file.

Nothing leaves the machine. `whisper-cli` runs as a local subprocess with an argument list (no
shell), the WAV chunks live in a temporary directory and are deleted after use, and the script makes
no network call. It uses only the Python standard library.

## Prerequisites

- `pwcap` installed as described in the top-level README (section Install), with
  `$HOME/.local/bin` on `PATH`.
- Python 3 and a running PipeWire session with `pw-play` (`pipewire-bin`).
- `whisper.cpp` built from the fork below, with a ggml model.

## Build whisper.cpp

The example was tested with this fork, branch and commit:

- repository: `https://github.com/nemoe404/whisper.cpp`
- branch: `consigliere-v1.9.5`
- commit: `0d0f753`
- model: `ggml-base.en.bin`

Build it, then export the paths:

```bash
git clone https://github.com/nemoe404/whisper.cpp
cd whisper.cpp
git checkout consigliere-v1.9.5
git rev-parse --short HEAD    # prints 0d0f753
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./models/download-ggml-model.sh base.en
export WHISPER_CLI="$PWD/build/bin/whisper-cli"
export WHISPER_MODEL="$PWD/models/ggml-base.en.bin"
```

What was run in this phase: the existing build of this fork from the earlier spike was used, at
commit `0d0f753` with `CMAKE_BUILD_TYPE=Release`, binary `build/bin/whisper-cli`, and the model
`models/ggml-base.en.bin`. The clone, the configure and build steps, and the model download above
were not run again in this phase. `WHISPER_CLI` and `WHISPER_MODEL` were set to those paths for the
runs below.

## Speech source

To test without a call, play a speech recording as the application `pwcap-demo`. Any WAV file works;
the tests used LibriSpeech clips (CC BY 4.0):

```bash
pw-play -P '{ application.name=pwcap-demo }' speech.wav &
```

## Live transcription (Ctrl-C to stop)

```bash
pwcap capture --app pwcap-demo | python3 examples/whisper/transcribe.py --whisper-cli "$WHISPER_CLI" --model "$WHISPER_MODEL" --seconds 10
```

This runs until Ctrl-C. Ctrl-C reaches `pwcap` and the script at the same time. `pwcap` stops and
closes the pipe. The script ignores SIGINT, keeps going until end of input, and transcribes the last
chunk before it exits. `whisper-cli` also ignores SIGINT, so a transcription that is already running
is not cut short.

## Bounded run

```bash
timeout --preserve-status -s INT 30 pwcap capture --app pwcap-demo | python3 examples/whisper/transcribe.py --whisper-cli "$WHISPER_CLI" --model "$WHISPER_MODEL" --seconds 10
```

`timeout` stops the capture after 30 seconds. The script then transcribes what remains and exits
0.

## Output and exit codes

- Each transcribed line is `[mm:ss] text`, where `mm:ss` is the start of the chunk the text came
  from. Text appears one chunk behind the capture, because a chunk is transcribed after it is full.
- Exit 0: every chunk was transcribed.
- Exit 1: at least one `whisper-cli` run failed. The script prints the last lines of that run's
  stderr and continues with the next chunk.
- Exit 2: a bad argument, such as a missing `--whisper-cli` or model path, or `--seconds` outside
  1 to 30. Nothing is read from stdin in that case.
- If transcription falls behind the capture, a line on stderr says how many chunks are waiting.
- `pwcap` writes its own status lines to stderr too, for example `NoMatchingSource: no application
  stream matches the include rule; waiting` while it waits for the application, and
  `pwcap: matching stream found; linking` when it links. They are not transcription errors.

Options: `--seconds` (1 to 30, default 10), `--language` (default `en`), `--model` and
`--whisper-cli` (default from `$WHISPER_MODEL` and `$WHISPER_CLI`).

## What was tested

- Speech from LibriSpeech clips, played through the speakers with `pw-play` as `pwcap-demo`, with
  the graph checked first: the demo node has exactly two links into the capture node.
- Model `ggml-base.en.bin` with the fork at `0d0f753`.
- A bounded run of 30 seconds, and a simulated Ctrl-C (SIGINT to the whole process group) during a
  live run.

- Bounded run (30 s): 23.16 s of speech from LibriSpeech clips `1272-128104-0000` to `-0002`. The
  transcript held 57 of the 60 reference words, a recall of 0.950 (59 words in the output).
- Live run with a simulated Ctrl-C (SIGINT to the process group) 12 s after the start: the process
  exited 0, and the last partial chunk was transcribed (`[00:10] He tells us`).
- `whisper-cli` does not catch SIGINT. Run under a parent that ignores it, the child showed SIGINT
  as ignored in `/proc/<pid>/status`.
