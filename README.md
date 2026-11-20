# pipewire-audio-capture

Capture the audio of **one application** on Linux (a Zoom call, a Google Meet tab, a media
player) without recording the system mix or your microphone. The audio goes out as raw PCM
(pulse-code modulation: uncompressed samples, no header) on stdout or into a C++ callback,
ready for a script, `whisper.cpp`, or any other consumer.

> **Status: pre-alpha.** The link-routing mechanism has been checked by hand on PipeWire 1.6.2.
> The library, CLI (command-line interface)
> and per-app matching are not written yet. Nothing below is a finished feature until a release
> tag says so.

## Why

PipeWire can capture the default output (everything you hear) or the microphone. It cannot
ask for "only Zoom" with a one-line command, and the usual answers involve moving streams
around, which silences the call for you. This library routes the one application's output
into a capture stream and leaves the application playing on your speakers as before.

```bash
# planned interface - not implemented yet
pwcap list
pwcap capture --app zoom --rate 16000 --channels 1 --format s16le | ./transcribe.py
```

## Planned features

- Select sources by `application.name`, `application.process.binary`, `node.name`, or
  `media.name`, with include and exclude rules.
- Output as 16 kHz (kilohertz, samples per second) mono PCM (the format Whisper expects) or
  the native format, on stdout or to a C++ callback.
- Follow the application as its streams appear and disappear (a call starts, a tab closes).
- A C++23 API (application programming interface; `std::expected` errors, RAII handles, that
  is Resource Acquisition Is Initialization, objects that release their resources on destruction)
  and a CLI built on the same core.
- Examples: a Python consumer, a `whisper.cpp` pipeline, and a shell one-liner.

## Limits (read before you rely on it)

- **Linux with PipeWire only.** PulseAudio is supported through `pipewire-pulse`, not as a
  separate server. X11 (the X Window System), Wayland screencasting and macOS/Windows are out
  of scope.
- **Browser tabs: some can be told apart, some cannot.** Tested on PipeWire 1.6.2 with two
  playing tabs in each of Brave 1.100.58 (Chromium-based) and Firefox 160.0-1, both snap builds:
  each browser gave one output node per tab. Firefox puts the page title in `media.name`, so its
  tabs can be matched by that property. Brave's two nodes carry the same properties and differ
  only in id and `object.serial`, so a Brave tab cannot be matched by node properties. Not tested:
  Google Chrome itself, WebRTC calls such as Google Meet, other versions or packaging.
- **A stream restart drops manual links.** Tested on PipeWire 1.6.2 with WirePlumber 0.5.13:
  when an application's stream is killed and started again, its links to the speakers come back
  by themselves within about 10 to 17 ms, but a capture link made by hand disappears about 2 ms
  after the kill and is not re-created within 3 s. Node ids, port ids and `object.serial` all
  change on restart. Not tested: pause and resume, browsers, Zoom, other versions.
- **Targeting by `target.object` is only partly verified.** On the same versions, a capture
  stream targeted by serial or by name was linked to the right application while it played,
  with the same links as manual linking. After the application's stream restarted, a stream
  targeted by name was linked to the microphone within about 6 ms; with
  `node.dont-fallback=true` the stream was removed instead. `target.object` was not tested with
  `node.autoconnect=false`, the setting the capture stream is meant to use.
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
  MPL-2.0 (Mozilla Public License 2.0). It is a C++ library and a Node addon; its README says it
  is not meant for standalone use. This project aims at scripts and a documented CLI.
- [obs-pipewire-audio-capture](https://github.com/dimtpap/obs-pipewire-audio-capture) — an OBS
  (Open Broadcaster Software) Studio plugin, GPL-2.0 (GNU General Public License 2.0). It is an
  application plugin, not a library.

## Built on

This library is the audio-capture layer of **Consigliere**, a visible desktop assistant for
technical interviews. Need to feed Zoom audio into Whisper in real-time? That's exactly what
Consigliere does automatically. Try the full desktop assistant here [Link].

## License

Apache-2.0. See [LICENSE](LICENSE).
