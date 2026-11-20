# pipewire-audio-capture

Capture the audio of **one application** on Linux — a Zoom call, a Google Meet tab, a media
player — without recording the system mix or your microphone. The audio goes out as raw PCM
on stdout or into a C++ callback, ready for a script, `whisper.cpp`, or any other consumer.

> **Status: pre-alpha.** The link-routing mechanism has been checked by hand on PipeWire 1.6.2
> (see [`docs/spikes/s0-fanout-probe.md`](docs/spikes/s0-fanout-probe.md)). The library, CLI
> and per-app matching are not written yet. Nothing below is a finished feature until a release
> tag says so.

## Why

PipeWire can capture the default output (everything you hear) or the microphone. It cannot
ask for "only Zoom" with a one-line command, and the usual answers involve moving streams
around, which silences the call for you. This library routes the one application's output
into a capture stream and leaves the application playing on your speakers as before.

```bash
# planned interface — not implemented yet
pwcap list
pwcap capture --app zoom --rate 16000 --channels 1 --format s16le | ./transcribe.py
```

## Planned features

- Select sources by `application.name`, `application.process.binary`, `node.name`, or
  `media.name`, with include and exclude rules.
- Output as 16 kHz mono PCM (the format Whisper expects) or the native format, on stdout or
  to a C++ callback.
- Follow the application as its streams appear and disappear (a call starts, a tab closes).
- A C++23 API (`std::expected` errors, RAII handles) and a CLI built on the same core.
- Examples: a Python consumer, a `whisper.cpp` pipeline, and a shell one-liner.

## Limits (read before you rely on it)

- **Linux with PipeWire only.** PulseAudio is supported through `pipewire-pulse`, not as a
  separate server. X11, Wayland screencasting and macOS/Windows are out of scope.
- **Browser tabs are not separable by default.** Google Meet runs inside Chrome or Firefox,
  which may share one stream for every tab. Matching on the tab title is a best effort and is
  not guaranteed.
- **Stream identity can change.** Some applications create a new stream per call. The library
  re-matches when that happens, but a few samples can be lost during the switch.
- **Recording a call may need consent.** Many jurisdictions require every participant's
  consent to record. You are responsible for that. This library records what you point it at
  and does nothing to notify anyone.
- **Not for hiding from anyone.** The library makes no attempt to conceal a capture from the
  application, the desktop, or screen-sharing and proctoring software.

## Build

Requirements: Linux, `libpipewire-0.3` development files, CMake, and clang 21 with C++23.
Other compilers are not tested.

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

## Related projects

- [venmic](https://github.com/Vencord/venmic) — per-application PipeWire routing for Discord,
  MPL-2.0. It is a C++ library and a Node addon; its README says it is not meant for standalone
  use. This project aims at scripts and a documented CLI.
- [obs-pipewire-audio-capture](https://github.com/dimtpap/obs-pipewire-audio-capture) — an OBS
  Studio plugin, GPL-2.0. It is an application plugin, not a library.

## Built on

This library is the audio-capture layer of **Consigliere**, a visible desktop assistant for
technical interviews. Need to feed Zoom audio into Whisper in real-time? That's exactly what
Consigliere does automatically. Try the full desktop assistant here [Link].

## License

Apache-2.0. See [LICENSE](LICENSE).
