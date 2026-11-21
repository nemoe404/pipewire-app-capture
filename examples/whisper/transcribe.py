# SPDX-License-Identifier: Apache-2.0
"""Transcribe raw PCM from stdin with whisper.cpp, chunk by chunk.

Reads the output of `pwcap capture`, which is 16000 Hz mono S16LE. The main thread slices stdin
into chunks of --seconds and queues them, so a slow transcription never blocks the capture. One
worker thread writes each chunk to a WAV file in a temporary directory, runs whisper-cli on it, and
deletes the file. Standard library only. Audio stays on this machine: whisper-cli runs as a local
subprocess with an argument list (no shell), and nothing is sent over the network.
"""

import argparse
import os
import pathlib
import queue
import signal
import subprocess
import sys
import tempfile
import threading
import wave

RATE = 16000
BYTES_PER_SECOND = RATE * 2  # mono, 2 bytes per sample
MAX_QUEUED_CHUNKS = 6
MIN_TAIL_SECONDS = 1
STDERR_TAIL_LINES = 5


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Transcribe raw 16 kHz mono S16LE PCM from stdin with whisper.cpp."
    )
    parser.add_argument("--whisper-cli", default=os.environ.get("WHISPER_CLI"),
                        help="path to whisper-cli (default: $WHISPER_CLI)")
    parser.add_argument("--model", default=os.environ.get("WHISPER_MODEL"),
                        help="path to a ggml model file (default: $WHISPER_MODEL)")
    parser.add_argument("--seconds", type=int, default=10,
                        help="chunk length in seconds, 1 to 30 (default: 10)")
    parser.add_argument("--language", default="en", help="language code (default: en)")
    args = parser.parse_args(argv)
    if not 1 <= args.seconds <= 30:
        parser.error("--seconds must be between 1 and 30")
    for label, path in (("--whisper-cli", args.whisper_cli), ("--model", args.model)):
        if not path:
            parser.error(f"{label} is not set; give it or set the environment variable")
        if not pathlib.Path(path).is_file():
            parser.error(f"{label} does not exist: {path}")
    if not os.access(args.whisper_cli, os.X_OK):
        parser.error(f"--whisper-cli is not executable: {args.whisper_cli}")
    return args


def write_wav(path, pcm):
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(RATE)
        wav.writeframes(pcm)


def process_chunk(job, workdir, args, state):
    index, pcm = job
    seconds = index * args.seconds
    minutes, secs = divmod(seconds, 60)
    wav_path = workdir / f"chunk-{index}.wav"
    try:
        write_wav(wav_path, pcm)
        # A list argument, never a shell. SIGINT stays ignored in the child (see main), so a
        # Ctrl-C on the pipeline does not abort a transcription that is already running.
        done = subprocess.run(
            [args.whisper_cli, "-m", args.model, "-f", str(wav_path), "-l", args.language,
             "-nt", "-np"],
            capture_output=True, text=True, encoding="utf-8", errors="replace", check=False,
        )
    finally:
        wav_path.unlink(missing_ok=True)
    if done.returncode != 0:
        state["failed"] = True
        tail = "\n".join(done.stderr.splitlines()[-STDERR_TAIL_LINES:])
        print(f"transcribe: whisper-cli failed on chunk {index} (status {done.returncode})\n{tail}",
              file=sys.stderr, flush=True)
        return
    for line in done.stdout.splitlines():
        text = line.strip()
        if text:
            print(f"[{minutes:02d}:{secs:02d}] {text}", flush=True)


def worker(jobs, workdir, args, state):
    while True:
        job = jobs.get()
        try:
            if job is None:
                return
            process_chunk(job, workdir, args, state)
        except Exception as exc:  # one bad chunk must not stop the rest, or hang the join
            state["failed"] = True
            print(f"transcribe: chunk could not be transcribed: {exc}", file=sys.stderr, flush=True)
        finally:
            jobs.task_done()


def main(argv=None):
    # Ctrl-C on the pipeline also reaches pwcap and this script. pwcap stops and closes the pipe.
    # This script keeps reading until end of input, so the last chunk is still transcribed.
    # Python leaves SIGINT ignored across exec, so whisper-cli ignores it too. Do not reset it in
    # a preexec function.
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    args = parse_args(argv)
    chunk_bytes = args.seconds * BYTES_PER_SECOND
    jobs = queue.Queue()
    state = {"failed": False}
    stdin = sys.stdin.buffer

    with tempfile.TemporaryDirectory(prefix="pwcap-whisper-") as tmp:
        workdir = pathlib.Path(tmp)
        thread = threading.Thread(target=worker, args=(jobs, workdir, args, state),
                                  name="whisper-worker")
        thread.start()
        try:
            index = 0
            while True:
                # BufferedReader.read(n) blocks until n bytes arrive or the input ends.
                chunk = stdin.read(chunk_bytes)
                if len(chunk) < chunk_bytes:
                    tail = len(chunk) - len(chunk) % 2
                    if tail >= BYTES_PER_SECOND * MIN_TAIL_SECONDS:
                        jobs.put((index, chunk[:tail]))
                    elif tail:
                        print(f"transcribe: dropped {tail} bytes at end of input, under "
                              f"{MIN_TAIL_SECONDS} s", file=sys.stderr, flush=True)
                    break
                jobs.put((index, chunk))
                index += 1
                if jobs.qsize() > MAX_QUEUED_CHUNKS:
                    print(f"transcribe: {jobs.qsize()} chunks waiting; transcription is behind "
                          "the capture", file=sys.stderr, flush=True)
        finally:
            jobs.put(None)
            thread.join()
    return 1 if state["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
