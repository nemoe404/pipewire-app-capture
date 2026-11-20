#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Targeted PipeWire graph extracts and a continuous fallback guard for the S0 spikes.

Standard library only. Spawns local pw-* binaries (pw-dump, pw-link via stdbuf) with argv
lists; no shell, no network.

  extract  targeted pw-dump extract: nodes and their ports as JSON lines
  links    resolved link listing from pw-dump (port aliases)
  guard    watch `pw-link -m` events and SIGKILL the meter on the first link from the
           recorder to a microphone (alsa_input) or a sink monitor (:monitor_) port
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import threading
import time

NODE_KEYS = (
    "object.serial",
    "node.name",
    "media.name",
    "application.name",
    "application.process.binary",
    "application.process.id",
    "media.class",
    "target.object",
    "node.autoconnect",
)
MATCH_KEYS = ("node.name", "application.name", "media.name", "application.process.binary")
SIGNS = ("+", "-", "*")


def parse_link(line):
    """`SIGN LINKID |<- PEERID ALIAS` or the `|->` form -> (sign, link, arrow, peer id, alias)."""
    t = line.split(None, 4)
    if len(t) == 5 and t[0] in SIGNS and t[1].isdigit() and t[2] in ("|<-", "|->") and t[3].isdigit():
        return t[0], t[1], t[2], t[3], t[4].strip()
    return None


def parse_port(line):
    """`SIGN PORTID ALIAS` -> (sign, port id, alias)."""
    t = line.split(None, 2)
    if len(t) == 3 and t[0] in SIGNS and t[1].isdigit():
        return t[0], t[1], t[2].strip()
    return None


def run_pw_dump():
    try:
        out = subprocess.run(["pw-dump"], check=True, capture_output=True, text=True).stdout
        return json.loads(out)
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
        print(f"pwgraph: pw-dump failed: {exc}", file=sys.stderr)
        sys.exit(1)


def props_of(obj):
    return (obj.get("info") or {}).get("props") or {}


def node_selected(props, matches):
    if matches is None:
        return str(props.get("media.class", "")).startswith("Stream/")
    needles = [m.lower() for m in matches]
    hay = [str(props.get(k, "")).lower() for k in MATCH_KEYS]
    return any(n in h for n in needles for h in hay)


def cmd_extract(args):
    objs = run_pw_dump()
    nodes = [o for o in objs if o.get("type") == "PipeWire:Interface:Node"]
    ports = [o for o in objs if o.get("type") == "PipeWire:Interface:Port"]
    for node in nodes:
        props = props_of(node)
        if not node_selected(props, args.match):
            continue
        row = {"kind": "node", "id": node["id"]}
        row.update({k: props.get(k) for k in NODE_KEYS})
        print(json.dumps(row))
        for port in ports:
            pp = props_of(port)
            if pp.get("node.id") != node["id"]:
                continue
            print(json.dumps({
                "kind": "port",
                "id": port["id"],
                "node.id": pp.get("node.id"),
                "port.name": pp.get("port.name"),
                "port.direction": pp.get("port.direction"),
                "port.alias": pp.get("port.alias"),
                "audio.channel": pp.get("audio.channel"),
            }))
    return 0


def cmd_links(args):
    objs = run_pw_dump()
    node_names = {o["id"]: props_of(o).get("node.name") for o in objs
                  if o.get("type") == "PipeWire:Interface:Node"}
    alias = {}
    for o in objs:
        if o.get("type") != "PipeWire:Interface:Port":
            continue
        pp = props_of(o)
        alias[o["id"]] = pp.get("port.alias") or f"{node_names.get(pp.get('node.id'))}:{pp.get('port.name')}"
    needles = None if args.match is None else [m.lower() for m in args.match]
    for o in objs:
        if o.get("type") != "PipeWire:Interface:Link":
            continue
        info = o.get("info") or {}
        out_a = alias.get(info.get("output-port-id"), f"port{info.get('output-port-id')}")
        in_a = alias.get(info.get("input-port-id"), f"port{info.get('input-port-id')}")
        if needles is None or any(n in out_a.lower() or n in in_a.lower() for n in needles):
            print(f"{o['id']} {out_a} -> {in_a}")
    return 0


def is_mic_or_monitor(alias):
    return "alsa_input" in alias or ":monitor_" in alias


