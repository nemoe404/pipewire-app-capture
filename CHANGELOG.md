# Changelog

## v0.1.0

Verified on Ubuntu 26.04 with PipeWire 1.6.2 and WirePlumber 0.5.13.

### Added

- C++ library: `Capture::open` links one application's output into a capture stream and delivers
  PCM to a callback. The application keeps playing on your speakers.
- `pwcap list` and `pwcap capture` command-line tools.
- Examples: a shell one-liner, a Python level script, and a `whisper.cpp` pipeline.

### Verified limits

- Browser tabs: Firefox puts the page title in `media.name`, so its tabs can be matched by that
  property. Brave's two output nodes carry the same node properties and differ only in id and
  `object.serial`, so a Brave tab cannot be matched by node properties. Google Chrome, WebRTC calls
  such as Google Meet, and other browser versions were not verified.
- A capture link made by hand is dropped when the application restarts its stream; the library
  re-links to the new ports.
- `target.object` combined with `node.autoconnect=false` was not verified.
- Link refusal by PipeWire and loss of the daemon during a capture have no live check; the
  no-daemon path is checked with a subprocess.
- The examples were run on test tones and LibriSpeech speech, not on a call.
- Recording a call may need every participant's consent. The library does not notify anyone.
