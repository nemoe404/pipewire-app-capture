#!/usr/bin/env bash
# Starts PipeWire and WirePlumber inside the current D-Bus session and adds a null sink.
# With no hardware on the runner, the null sink is the only sink, so WirePlumber makes it the
# default: the tone the tests play is linked to it, never to a speaker.
set -euo pipefail
pipewire >"${RUNNER_TEMP:-/tmp}/pipewire.log" 2>&1 &
wireplumber >"${RUNNER_TEMP:-/tmp}/wireplumber.log" 2>&1 &
for _ in $(seq 100); do
    pw-cli info 0 >/dev/null 2>&1 && break
    sleep 0.2
done
pw-cli info 0 >/dev/null || { echo "session: no PipeWire answer on pw-cli info 0" >&2; exit 1; }
pw-cli create-node adapter \
    '{ factory.name=support.null-audio-sink node.name=ci-null media.class=Audio/Sink object.linger=true audio.position=[ FL FR ] }' >/dev/null
sleep 1
pw-cli info 0 >/dev/null
echo "session: pipewire $(pipewire --version 2>&1 | head -n 1), null sink created"
