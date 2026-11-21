#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Restart harness: what happens to a capture link when the application restarts.

Standard library only. Spawns local pw-play, pw-cat, pw-link (via stdbuf), and the sibling
pwgraph.py / tones.py through sys.executable. Argv lists only; no shell, no network.

The restart is a kill and relaunch of pw-play (D-05). Every monitor line and every harness
action is stamped with time.monotonic_ns() on this one process's clock, so there is no skew
between the graph events and the harness actions.

No recorder PCM reaches disk: pw-cat writes to stdout and that pipe feeds
`tones.py goertzel --in -` directly. A recorder link to a microphone or monitor port fires the
fallback guard, which SIGKILLs the meter first so no numbers are computed from that audio.

Exit codes: 0 ok; 1 harness error; 3 fallback guard fired in manual mode (design invariant broken).
"""

import argparse
import os
import signal
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PWGRAPH = os.path.join(HERE, "pwgraph.py")
TONES = os.path.join(HERE, "tones.py")
SIGNS = ("+", "-", "*")
REC = "pwprobe-rec"
TONE = "pwprobe-tone440"
EXIT_FALLBACK_MANUAL = 3


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


def is_mic_or_monitor(alias):
    return "alsa_input" in alias or ":monitor_" in alias


def extract_field(line, key):
    """Pull one flat field out of a pwgraph.py extract row (json is outside the module allowlist)."""
    marker = '"' + key + '": '
    i = line.find(marker)
    if i < 0:
        return None
    rest = line[i + len(marker):]
    if rest.startswith('"'):
        return rest[1:rest.index('"', 1)]
    j = 0
    while j < len(rest) and rest[j] not in ",}":
        j += 1
    return rest[:j].strip()


def classify(head, peer):
    both = head + "|" + peer
    if REC in both and TONE in both:
        return "rec-tone"
    if TONE in both and "alsa_output" in both:
        return "speaker"
    if REC in both:
        return "rec-other"
    return "other"


class Monitor:
    """Stamps `pw-link -m` lines and keeps the graph facts the harness decides on."""

    def __init__(self):
        self.lock = threading.Lock()
        self.events = []  # (t_ns, source, text)
        self.line_count = 0
        self.header_alias = ""
        self.kill_ns = None
        self.pre_kill_tone_ports = set()
        self.removed_after_kill = set()
        self.live_add = set()
        self.rm_seen = set()
        self.new_ports = {}  # alias -> (port id, t_ns)
        self.new_ports_evt = threading.Event()
        self.fallback = threading.Event()
        self.fallback_line = None
        self.mic_monitor_links = 0
        self.link_events = []  # dicts: t, sign, id, head, peer, kind, after_kill
        self.tone_port_removed_ns = None

    def log(self, source, text):
        with self.lock:
            self.events.append((time.monotonic_ns(), source, text))

    def freeze_kill(self, t_ns, pre_ports):
        with self.lock:
            self.kill_ns = t_ns
            self.pre_kill_tone_ports = set(pre_ports)
            self.events.append((t_ns, "harness", "kill"))

    def feed(self, t_ns, line):
        raw = line.rstrip("\n")
        if not raw.strip():
            return
        with self.lock:
            self.line_count += 1
            self.events.append((t_ns, "mon", raw))
            after_kill = self.kill_ns is not None and t_ns >= self.kill_ns
            lk = parse_link(raw)
            if lk:
                self._feed_link(t_ns, raw, lk, after_kill)
                return
            pt = parse_port(raw)
            if pt:
                self._feed_port(t_ns, pt, after_kill)

    def _feed_link(self, t_ns, raw, lk, after_kill):
        sign, link_id, _arrow, _peer_id, peer = lk
        head = self.header_alias
        # Fallback check on EVERY listing, before LINKID de-duplication.
        if sign == "+" and ((REC in head and is_mic_or_monitor(peer))
                            or (REC in peer and is_mic_or_monitor(head))):
            if not self.fallback.is_set():
                self.fallback_line = raw + "   [header: " + head + "]"
            self.mic_monitor_links += 1
            self.fallback.set()
        # One link is listed under both of its ports; ids are recycled, so a removal clears the
        # add-side key and an add clears the removal-side key.
        dup = False
        if sign == "+":
            self.rm_seen.discard(link_id)
            dup = link_id in self.live_add
            self.live_add.add(link_id)
        elif sign == "-":
            self.live_add.discard(link_id)
            dup = link_id in self.rm_seen
            self.rm_seen.add(link_id)
        if not dup and sign in ("+", "-"):
            self.link_events.append({"t": t_ns, "sign": sign, "id": link_id, "head": head,
                                     "peer": peer, "kind": classify(head, peer),
                                     "after_kill": after_kill})

    def _feed_port(self, t_ns, pt, after_kill):
        sign, port_id, alias = pt
        self.header_alias = alias
        if after_kill and sign == "-":
            self.removed_after_kill.add(port_id)
            if TONE in alias and self.tone_port_removed_ns is None:
                self.tone_port_removed_ns = t_ns
        if sign == "+" and after_kill and alias in (TONE + ":output_FL", TONE + ":output_FR"):
            # PipeWire recycles ids: a port is new when its id is absent from the pre-kill set,
            # or when that id was removed after the kill stamp.
            if port_id not in self.pre_kill_tone_ports or port_id in self.removed_after_kill:
                self.new_ports[alias] = (port_id, t_ns)
                if len(self.new_ports) == 2:
                    self.new_ports_evt.set()


def monitor_reader(mon, stream):
    try:
        while True:
            line = stream.readline()
            if not line:
                break
            mon.feed(time.monotonic_ns(), line)
    except (OSError, ValueError):
        pass


def run_capture(argv):
    return subprocess.run(argv, capture_output=True, text=True, check=False).stdout


def rel(t_ns, kill_ns):
    return None if t_ns is None else (t_ns - kill_ns) / 1e6


def fmt(v):
    return "none" if v is None else f"{v:.3f}"


def build_parser():
    p = argparse.ArgumentParser(description="restart harness (kill and relaunch pw-play)")
    p.add_argument("--mode", choices=("manual", "target"), required=True)
    p.add_argument("--run-dir", required=True, help="directory holding tone440.wav and tone880.wav")
    p.add_argument("--tag", required=True)
    p.add_argument("--settle-s", type=float, default=3.0)
    p.add_argument("--wp-grace-ms", type=int, default=3000)
    p.add_argument("--window-s", type=float, default=10.0)
    p.add_argument("--rec-extra-props", default="", help="inserted inside the recorder -P braces")
    return p


class Run:
    def __init__(self, args):
        self.args = args
        self.run_dir = os.path.abspath(args.run_dir)
        self.t0 = time.monotonic_ns()
        self.mon = Monitor()
        self.children = []
        self.errors = []
        self.outcome = None
        self.late = False
        self.relink_cmd_ns = None
        self.relink_codes = None
        self.rec_start_ns = None
        self.rec_stop_ns = None
        self.new_player = None
        self.errlog = None

    def path(self, name):
        return os.path.join(self.run_dir, name.replace("TAG", self.args.tag))

    def spawn(self, argv_list, **kw):
        proc = subprocess.Popen(argv_list, **kw)
        self.children.append(proc)
        return proc

    def mark(self, text):
        self.mon.log("harness", text)

    def sleep_until(self, deadline_ns):
        """Sleep to deadline_ns; return True early if the fallback guard fired."""
        while time.monotonic_ns() < deadline_ns:
            if self.mon.fallback.wait(timeout=0.02):
                return True
        return self.mon.fallback.is_set()

    def tone_argv(self, name, wav):
        return ["pw-play", "-P", "{ node.name=" + name + " }", os.path.join(self.run_dir, wav)]

    def start(self):
        a = self.args
        self.mon_proc = self.spawn(["stdbuf", "-oL", "pw-link", "-m", "-I", "-o", "-i", "-l"],
                                   stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                                   bufsize=1)
        threading.Thread(target=monitor_reader, args=(self.mon, self.mon_proc.stdout),
                         daemon=True).start()
        time.sleep(0.5)
        self.errlog = open(self.path("stderr-TAG.txt"), "w", encoding="utf-8")
        # Distractor first: it shows up in the numbers if a monitor fallback happens.
        self.p880 = self.spawn(self.tone_argv("pwprobe-tone880", "tone880.wav"),
                               stdout=subprocess.DEVNULL, stderr=self.errlog)
        self.p440 = self.spawn(self.tone_argv(TONE, "tone440.wav"),
                               stdout=subprocess.DEVNULL, stderr=self.errlog)
        self.mark("players-spawned")
        props = "node.name=" + REC
        if a.mode == "manual":
            props += " node.autoconnect=false"
        if a.rec_extra_props:
            props += " " + a.rec_extra_props
        rec_argv = ["pw-cat", "-r", "-a", "--rate", "48000", "--channels", "2", "--format", "s16"]
        if a.mode == "target":
            rec_argv += ["--target", TONE]
        rec_argv += ["-P", "{ " + props + " }", "-"]
        goertzel_out = open(self.path("goertzel-TAG.txt"), "w", encoding="utf-8")
        self.rec = self.spawn(rec_argv, stdout=subprocess.PIPE, stderr=self.errlog)
        self.meter = self.spawn([sys.executable, TONES, "goertzel", "--in", "-",
                                 "--freq", "440", "--freq", "880"],
                                stdin=self.rec.stdout, stdout=goertzel_out, stderr=self.errlog)
        self.rec.stdout.close()  # parent's copy; the meter owns the read end now
        self.rec_start_ns = time.monotonic_ns()
        self.mark("rec-start")

    def initial_manual_link(self):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            outs = run_capture(["pw-link", "-o"])
            ins = run_capture(["pw-link", "-i"])
            if (TONE + ":output_FL" in outs and TONE + ":output_FR" in outs
                    and REC + ":input_FL" in ins and REC + ":input_FR" in ins):
                break
            time.sleep(0.1)
        else:
            self.errors.append("ports for the initial manual link did not appear within 5 s")
            return
        self.mark("link-cmd")
        for ch in ("FL", "FR"):
            cp = subprocess.run(["pw-link", TONE + ":output_" + ch, REC + ":input_" + ch],
                                capture_output=True, text=True, check=False)
            if cp.returncode != 0:
                self.errors.append("initial pw-link " + ch + " exited " + str(cp.returncode))

    def snapshot(self, which):
        with open(self.path("links-TAG-" + which + ".txt"), "w", encoding="utf-8") as fh:
            fh.write(run_capture(["pw-link", "-l"]))
        with open(self.path("extract-TAG-" + which + ".jsonl"), "w", encoding="utf-8") as fh:
            fh.write(run_capture([sys.executable, PWGRAPH, "extract", "--match", "pwprobe"]))

    def read_before_ids(self):
        rec_in, pre_ports = {}, set()
        with open(self.path("extract-TAG-before.jsonl"), encoding="utf-8") as fh:
            for line in fh:
                if extract_field(line, "kind") != "port":
                    continue
                alias = str(extract_field(line, "port.alias"))
                port_id = str(extract_field(line, "id"))
                if alias.startswith(REC + ":input_"):
                    rec_in[alias.rsplit("_", 1)[1]] = port_id
                if alias.startswith(TONE + ":output_"):
                    pre_ports.add(port_id)
        return rec_in, pre_ports

    def fallback_abort(self):
        self.mark("fallback-guard")
        if self.meter.poll() is None:
            self.meter.kill()  # discard in-memory sums first: no numbers from this audio
        if self.rec.poll() is None:
            self.rec.send_signal(signal.SIGINT)

    def stop_everything(self):
        if self.rec.poll() is None:
            self.rec.send_signal(signal.SIGINT)
        try:
            self.rec.wait(timeout=3)
            self.meter.wait(timeout=3)
        except subprocess.TimeoutExpired:
            pass
        self.rec_stop_ns = time.monotonic_ns()
        self.mark("rec-stop")
        players = [p for p in (self.p440, self.p880, self.new_player) if p is not None]
        for pp in players:
            if pp.poll() is None:
                pp.send_signal(signal.SIGTERM)
        for pp in players:
            try:
                pp.wait(timeout=3)
            except subprocess.TimeoutExpired:
                pass
        if self.mon_proc.poll() is None:
            self.mon_proc.send_signal(signal.SIGTERM)
            try:
                self.mon_proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                pass
        for pp in self.children:
            if pp.poll() is None:
                pp.kill()
                pp.wait()

    def kill_and_respawn(self, pre_ports):
        kill_t = time.monotonic_ns()
        self.mon.freeze_kill(kill_t, pre_ports)
        self.p440.send_signal(signal.SIGTERM)
        try:
            self.p440.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.p440.kill()
            self.p440.wait()
        self.mark("exited")
        self.new_player = self.spawn(self.tone_argv(TONE, "tone440.wav"),
                                     stdout=subprocess.DEVNULL, stderr=self.errlog)
        self.mark("respawn")
        return kill_t

    def wait_new_ports(self, timeout_s):
        end = time.monotonic() + timeout_s
        while time.monotonic() < end:
            # Poll the fallback guard at 1 ms so a microphone link is cut off almost at once.
            if self.mon.fallback.wait(timeout=0.001) or self.mon.new_ports_evt.is_set():
                break
        return self.mon.new_ports_evt.is_set()

    def rec_tone_added(self, since_ns=None):
        with self.mon.lock:
            return any(e["after_kill"] and e["sign"] == "+" and e["kind"] == "rec-tone"
                       and (since_ns is None or e["t"] >= since_ns) for e in self.mon.link_events)

    def late_link_in_graph(self):
        head, found = "", False
        for ln in run_capture(["pw-link", "-l"]).splitlines():
            if ln and not ln.startswith(" "):
                head = ln.strip()
            elif head.startswith(TONE + ":output_") and "|->" in ln and (REC + ":input_") in ln:
                found = True
        return found

    def relink(self, rec_in):
        fl = self.mon.new_ports.get(TONE + ":output_FL")
        fr = self.mon.new_ports.get(TONE + ":output_FR")
        if not (fl and fr and "FL" in rec_in and "FR" in rec_in):
            self.errors.append("missing port ids for the relink")
            return
        self.relink_cmd_ns = time.monotonic_ns()
        self.mark("relink-cmd")
        codes = []
        for new_id, rec_id in ((fl[0], rec_in["FL"]), (fr[0], rec_in["FR"])):
            cp = subprocess.run(["pw-link", new_id, rec_id], capture_output=True, text=True,
                                check=False)
            codes.append(cp.returncode)
        self.relink_codes = ",".join(str(c) for c in codes)
        if any(c != 0 for c in codes):
            self.errors.append("relink pw-link exited " + self.relink_codes)
            return
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            if self.rec_tone_added(self.relink_cmd_ns) or self.mon.fallback.is_set():
                return
            time.sleep(0.01)
        self.errors.append("no new recorder link in the monitor within 2 s of relink-cmd")

    def run_restart(self, kill_t, rec_in):
        """Return True when the fallback guard fired."""
        a = self.args
        got_ports = self.wait_new_ports(5)
        if self.mon.fallback.is_set():
            return True
        if not got_ports:
            self.errors.append("new pwprobe-tone440 ports never appeared within 5 s")
            return False
        self.mark("new-ports-seen")
        window_end = kill_t + int(a.window_s * 1e9)
        # Observe first, in both modes: touch no link for the grace period; only watch what the
        # graph does by itself. Only the relink below is manual-only.
        if self.sleep_until(time.monotonic_ns() + a.wp_grace_ms * 1_000_000):
            return True
        self.mark("grace-end")
        # Graph state at the end of the grace period, before any harness link command.
        with open(self.path("links-TAG-grace.txt"), "w", encoding="utf-8") as fh:
            fh.write(run_capture(["pw-link", "-l"]))
        self.outcome = "re-created" if self.rec_tone_added() else "disappears"
        if a.mode == "manual" and self.outcome == "disappears":
            if self.late_link_in_graph():
                self.outcome = "re-created"
                self.late = True
            else:
                self.relink(rec_in)
        if self.errors:
            return self.mon.fallback.is_set()
        return self.sleep_until(window_end)

    def write_outputs(self, exit_code):
        a, mon = self.args, self.mon
        with mon.lock:
            events = sorted(mon.events, key=lambda e: e[0])
            kill_ns = mon.kill_ns
            link_events = list(mon.link_events)
            new_ports = dict(mon.new_ports)
        with open(self.path("events-TAG.tsv"), "w", encoding="utf-8") as fh:
            for t_ns, src, text in events:
                fh.write(f"{(t_ns - self.t0) / 1e6:.3f}\t{src}\t{text}\n")
        fired = mon.fallback.is_set()
        s = {k: None for k in ("old_rec_link_removed_ms", "old_tone_ports_removed_ms", "respawn_ms",
                               "new_tone_ports_ms", "new_speaker_link_ms", "wp_new_rec_link_ms",
                               "relink_cmd_ms", "new_rec_link_ms", "relink_latency_ms",
                               "reconciler_gap_ms", "observed_gap_ms")}
        outcome = "none"
        if kill_ns is not None:
            def first(pred):
                return next((e["t"] for e in link_events if e["after_kill"] and pred(e)), None)
            old_rm = rel(first(lambda e: e["sign"] == "-" and e["kind"] == "rec-tone"), kill_ns)
            s["old_rec_link_removed_ms"] = old_rm
            s["old_tone_ports_removed_ms"] = rel(mon.tone_port_removed_ns, kill_ns)
            s["respawn_ms"] = rel(next((t for t, src, tx in events
                                        if src == "harness" and tx == "respawn"), None), kill_ns)
            if len(new_ports) == 2:
                s["new_tone_ports_ms"] = rel(max(v[1] for v in new_ports.values()), kill_ns)
            s["new_speaker_link_ms"] = rel(first(lambda e: e["sign"] == "+" and e["kind"] == "speaker"),
                                           kill_ns)
            s["relink_cmd_ms"] = rel(self.relink_cmd_ns, kill_ns)
            wp_ns = first(lambda e: e["sign"] == "+" and e["kind"] == "rec-tone"
                          and (self.relink_cmd_ns is None or e["t"] < self.relink_cmd_ns))
            s["wp_new_rec_link_ms"] = rel(wp_ns, kill_ns)
            new_rec_ns = wp_ns
            if self.relink_cmd_ns is not None:
                new_rec_ns = first(lambda e: e["sign"] == "+" and e["kind"] == "rec-tone"
                                   and e["t"] >= self.relink_cmd_ns)
                if new_rec_ns is not None:
                    s["relink_latency_ms"] = (new_rec_ns - self.relink_cmd_ns) / 1e6
            s["new_rec_link_ms"] = rel(new_rec_ns, kill_ns)
            if fired:
                outcome = "disappears" if a.mode == "target" else "none"
            elif self.outcome is not None:
                outcome = self.outcome
            if not fired and old_rm is not None:
                if self.relink_cmd_ns is not None:
                    if s["relink_latency_ms"] is not None and s["new_tone_ports_ms"] is not None:
                        s["reconciler_gap_ms"] = (s["new_tone_ports_ms"] - old_rm) + s["relink_latency_ms"]
                elif s["wp_new_rec_link_ms"] is not None:
                    s["reconciler_gap_ms"] = s["wp_new_rec_link_ms"] - old_rm
                if s["new_rec_link_ms"] is not None:
                    s["observed_gap_ms"] = s["new_rec_link_ms"] - old_rm
        window_ms = int(a.window_s * 1000)
        out = [f"mode={a.mode}", f"outcome={outcome}", f"grace_ms={a.wp_grace_ms}",
               f"window_ms={window_ms}",
               f"kill_since_start_ms={fmt(None if kill_ns is None else (kill_ns - self.t0) / 1e6)}"]
        for k, v in s.items():
            if (v is None and a.mode == "target" and not fired and kill_ns is not None
                    and k in ("reconciler_gap_ms", "observed_gap_ms")):
                out.append(f"{k}=none-within-window-{window_ms}ms")
            else:
                out.append(f"{k}={fmt(v)}")
        out.append("relink_exit_codes=" + (self.relink_codes or "none"))
        out.append("late=" + ("yes" if self.late else "no"))
        out.append(f"rec_mic_or_monitor_links={mon.mic_monitor_links}")
        out.append("fallback_guard=" + ("fired" if fired else "no"))
        if mon.fallback_line:
            out.append("fallback_line=" + mon.fallback_line)
        life = None if self.rec_stop_ns is None else (self.rec_stop_ns - self.rec_start_ns) / 1e6
        out.append(f"rec_life_ms={fmt(life)}")
        out.append(f"monitor_lines={mon.line_count}")
        text = "\n".join(out) + "\n"
        with open(self.path("summary-TAG.txt"), "w", encoding="utf-8") as fh:
            fh.write(text)
        sys.stdout.write(text)
        for e in self.errors:
            print("restart_probe: " + e, file=sys.stderr)
        if exit_code == 0 and self.errors:
            return 1
        return exit_code

    def finish(self, exit_code):
        self.snapshot("after")
        self.stop_everything()
        return self.write_outputs(exit_code)


def main(argv=None):
    args = build_parser().parse_args(argv)
    run = Run(args)
    run.start()
    if args.mode == "manual":
        run.initial_manual_link()
    if run.errors:
        run.stop_everything()
        for e in run.errors:
            print("restart_probe: " + e, file=sys.stderr)
        return 1
    fired = run.sleep_until(time.monotonic_ns() + int(args.settle_s * 1e9))
    if run.mon.line_count == 0:
        run.errors.append("the pw-link monitor produced no lines")
        run.stop_everything()
        for e in run.errors:
            print("restart_probe: " + e, file=sys.stderr)
        return 1
    run.snapshot("before")
    rec_in, pre_ports = run.read_before_ids()
    kill_t = None
    if not fired:
        kill_t = run.kill_and_respawn(pre_ports)
        fired = run.run_restart(kill_t, rec_in)
    if fired:
        run.fallback_abort()
        return run.finish(EXIT_FALLBACK_MANUAL if args.mode == "manual" else 0)
    return run.finish(1 if run.errors else 0)


if __name__ == "__main__":
    sys.exit(main())