class GuardState:
    def __init__(self, name):
        self.name = name
        self.lock = threading.Lock()
        self.monitor_lines = 0
        self.rec_ports_seen = 0
        self.rec_links_added = 0
        self.rec_mic_or_monitor_links = 0
        self.rec_link_lines = []
        self.header = ""
        self.fired = threading.Event()
        self.first_line = threading.Event()
        self.eof = threading.Event()

    def feed(self, line):
        line = line.rstrip("\n")
        if not line.strip():
            return
        with self.lock:
            self.monitor_lines += 1
            m = parse_link(line)
            if m:
                sign, link_id, arrow, _peer_id, peer = m
                head = self.header
                head_rec = self.name in head
                peer_rec = self.name in peer
                if head_rec or peer_rec:
                    sym = "<-" if arrow == "|<-" else "->"
                    self.rec_link_lines.append(f"rec_link={sign} {link_id} {head} {sym} {peer}")
                    if sign == "+":
                        self.rec_links_added += 1
                        if (head_rec and is_mic_or_monitor(peer)) or (peer_rec and is_mic_or_monitor(head)):
                            self.rec_mic_or_monitor_links += 1
                            self.fired.set()
            else:
                m = parse_port(line)
                if m:
                    sign, _port_id, alias = m
                    self.header = alias
                    if sign == "+" and self.name in alias:
                        self.rec_ports_seen += 1
        self.first_line.set()


def reader(state, stream):
    try:
        for line in stream:
            state.feed(line)
    except (OSError, ValueError):
        pass
    finally:
        state.eof.set()


def read_pid(path):
    try:
        with open(path, "r", encoding="ascii") as fh:
            text = fh.read().strip()
        return int(text) if text.isdigit() else None
    except (OSError, ValueError):
        return None


def pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def append_log(path, text):
    with open(path, "a", encoding="utf-8") as fh:
        fh.write(text)
        fh.flush()


def cmd_guard(args):
    t0 = time.monotonic_ns()
    state = GuardState(args.rec)
    child = None
    if args.monitor_stdin:
        stream = sys.stdin
    else:
        try:
            child = subprocess.Popen(
                ["stdbuf", "-oL", "pw-link", "-m", "-I", "-o", "-i", "-l"],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
        except OSError as exc:
            print(f"guard: cannot start pw-link monitor: {exc}", file=sys.stderr)
            return 2
        stream = child.stdout
    open(args.log, "w", encoding="utf-8").close()
    threading.Thread(target=reader, args=(state, stream), daemon=True).start()

    pid = None
    ready_at = None
    killed = False
    exit_code = 0
    reason = None
    deadline = time.monotonic() + args.max_s
    try:
        while True:
            now = time.monotonic()
            if ready_at is None and state.first_line.is_set():
                append_log(args.log, "ready\n")
                ready_at = now
            if pid is None and ready_at is not None:
                pid = read_pid(args.pid_file)
                if pid is None and now - ready_at > 5.0:
                    reason = "pid file never appeared within 5 s of ready"
                    exit_code = 2
                    break
            if state.fired.is_set() and pid is not None:
                if pid_alive(pid):
                    os.kill(pid, signal.SIGKILL)
                    killed = True
                    append_log(args.log, f"killed_pid={pid}\n")
                break
            if pid is not None and not pid_alive(pid):
                break
            if (not args.monitor_stdin) and state.eof.is_set() and not state.fired.is_set():
                # The real monitor died: the guard is blind, so fail closed.
                reason = "pw-link monitor ended before the meter exited"
                exit_code = 2
                if pid is not None and pid_alive(pid):
                    os.kill(pid, signal.SIGKILL)
                    killed = True
                    append_log(args.log, f"killed_pid={pid}\n")
                break
            if now > deadline:
                break
            time.sleep(0.02)
    finally:
        if child is not None:
            child.terminate()
            try:
                child.wait(timeout=1)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
    with state.lock:
        lines = list(state.rec_link_lines)
        counts = (state.monitor_lines, state.rec_ports_seen, state.rec_links_added,
                  state.rec_mic_or_monitor_links)
    if counts[0] == 0 and exit_code == 0:
        reason = "monitor produced no lines"
        exit_code = 2
    life_ms = (time.monotonic_ns() - t0) // 1_000_000
    verdict = "fired" if (state.fired.is_set() or killed) else "no"
    out = "".join(f"{ln}\n" for ln in lines)
    out += f"monitor_lines={counts[0]}\nrec_ports_seen={counts[1]}\nrec_links_added={counts[2]}\n"
    out += f"rec_mic_or_monitor_links={counts[3]}\nfallback_guard={verdict}\nguard_life_ms={life_ms}\n"
    append_log(args.log, out)
    if exit_code:
        print(f"guard: {reason}", file=sys.stderr)
    return exit_code


def build_parser():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    e = sub.add_parser("extract", help="targeted pw-dump extract")
    e.add_argument("--match", action="append", default=None, metavar="SUBSTR")
    e.set_defaults(func=cmd_extract)

    lk = sub.add_parser("links", help="resolved link listing")
    lk.add_argument("--match", action="append", default=None, metavar="SUBSTR")
    lk.set_defaults(func=cmd_links)

    g = sub.add_parser("guard", help="continuous microphone/monitor fallback guard")
    g.add_argument("--rec", required=True, help="recorder node name substring")
    g.add_argument("--pid-file", required=True)
    g.add_argument("--log", required=True)
    g.add_argument("--max-s", type=float, default=20.0)
    g.add_argument("--monitor-stdin", action="store_true",
                   help="read pw-link -m style lines from stdin (offline tests)")
    g.set_defaults(func=cmd_guard)
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
