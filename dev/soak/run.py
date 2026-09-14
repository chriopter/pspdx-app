#!/usr/bin/env python3
"""The soak: a hundred customers through the client, one emulator at a time.

    python3 dev/soak/run.py --runs 100 --seed 1
    python3 dev/soak/run.py --perf 20

The first builds the client against the mock catalog on the host, starts that
catalog, and then for each run re-plants the thirty apps' install state,
writes a key script out of scenarios.py, runs the rig, and holds what the
stick says afterwards against what model.py said it would say. The second
runs stress scripts instead -- directions held down through PPSSPP's
debugger, stick swings, installs mid-scroll -- and reports frame times.

Either way the client is rebuilt for the published catalog at the end and the
mock server is stopped, so the desk is left the way it was found.

What the log is worth
---------------------
PSPDX.LOG is the last forty lines of a ring (util/runtime.c), rewritten whole
every ten seconds and after every install. A run of a minute overruns that
ring several times over, so the file at the end is not the run -- it is the
end of the run. This harness therefore reads the file *while* the emulator is
up, several times a second, and stitches the snapshots back together by
their overlap. That gives the whole log, in order, with a host timestamp on
every line, which is also how a window is known to have had an install in it
and how "the last frames: line is within 15 s of the end" is measured.

It is also how a run knows the log is its own: the first snapshot of a run
starts at `font:` and `ripple:`, which are the first two lines the client
writes. The stick is shared with whatever else is on this desk -- the user's
own emulator overwrites the same file -- so a run whose log does not start
there, or whose snapshots stop overlapping, is reported as untrusted rather
than as a pass.
"""

import argparse
import fcntl
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import edge                                             # noqa: E402
import model                                            # noqa: E402
import scenarios                                        # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
APP = os.path.join(REPO, "app")
RIG = os.path.join(HERE, "rig.sh")
RESULTS = os.path.join(HERE, "results")
HOLD = os.path.join(HERE, "hold.mjs")

# One memory stick, one port 8443, and more than one person at this desk.
# Everything from planting the state to copying the log back happens under
# this, so two rigs interleaving cannot be mistaken for a bug in the client.
LOCK_PATH = "/tmp/pspdx-rig.lock"

# From starting the emulator to the catalog being up and the key script's
# clock starting: boot, the seed, the handshake, the fetch and the first
# settled screenshot. Measured at about eleven seconds on this desk; the
# margin only costs idle browsing at the end of a run.
CATALOG_S = 16
# After the last key. A scripted `shot` can draw 360 frames before it
# photographs, and there has to be room for at least one more ten-second
# frames: line after it, or the liveness check has nothing to stand on.
TAIL_S = 24
# PPSSPP does not keep up with a PSP on this workload: thirty rows, a film
# looping behind the card and a piano on another thread come to about six
# guest seconds for every ten of the host's. A key file is written in guest
# milliseconds, so the wall-clock length of a run is the script stretched by
# this. Measured per run afterwards and reported; here it only has to be a
# safe overestimate, because a run that ends before its own last key is
# thrown away and started again.
SLOWDOWN = 1.9

# The client rewrites PSPDX.LOG whole every ten seconds and again after every
# install, so a poll only has to be quicker than two dumps in a row -- which
# a run of ten installs manages -- to see all of them.
POLL_S = 0.6

# ------------------------------------------------------------- log reading

FRAMES_RE = re.compile(
    r"frames: (\d+) in 10 s, avg (\d+) ms, worst (\d+) ms, (\d+) late "
    r"\(17-20 (\d+), 20-25 (\d+), 25-35 (\d+), 35\+ (\d+)\)")
DRAW_RE = re.compile(
    r"slowest draw (\d+) us: back (\d+), front (\d+), end (\d+) "
    r"\(ge (\d+), vblank (\d+)\)")
OUTSIDE_RE = re.compile(
    r"outside draw: tick (\d+) us, audio callback (\d+) us, free (\d+) KB")
SLOW_FRAME_RE = re.compile(r"^frame (\d+): (\d+) ms, (\d+) ms since the shell")

# A window with one of these in it was not browsing: the loop was away
# downloading, hashing, unpacking or photographing, and a frame that took a
# whole second there is the install, not a stutter.
#
# These are only half of how a window is known to be busy, because the ring
# is forty lines and a row rested on writes six of them -- a handshake, a
# status, a picture, a texture, a film, a player. Between two ten-second
# dumps a browsing client can write more than forty, and then the lines in
# the middle are gone before anything can read them. The frames: lines
# themselves are never lost (the dump follows them in the same breath), and
# neither is anything an install writes (install_all dumps after each one),
# but a `shot:` in the middle of a quiet stretch can be. So the other half is
# the clock: the script says when it pressed what, and the first screenshot's
# mtime says when the script's clock started.
BUSY = ("manifest:", "download:", "unpack:", "commit:", "db:", "install:",
        "installed ", "Installed ", "uninstalled ", "Removed ", "recovered:",
        "Install failed", "Remove failed", " installed", "shot:",
        "net up", "catalog:", "updates:", "keys:", "entropy:", "benchmark")

# Lines no run of this harness can produce: the published catalog's host, an
# id from it, or a second client start inside one run. Any of them means
# another emulator had the stick at the same time, which is a fact about the
# desk and not about the client.
FOREIGN = ("chriopter.github.io", "io.github.")

# What must never appear. "failed" on its own catches network failed, write
# failed, install failed and anything new that says so in the same word.
BAD = ("Install failed", "Remove failed", "failed", "refusing", "MISMATCH",
       "not persisted", "did not go", "stayed", "cannot ")


def overlap_append(full, snapshot):
    """Stitch one ring dump onto what is already known. The dump is the last
    forty lines; the longest suffix of what we have that is also a prefix of
    it is where the two meet. Returns the newly added lines, or None when
    nothing matched, which means lines were lost -- a dump missed, or another
    emulator writing the same file."""
    if not full:
        return list(snapshot)
    top = min(len(full), len(snapshot))
    for k in range(top, 0, -1):
        if full[-k:] == snapshot[:k]:
            return list(snapshot[k:])
    # A snapshot wholly inside what we have already (nothing new) still
    # overlaps; one that shares nothing at all does not.
    return None


class LogTail(threading.Thread):
    """PSPDX.LOG as it is being written, put back together."""

    def __init__(self, path):
        super().__init__(daemon=True)
        self.path = path
        self.lines = []                 # (host time, text)
        self.first_snapshot = None
        self.gaps = 0
        self.stop_at = None
        self._stop = threading.Event()

    def snapshot(self):
        try:
            with open(self.path, "r", errors="replace") as fh:
                text = fh.read()
        except OSError:
            return None
        if not text:
            return None
        lines = text.split("\n")
        if lines and lines[-1] == "":
            lines.pop()
        else:
            lines = lines[:-1]          # a dump caught mid-write
        return lines or None

    def poll(self):
        lines = self.snapshot()
        if lines is None:
            return
        if self.first_snapshot is None:
            self.first_snapshot = list(lines)
        added = overlap_append([t for _at, t in self.lines], lines)
        if added is None:
            self.gaps += 1
            added = lines
        now = time.time()
        self.lines.extend((now, t) for t in added)

    def run(self):
        while not self._stop.is_set():
            self.poll()
            self._stop.wait(POLL_S)

    def finish(self):
        self._stop.set()
        self.join(timeout=5)
        self.poll()                     # the last dump, after the emulator went


# ------------------------------------------------------------------ checks

def busy_spans(pred, keys, catalog_up, slowdown):
    """When the loop was away, in host time. The script's clock starts when
    the catalog is up, which is the moment the first screenshot was written,
    so every key's moment is known without reading the log for it.

    The two clocks do not run at the same rate: a key file is timed in the
    PSP's milliseconds, and PPSSPP on this workload gets through about six
    of those for every ten of the host's. The factor is measured from the run
    itself -- the scripted `shot` is the last key, and its screenshot's mtime
    says when the guest got to it.

    A batch takes its events at one press: they are counted, and the span is
    stretched to cover the run of them."""
    def host(ms):
        return catalog_up + ms / 1000.0 * slowdown

    spans = [(0.0, catalog_up + 3.0)]           # the sync and the first shot
    by_press = {}
    for kind, at, _id in pred["events"]:
        by_press[(kind, at)] = by_press.get((kind, at), 0) + 1
    for (kind, at), n in by_press.items():
        start = host(at) - 2.0
        # An install of one of these packages is a handshake, 85 KB, a
        # hash, one file unpacked and two renames: under a second of guest
        # time, measured. Three is the margin, and the log's own lines are
        # what actually marks a window busy -- dump_diagnostics() runs after
        # every one of them, so they are never the lines that get lost.
        if kind == "install":
            spans.append((start, start + 2.0 + n * 3.0 * slowdown))
        elif kind == "remove":
            spans.append((start, start + 2.0 + n * 2.0 * slowdown))
        else:                                   # a refetch of the catalog
            spans.append((start, start + 14.0 * slowdown))
    if keys:
        last = host(keys[-1][0])
        spans.append((last - 2.0, last + 10.0))  # the settled screenshot
    return spans


def windows(lines, spans=()):
    """The run cut at every frames: line: what happened in each ten seconds,
    and whether any of it was an install. A window runs from the frames: line
    before it to its own."""
    out = []
    held = []
    started = lines[0][0] if lines else 0.0
    for at, text in lines:
        m = FRAMES_RE.search(text)
        if not m:
            held.append((at, text))
            continue
        body = [t for _a, t in held]
        busy = any(any(b in t for b in BUSY) for t in body)
        if not busy:
            busy = any(s < at and e > started for s, e in spans)
        out.append({
            "at": at, "from": started,
            "frames": int(m.group(1)), "avg": int(m.group(2)),
            "worst": int(m.group(3)), "late": int(m.group(4)),
            "buckets": [int(m.group(i)) for i in range(5, 9)],
            "busy": busy,
            "lines": body,
        })
        held = []
        started = at
    return out


def mtime(path):
    """When a file on the stick was last closed, or None if it is not there.
    PPSSPP writes an emulated file through to the host on close, so this is
    the moment the client wrote it."""
    try:
        return os.path.getmtime(path)
    except OSError:
        return None


def read_stick(ms, world):
    """What the mock's third of the stick looks like now. Only the mock's own
    ids and directory names are read: the user's real installs live in the
    same two directories and are none of this harness's business."""
    db_dir = os.path.join(ms,"PSP/PSPDX/INSTALLED")
    game_dir = os.path.join(ms,"PSP/GAME")
    records = {}
    for name in os.listdir(db_dir) if os.path.isdir(db_dir) else []:
        if not name.startswith(world["id_prefix"]) or not name.endswith(".state.json"):
            continue
        app_id = name[:-len(".state.json")]
        with open(os.path.join(db_dir, name)) as source:
            installed = json.load(source)["installed"]
        records[app_id] = dict(id=app_id, rev=installed["published_at"],
                               dir=installed["installdir"][9:],
                               version=installed["version"], manifest="")
    dirs=[]
    if os.path.isdir(game_dir):
        dirs = sorted(n for n in os.listdir(game_dir)
                      if n.startswith(world["dir_prefix"]))
    return records, dirs


def compare_state(pred, records, dirs):
    """The prediction against the stick, field by field, so a mismatch names
    the record and the field rather than printing two dictionaries."""
    problems = []
    want = pred["db"]
    for app_id in sorted(set(want) | set(records)):
        if app_id not in records:
            problems.append("record missing: %s" % app_id)
            continue
        if app_id not in want:
            problems.append("record unexpected: %s" % app_id)
            continue
        for field in ("id", "rev", "dir", "manifest", "version"):
            if records[app_id].get(field) != want[app_id][field]:
                problems.append("%s: %s is %r, expected %r"
                                % (app_id, field, records[app_id].get(field),
                                   want[app_id][field]))
    if sorted(dirs) != sorted(pred["dirs"]):
        missing = sorted(set(pred["dirs"]) - set(dirs))
        extra = sorted(set(dirs) - set(pred["dirs"]))
        if missing:
            problems.append("PSP/GAME missing: %s" % ", ".join(missing))
        if extra:
            problems.append("PSP/GAME unexpected: %s" % ", ".join(extra))
    return problems


def check(result, tail, pred, records, dirs, ms, began_at, ended_at, keys, world):
    """(a) the log is ours, (b) nothing failed and it was still alive at the
    end, (c) the stick matches the model, (d) no late frames outside the
    install windows, (e) the screenshot the script asked for is there."""
    fails = []
    lines = [t for _a, t in tail.lines]

    first = tail.first_snapshot or []
    if len(first) < 2 or not first[0].startswith("font:") or \
            not first[1].startswith("ripple:"):
        fails.append("a: the log did not start at font:/ripple: -- "
                     "first lines were %r" % first[:2])
    boots = [t for t in lines if t.startswith("font:")]
    if len(boots) > 1:
        fails.append("a: the client started %d times in one run; another "
                     "emulator is writing this stick" % len(boots))
    for text in lines:
        if any(f in text for f in FOREIGN):
            fails.append("a: %r is not from this run; another emulator is "
                         "writing this stick" % text)
            break
    # A gap is not by itself a foreign log: forty lines is the whole ring and
    # a browsing client can overrun it between two dumps. It is recorded so a
    # run that looks odd can be read with that in mind.
    result["log_gaps"] = tail.gaps
    scripted = [t for t in lines if t.startswith("keys: ")]
    if not scripted:
        fails.append("a: the client never loaded a key script")
    elif scripted[0] != "keys: %d scripted" % len(keys):
        fails.append("a: the client loaded %r, not %d keys"
                     % (scripted[0], len(keys)))
    # The mock catalog is thirty apps and the published one is three, so
    # this line says both that the fetch worked and that the EBOOT on the
    # stick was the one built for the host. A run without it read somebody
    # else's catalog, or none.
    # The line is "catalog: N of M sources, X apps, Y usable" now that the
    # catalog comes through sources.txt; the sources are the rig's own and
    # not what is being judged, so only the tail is held.
    count = len(world["apps"])
    want = "%d apps, %d usable" % (count, count)
    if not any(t.startswith("catalog:") and t.endswith(want) for t in lines):
        got = [t for t in lines if t.startswith("catalog:")]
        fails.append("a: no 'catalog: ... %s' in the log -- saw %r" % (want, got[:2]))

    for text in lines:
        for bad in BAD:
            if bad in text:
                fails.append("b: %r" % text)
                break

    # (e), and the clock the windows are read against. PSPDX.BMP is written
    # the moment the catalog is up, immediately before the key script's own
    # clock starts, and PSPDX1.BMP when the script's last key asks for it.
    # Both are closed straight away, so the host's mtime is the moment.
    catalog_up = mtime(os.path.join(ms, "PSP/PSPDX/DEBUG/PSPDX.BMP"))
    shot_at = mtime(os.path.join(ms, "PSP/PSPDX/DEBUG/PSPDX1.BMP"))
    if shot_at is None:
        fails.append("e: PSPDX1.BMP was not written")
    elif shot_at < began_at:
        fails.append("e: PSPDX1.BMP is older than this run")
    if catalog_up is None:
        fails.append("a: PSPDX.BMP was not written; the catalog never came up")
        catalog_up = began_at

    # How much slower than a wall clock the guest ran. The script's last key
    # is the shot, so its screenshot dates the end of the script in host time
    # and the whole file can be placed against the log.
    slowdown = 1.0
    if shot_at and catalog_up and keys and keys[-1][0] > 5000:
        slowdown = (shot_at - catalog_up) / (keys[-1][0] / 1000.0)
        if not 0.7 <= slowdown <= 5.0:
            fails.append("a: the guest ran at %.2fx the host's clock, which is "
                         "not a run anything can be read off" % slowdown)
            slowdown = max(0.7, min(5.0, slowdown))
    result["slowdown"] = round(slowdown, 2)

    spans = busy_spans(pred, keys, catalog_up, slowdown)
    win = windows(tail.lines, spans)
    result["windows"] = len(win)
    result["busy_windows"] = [i for i, w in enumerate(win) if w["busy"]]
    if not win:
        fails.append("b: no frames: line at all -- the loop never ran ten "
                     "seconds")
    else:
        # Fifteen seconds in the client's own clock, which is the one the
        # ten-second dump interval is counted in: at 0.6x that is
        # twenty-three of the host's, and a loop that has actually stopped
        # is silent for very much longer than either.
        gap = ended_at - win[-1]["at"]
        result["silence_s"] = round(gap, 1)
        result["silence_guest_s"] = round(gap / slowdown, 1)
        if gap > 15.0 * slowdown:
            fails.append("b: the last frames: line was %.0f s (%.0f in the "
                         "guest's clock) before the emulator was stopped"
                         % (gap, gap / slowdown))
        if shot_at is not None and win[-1]["at"] < shot_at:
            fails.append("b: nothing was logged after the shot -- the run was "
                         "cut short, give it more tail")

    problems = compare_state(pred, records, dirs)
    fails.extend("c: " + p for p in problems)

    late = [(i, w["late"], w["worst"]) for i, w in enumerate(win)
            if w["late"] and not w["busy"]]
    result["late_windows"] = late
    for i, n, worst in late:
        fails.append("d: window %d (%.0f-%.0f s in) had %d late frames "
                     "(worst %d ms) with no install in it"
                     % (i, win[i]["from"] - began_at, win[i]["at"] - began_at,
                        n, worst))

    result["worst_frame_ms"] = max([w["worst"] for w in win], default=0)
    result["late_total"] = sum(w["late"] for w in win)
    return fails


# --------------------------------------------------------------- one run

def wait_idle(timeout=180):
    """Nobody else's emulator on the stick. The desk instance is stopped the
    way dev/start stops it; another rig's is waited out, because killing it
    would corrupt somebody else's run instead of this one."""
    until = time.time() + timeout
    while True:
        out = subprocess.run(["sh", RIG, "idle"], capture_output=True,
                             text=True).stdout.strip().splitlines()
        if out and out[-1] == "idle":
            return True
        if time.time() > until:
            return False
        print("        waiting for another emulator to finish")
        time.sleep(10)


# The build this campaign runs, copied aside once it is made: app/EBOOT.PBP
# is whatever the last make left there, and a make during a two-hour
# campaign would otherwise run some other client against the mock's
# expectations from that run on.
CAMPAIGN_EBOOT = os.path.join(RESULTS, "EBOOT.PBP")


def keep_build():
    import shutil
    os.makedirs(RESULTS, exist_ok=True)
    shutil.copyfile(os.path.join(APP, "EBOOT.PBP"), CAMPAIGN_EBOOT)


def run_rig(secs, keyfile, extra=()):
    env = dict(os.environ, EBOOT=CAMPAIGN_EBOOT)
    return subprocess.Popen(
        ["sh", os.path.join(REPO, "dev", "rig"), str(secs),
         "--keys", keyfile, *extra],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)


def keep_evidence(tag, ms, keyfile, tail):
    """A run that failed leaves its log, its keys and its screenshot behind;
    a run that passed leaves one line of JSON."""
    out = os.path.join(RESULTS, tag)
    os.makedirs(out, exist_ok=True)
    shutil.copyfile(keyfile, os.path.join(out, "keys.txt"))
    began = tail.lines[0][0] if tail.lines else 0.0
    with open(os.path.join(out, "PSPDX.LOG"), "w") as fh:
        if not tail.lines:
            fh.write("nothing was ever written to PSPDX.LOG\n")
        for at, text in tail.lines:
            fh.write("%8.1f  %s\n" % (at - began, text))
    for name in ("PSPDX1.BMP", "PSPDX.BMP", "PSPDX2.BMP"):
        src = os.path.join(ms, "PSP/PSPDX/LOGS/pspdx.log" if name == "PSPDX.LOG" else "PSP/PSPDX/DEBUG/" + name)
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(out, name))
    return out


def one_run(paths, world, seed, run, lock, stretch=SLOWDOWN):
    work, ms, _app = paths
    plan = scenarios.build(world, seed, run)
    keys = plan.script
    text = scenarios.render(keys)
    keyfile = os.path.join(RESULTS, "keys-%d-%d.txt" % (seed, run))
    open(keyfile, "w").write(text)

    # The prediction is made from the file that was written, not from the
    # planner's own walk: the two agreeing is itself a check on the model.
    pred = model.simulate(world, model.parse_script(text))
    if pred != plan.sim.prediction():
        raise SystemExit("scenarios and model disagree on run %d" % run)

    secs = int(CATALOG_S + keys[-1][0] / 1000.0 * stretch + TAIL_S)
    result = {
        "seed": seed, "run": run, "profiles": plan.profiles,
        "keys": len(keys), "seconds": secs,
        "predicted_records": len(pred["db"]),
        "installs": pred["installs"],
        "removes": sum(1 for e in pred["events"] if e[0] == "remove"),
    }

    with lock:
        wait_idle()
        subprocess.run(["sh", RIG, "plant"], check=True,
                       stdout=subprocess.DEVNULL)
        logpath = os.path.join(ms, "PSP/PSPDX/LOGS/pspdx.log")
        for name in ("PSPDX.LOG", "PSPDX1.BMP", "PSPDX2.BMP"):
            try:
                os.remove(os.path.join(ms, "PSP/PSPDX/LOGS/pspdx.log" if name == "PSPDX.LOG" else "PSP/PSPDX/DEBUG/" + name))
            except OSError:
                pass
        tail = LogTail(logpath)
        began_at = time.time()
        proc = run_rig(secs, keyfile)
        tail.start()
        proc.wait()
        ended_at = time.time()
        tail.finish()
        records, dirs = read_stick(ms, world)
        fails = check(result, tail, pred, records, dirs, ms, began_at, ended_at,
                      keys, world)
        tag = "%d-%d" % (seed, run)
        if fails:
            result["kept"] = keep_evidence(tag, ms, keyfile, tail)
        else:
            os.remove(keyfile)

    result["ok"] = not fails
    result["failures"] = fails
    json.dump(result, open(os.path.join(RESULTS, "%d-%d.json" % (seed, run)), "w"),
              indent=1)
    return result


# ------------------------------------------------------------------- perf

# A stress script is not a customer: it is a thumb held on a direction until
# something gives. main.c's repeat() takes a held direction to twenty-five
# rows a second, which a key file cannot ask for -- a scripted press lasts
# one frame. So the holding is done through PPSSPP's own debugger, in frames,
# by hold.mjs, while a key file does the parts a press can do.
PERF_HOLDS = [
    # (button, frames): five seconds of held scrolling is 300 frames.
    ("down", 300), ("up", 300), ("down", 300),
    ("right", 60), ("down", 300), ("left", 60), ("up", 180),
]


def perf_program(rng):
    """One stress run: held scrolling, tab sweeps and stick swings, with the
    key file's installs landing in the middle of them."""
    program = [{"op": "wait", "ms": 3000}]
    for button, frames in PERF_HOLDS:
        program.append({"op": "hold", "button": button, "frames": frames})
        program.append({"op": "wait", "ms": 400})
        if rng.random() < 0.5:
            for _ in range(rng.randint(2, 5)):
                program.append({"op": "analog",
                                "x": round(rng.uniform(-1, 1), 3),
                                "y": round(rng.uniform(-1, 1), 3),
                                "ms": rng.randint(120, 400)})
            program.append({"op": "analog", "x": 0, "y": 0, "ms": 100})
    return program


def perf_keys(world, rng, run):
    """The half a key file can do: an install or two, started while the
    holding is going on, and the film left playing on a row that has one."""
    plan = scenarios.Planner(world, rng)
    plan.wait(20000)                    # the first holds have the list to
                                        # themselves
    want = [a.index for a in plan.sim.apps if a.state == model.NOT_INSTALLED]
    rng.shuffle(want)
    for index in want[:2]:
        if not plan.goto_app(index):
            continue
        plan.key("cross", scenarios.GAP_ACT)
        plan.key("cross", scenarios.GAP_ACT)
        plan.wait(scenarios.INSTALL_MS)
        plan.wait(6000)                 # the card fetches and the film starts
    plan.wait(4000)
    plan.key("shot", scenarios.GAP_ACT)
    plan.sim.finish()
    return plan


def perf_run(paths, world, seed, run, lock):
    import random
    rng = random.Random((seed << 20) ^ (run * 40503) ^ 0x50455246)
    work, ms, _app = paths
    plan = perf_keys(world, rng, run)
    program = perf_program(rng)
    keyfile = os.path.join(RESULTS, "perf-keys-%d-%d.txt" % (seed, run))
    open(keyfile, "w").write(scenarios.render(plan.script))
    progfile = os.path.join(RESULTS, "perf-prog-%d-%d.json" % (seed, run))
    json.dump(program, open(progfile, "w"))

    held_ms = sum(p.get("ms", 0) + p.get("frames", 0) * 1000 // 60
                  for p in program)
    secs = int(CATALOG_S + max(plan.script[-1][0] / 1000.0 * SLOWDOWN,
                               held_ms / 1000.0) + TAIL_S)
    result = {"seed": seed, "run": run, "perf": True, "seconds": secs,
              "keys": len(plan.script)}

    with lock:
        wait_idle()
        subprocess.run(["sh", RIG, "plant"], check=True, stdout=subprocess.DEVNULL)
        for name in ("PSPDX.LOG", "PSPDX1.BMP", "PSPDX2.BMP"):
            try:
                os.remove(os.path.join(ms, "PSP/PSPDX/LOGS/pspdx.log" if name == "PSPDX.LOG" else "PSP/PSPDX/DEBUG/" + name))
            except OSError:
                pass
        tail = LogTail(os.path.join(ms, "PSP/PSPDX/LOGS/pspdx.log"))
        began_at = time.time()
        proc = run_rig(secs, keyfile)
        tail.start()
        driver = subprocess.Popen(["node", HOLD, progfile],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True)
        proc.wait()
        ended_at = time.time()
        try:
            driver.wait(timeout=10)
        except subprocess.TimeoutExpired:
            driver.kill()
        # The whole of it would be one line per hold; what matters is
        # whether the debugger refused any of them, and how it ended.
        out = (driver.stdout.read() or "").strip().splitlines()
        result["driver"] = ([l for l in out if "error" in l.lower()] +
                            out[-2:])
        tail.finish()

    # The same clock the soak runs are read against: the shot is the script's
    # last key, so its screenshot dates the guest against the host, and the
    # installs the key file asked for become windows the frame times are not
    # judged in.
    catalog_up = mtime(os.path.join(ms, "PSP/PSPDX/DEBUG/PSPDX.BMP")) or began_at
    shot_at = mtime(os.path.join(ms, "PSP/PSPDX/DEBUG/PSPDX1.BMP"))
    slowdown = 1.0
    if shot_at and plan.script:
        slowdown = max(0.7, min(5.0, (shot_at - catalog_up) /
                                (plan.script[-1][0] / 1000.0)))
    pred = plan.sim.prediction()
    spans = busy_spans(pred, plan.script, catalog_up, slowdown)
    win = windows(tail.lines, spans)
    fails = []
    if not win:
        fails.append("no frames: line at all")
    if shot_at is None:
        fails.append("PSPDX1.BMP was not written")
    worst_back = worst_front = worst_audio = 0
    free_kb = []
    for at, text in tail.lines:
        m = DRAW_RE.search(text)
        if m:
            worst_back = max(worst_back, int(m.group(2)))
            worst_front = max(worst_front, int(m.group(3)))
        m = OUTSIDE_RE.search(text)
        if m:
            worst_audio = max(worst_audio, int(m.group(2)))
            free_kb.append(int(m.group(3)))
        m = SLOW_FRAME_RE.search(text)
        if m and int(m.group(2)) > 100:
            # A "frame N: X ms" line only exists above 100 ms, so any of them
            # outside an install or a screenshot is the failure.
            near = [w for w in win if at <= w["at"]]
            if not near or not near[0]["busy"]:
                fails.append("frame of %s ms with no install or shot in that "
                             "window" % m.group(2))
    for i, w in enumerate(win):
        if w["late"] and not w["busy"]:
            fails.append("window %d: %d late frames (worst %d ms)"
                         % (i, w["late"], w["worst"]))
    gap = ended_at - win[-1]["at"] if win else 999
    if gap > 15.0 * slowdown:
        fails.append("the last frames: line was %.0f s before the end" % gap)

    result.update({
        "worst_frame_ms": max([w["worst"] for w in win], default=0),
        "late_per_window": [w["late"] for w in win],
        "busy_windows": [i for i, w in enumerate(win) if w["busy"]],
        "worst_back_us": worst_back, "worst_front_us": worst_front,
        "worst_audio_us": worst_audio,
        "free_kb_min": min(free_kb) if free_kb else 0,
        "free_kb_last": free_kb[-1] if free_kb else 0,
        "windows": len(win),
        "slowdown": round(slowdown, 2),
        "ok": not fails, "failures": fails,
    })
    if fails:
        result["kept"] = keep_evidence("perf-%d-%d" % (seed, run), ms, keyfile, tail)
    json.dump(result, open(os.path.join(RESULTS, "perf-%d-%d.json" % (seed, run)), "w"),
              indent=1)
    return result


# ------------------------------------------------------------------- edge

# The same list as BAD, but a scenario that breaks something on purpose says
# in its `expect` which of these lines it broke, and only those are allowed.
EDGE_BAD = BAD

# What a run may begin with. install_recover() and record_self() both run
# before the shell and both have something to say when the stick was left in
# an odd state, so the first line of an edge run is not always font:.
EDGE_FIRST = ("font:", "ripple:", "recovered:", "self:", "db:")


def edge_invariants(rep, world):
    """What has to hold after any run whatever the scenario did: every
    record of the mock's has the directory it names, every directory of the
    mock's is remembered by a record, and nothing is half-installed."""
    fails = []
    prefix, dir_prefix = world["id_prefix"], world["dir_prefix"]
    claimed = set()
    for app_id, record in rep.records.items():
        if not app_id.startswith(prefix):
            continue
        if "unreadable" in record:
            fails.append("%s is not readable JSON" % app_id)
            continue
        name = record.get("dir", "")
        claimed.add(name)
        if name and name not in rep.dirs:
            fails.append("%s says PSP/GAME/%s, which is not there"
                         % (app_id, name))
    for name in rep.dirs:
        if name.startswith(dir_prefix) and name not in claimed:
            fails.append("PSP/GAME/%s has no record" % name)
    if ".pspdx-stage" in rep.dirs:
        fails.append("PSP/GAME/.pspdx-stage was left behind")
    for name in rep.dirs:
        if name.endswith(".old"):
            fails.append("PSP/GAME/%s was left behind" % name)
    return fails


def edge_catalog_up(ms, began, timeout=120):
    """When the client got as far as its first screenshot, which is the
    moment the key script's clock starts and the moment a timeline counts
    from. PSPDX.BMP is written whether the catalog came or the client gave
    up on it, so the offline scenarios have this too."""
    path = os.path.join(ms, "PSP/PSPDX/DEBUG/PSPDX.BMP")
    while time.time() - began < timeout:
        if os.path.exists(path):
            return time.time()
        time.sleep(0.3)
    return time.time()


def edge_check(sc, result, tail, rep, ms, began_at, ended_at, keys, world):
    fails = []
    lines = rep.lines

    # The soak's runs always start at font:/ripple:, the shell's first two
    # lines. An edge case can start earlier than the shell: install_recover()
    # and record_self() run before it and say what they settled, and those
    # are the two scenarios where that is the point.
    first = tail.first_snapshot or []
    if not first or not first[0].startswith(EDGE_FIRST):
        fails.append("a: the log did not start at one of %r -- the first "
                     "line was %r" % (EDGE_FIRST, first[:1]))
    if not any(t.startswith("font:") for t in lines):
        fails.append("a: no font: line: the shell never came up")
    if len([t for t in lines if t.startswith("font:")]) > 1:
        fails.append("a: the client started more than once in one run; "
                     "another emulator is writing this stick")
    scripted = [t for t in lines if t.startswith("keys: ")]
    if not scripted:
        fails.append("a: the client never loaded a key script")
    elif scripted[0] != "keys: %d scripted" % len(keys):
        fails.append("a: the client loaded %r, not %d keys"
                     % (scripted[0], len(keys)))
    result["log_gaps"] = tail.gaps

    for text in lines:
        for bad in EDGE_BAD:
            if bad in text and not any(ok in text for ok in sc.expect):
                fails.append("b: %r" % text)
                break

    catalog_up = mtime(os.path.join(ms, "PSP/PSPDX/DEBUG/PSPDX.BMP"))
    shot_at = mtime(os.path.join(ms, "PSP/PSPDX/DEBUG/PSPDX1.BMP"))
    if shot_at is None:
        fails.append("e: PSPDX1.BMP was not written; the final shot never "
                     "happened and the loop was not proved alive")
    elif shot_at < began_at:
        fails.append("e: PSPDX1.BMP is older than this run")
    if not rep.has("shot: PSPDX1.BMP"):
        fails.append("e: no 'shot:' line: the script's last key never landed")
    if catalog_up is None:
        fails.append("a: PSPDX.BMP was not written; the client never got as "
                     "far as its first screen")
        catalog_up = began_at

    # The guest against the host, for the liveness window. A scenario whose
    # last key waits behind a long install measures far more than the soak's
    # 1.9 and is clamped rather than failed: what it is used for here is one
    # threshold, not a prediction.
    slowdown = 1.0
    if shot_at and catalog_up and keys and keys[-1][0] > 5000:
        slowdown = max(0.7, min(5.0, (shot_at - catalog_up) /
                                (keys[-1][0] / 1000.0)))
    result["slowdown"] = round(slowdown, 2)

    win = rep.windows
    result["windows"] = len(win)
    if not win:
        fails.append("b: no frames: line at all -- the loop never ran ten "
                     "seconds")
    else:
        gap = ended_at - win[-1]["at"]
        result["silence_s"] = round(gap, 1)
        if gap > max(25.0, 15.0 * slowdown):
            fails.append("b: the last frames: line was %.0f s before the "
                         "emulator was stopped; the loop stopped drawing" % gap)
        if shot_at is not None and win[-1]["at"] < shot_at:
            fails.append("b: nothing was logged after the shot -- the run was "
                         "cut short, give it more tail")

    if sc.invariants:
        fails.extend("c: " + f for f in edge_invariants(rep, world))
    fails.extend("o: " + f for f in sc.oracle(sc.ctx, rep))

    result["worst_frame_ms"] = max([w["worst"] for w in win], default=0)
    result["late_total"] = sum(w["late"] for w in win)
    result["late_windows"] = [i for i, w in enumerate(win)
                              if w["late"] and not w["busy"]]
    return fails


def edge_run(paths, world, sc, lock, stretch=SLOWDOWN):
    """One edge case: plant it, play it, put the desk back, judge it."""
    work, ms, _app = paths
    keys = sc.script
    keyfile = os.path.join(RESULTS, "edge-keys-%s.txt" % sc.name)
    open(keyfile, "w").write(scenarios.render(keys))
    progfile = None
    if sc.driver:
        progfile = os.path.join(RESULTS, "edge-prog-%s.json" % sc.name)
        json.dump(sc.driver, open(progfile, "w"))

    catalog_s = sc.catalog_s or CATALOG_S
    stretch = sc.stretch or stretch
    secs = catalog_s + keys[-1][0] / 1000.0 * stretch + TAIL_S + sc.extra_s
    if sc.driver:
        driver_ms = sum(p.get("ms", 0) + p.get("frames", 0) * 1000 // 60
                        for p in sc.driver)
        secs = max(secs, catalog_s + driver_ms / 1000.0 + TAIL_S)
    for at, _what in sc.timeline:
        secs = max(secs, catalog_s + at + 30)
    secs = int(secs)

    result = {"name": sc.name, "why": sc.why, "keys": len(keys),
              "seconds": secs, "edge": True}
    ctx = edge.Ctx(work, ms, _app, world)
    sc.ctx = ctx
    driver = None
    fired = []
    # A timeline action that is still sleeping when the emulator is stopped
    # must not fire afterwards: it would leave a fault file or a stopped
    # server behind for the next scenario to trip over.
    over = threading.Event()

    def timeline_thread(began):
        """The host's half of the scenario: the server stopped or started
        again, a fault lifted, the key storm begun -- all of it counted from
        the moment the client had its first screen, which is the same moment
        the key script starts."""
        at_catalog = edge_catalog_up(ms, began)
        result["catalog_after_s"] = round(at_catalog - began, 1)
        nonlocal driver
        if progfile:
            driver = subprocess.Popen(["node", HOLD, progfile],
                                      stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT, text=True)
        for at, what in sorted(sc.timeline, key=lambda t: t[0]):
            wait = at_catalog + at - time.time()
            if wait > 0:
                over.wait(wait)
            if over.is_set():
                fired.append("%s skipped: the run was already over" % at)
                continue
            try:
                what(ctx)
                fired.append(at)
            except Exception as exc:                     # noqa: BLE001
                fired.append("%s failed: %s" % (at, exc))

    with lock:
        wait_idle()
        # mock-catalog asks the published catalog where the assets are on
        # every plant, so one DNS hiccup on this desk would otherwise end a
        # campaign an hour in. It is a hiccup: wait and ask again.
        for attempt in range(3):
            planted = subprocess.run(["sh", RIG, "plant"],
                                     stdout=subprocess.DEVNULL)
            if planted.returncode == 0:
                break
            print("        %s: the plant failed; the host's network, most "
                  "likely -- waiting" % sc.name)
            time.sleep(20)
        else:
            raise SystemExit("edge: three plants in a row failed")
        edge.faults(ctx, [])            # whatever the last scenario broke
        for step in sc.plant:
            step(ctx)
        for name in ("PSPDX.LOG", "PSPDX.BMP", "PSPDX1.BMP", "PSPDX2.BMP"):
            try:
                os.remove(os.path.join(ms, "PSP/PSPDX/LOGS/pspdx.log" if name == "PSPDX.LOG" else "PSP/PSPDX/DEBUG/" + name))
            except OSError:
                pass
        tail = LogTail(os.path.join(ms, "PSP/PSPDX/LOGS/pspdx.log"))
        began_at = time.time()
        proc = run_rig(secs, keyfile, sc.extra_args)
        tail.start()
        clock = threading.Thread(target=timeline_thread, args=(began_at,),
                                 daemon=True)
        clock.start()
        proc.wait()
        ended_at = time.time()
        over.set()
        clock.join(timeout=5)
        if driver:
            try:
                driver.wait(timeout=10)
            except subprocess.TimeoutExpired:
                driver.kill()
            out = (driver.stdout.read() or "").strip().splitlines()
            # A refusal comes back as an "error" event; a debugger that was
            # never reached at all says so and exits non-zero, and that is
            # the storm not having happened rather than the client passing.
            result["driver"] = [l for l in out if "error" in l.lower()][:4]
            if driver.returncode not in (0, None) and not result["driver"]:
                result["driver"] = ["hold.mjs exited %d: %s"
                                    % (driver.returncode, out[-1] if out else "")]
        tail.finish()
        records, dirs = edge.snapshot(ms)
        win = windows(tail.lines)
        free_kb = [int(m.group(3)) for m in
                   (OUTSIDE_RE.search(t) for _a, t in tail.lines) if m]
        rep = edge.Report([t for _a, t in tail.lines], records, dirs, ms,
                          win, free_kb)
        result["timeline"] = fired
        # The two numbers the long-idle oracle is about, kept for every run:
        # a leak shows up as the second being smaller than the first, and it
        # is worth having the figures on a run that passed as well.
        result["free_kb_first"] = free_kb[0] if free_kb else 0
        result["free_kb_last"] = free_kb[-1] if free_kb else 0
        fails = edge_check(sc, result, tail, rep, ms, began_at, ended_at,
                           keys, world)
        # The desk back as it was found, whatever the verdict: the records
        # and directories this scenario planted outside the mock's prefixes
        # are the user's own two directories.
        ctx.restore()
        if fails:
            result["kept"] = keep_evidence("edge-" + sc.name, ms, keyfile, tail)
        else:
            os.remove(keyfile)

    if result.get("driver"):
        fails.append("b: the debugger refused something: %s" % result["driver"][0])
    result["ok"] = not fails
    result["failures"] = fails
    json.dump(result, open(os.path.join(RESULTS, "edge-%s.json" % sc.name), "w"),
              indent=1)
    return result


# ------------------------------------------------------------------- main

class Lock:
    def __init__(self, path):
        self.fh = open(path, "w")

    def __enter__(self):
        fcntl.flock(self.fh, fcntl.LOCK_EX)
        return self

    def __exit__(self, *exc):
        fcntl.flock(self.fh, fcntl.LOCK_UN)


def paths():
    out = subprocess.run(["sh", RIG, "paths"], check=True, capture_output=True,
                         text=True).stdout.split()
    return out[0], out[1], out[2]


def edge_campaign(paths_, world, lock, args):
    """The thirty edge cases, in order, with a table at the end. Each one
    plants its own stick and its own site, so a scenario that fails does not
    poison the next; the rebuild for the published catalog still happens
    whatever goes wrong."""
    all_scenarios = edge.build_all(world)
    if args.only:
        all_scenarios = [s for s in all_scenarios if args.only in s.name]
    chosen = all_scenarios[args.start - 1:args.start - 1 + args.edge]
    print("edge: %d scenarios, %d apps, %d installed to start"
          % (len(chosen), len(world["apps"]), len(world["db"])))

    results = []
    try:
        for i, sc in enumerate(chosen, args.start):
            began = time.time()
            stretch = SLOWDOWN
            for _attempt in range(2):
                r = edge_run(paths_, world, sc, lock, stretch)
                again = [f for f in r["failures"]
                         if f.startswith("a:") or "cut short" in f]
                if r["ok"] or len(again) != len(r["failures"]):
                    break
                if any("cut short" in f for f in again):
                    stretch = max(stretch, r.get("slowdown", stretch)) * 1.3
                print("        %s: %s -- trying again" % (sc.name, again[0]))
            r["wall_s"] = round(time.time() - began, 1)
            results.append(r)
            print("%3d  %-32s %3d keys %4ds  worst %4d ms  late %2d  %s"
                  % (i, sc.name, r["keys"], r["seconds"], r["worst_frame_ms"],
                     r["late_total"], "ok" if r["ok"] else "FAIL"))
            for line in r["failures"]:
                print("        %s" % line)
    finally:
        with lock:
            subprocess.run(["sh", RIG, "stop"])
            subprocess.run(["sh", RIG, "clean"])
            if not args.keep_real:
                subprocess.run(["sh", RIG, "build-real"])

    bad = [r for r in results if not r["ok"]]
    print("\n%-34s %6s %6s %6s %5s  %s"
          % ("scenario", "keys", "wall", "worst", "late", "verdict"))
    for r in results:
        print("%-34s %6d %5.0fs %5dms %5d  %s"
              % (r["name"], r["keys"], r["wall_s"], r["worst_frame_ms"],
                 r["late_total"], "ok" if r["ok"] else "FAIL"))
    print("\n%d scenarios, %d failed, %.0f s of emulator"
          % (len(results), len(bad), sum(r["wall_s"] for r in results)))
    for r in bad:
        print("  %s: %s" % (r["name"], r["failures"][0]))
        print("         kept under %s" % r.get("kept", "-"))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--runs", type=int, default=10)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--perf", type=int, metavar="N",
                    help="N stress runs instead, through the debugger")
    ap.add_argument("--edge", type=int, metavar="N", nargs="?", const=30,
                    help="the first N of the thirty edge cases instead")
    ap.add_argument("--only", metavar="TEXT",
                    help="with --edge: only the scenarios whose name has "
                         "TEXT in it")
    ap.add_argument("--from", dest="start", type=int, default=1)
    ap.add_argument("--no-build", action="store_true",
                    help="the EBOOT on the stick is already the mock build")
    ap.add_argument("--keep-real", action="store_true",
                    help="skip the rebuild for the published catalog at the end")
    args = ap.parse_args()

    os.makedirs(RESULTS, exist_ok=True)
    lock = Lock(LOCK_PATH)
    with lock:
        subprocess.run(["sh", RIG, "plant" if args.no_build else "setup"],
                       check=True)
        keep_build()
        subprocess.run(["sh", RIG, "serve"], check=True)
    time.sleep(1)

    work, ms, _app = paths()
    world = model.load_world(os.path.join(work, "mock-site"))
    if args.edge:
        return edge_campaign((work, ms, _app), world, lock, args)
    total = args.perf if args.perf else args.runs
    kind = "perf" if args.perf else "soak"
    print("%s: %d runs, seed %d, %d apps, %d installed to start"
          % (kind, total, args.seed, len(world["apps"]), len(world["db"])))

    results = []
    try:
        for run in range(args.start, args.start + total):
            began = time.time()
            if args.perf:
                r = perf_run((work, ms, _app), world, args.seed, run, lock)
                print("%4d  %-6s %3ds  worst %4d ms  late %s  back %d us  "
                      "front %d us  audio %d us  free %d KB  %s"
                      % (run, "perf", r["seconds"], r["worst_frame_ms"],
                         "".join(str(min(n, 9)) for n in r["late_per_window"]),
                         r["worst_back_us"], r["worst_front_us"],
                         r["worst_audio_us"], r["free_kb_last"],
                         "ok" if r["ok"] else "FAIL"))
            else:
                # A run whose only complaint is that the log was not its own
                # was not a run: somebody else's emulator had the stick. That
                # is worth trying again rather than reporting as a client bug.
                stretch = SLOWDOWN
                for attempt in range(3):
                    r = one_run((work, ms, _app), world, args.seed, run, lock,
                                stretch)
                    again = [f for f in r["failures"]
                             if f.startswith("a:") or "cut short" in f]
                    if r["ok"] or len(again) != len(r["failures"]):
                        break
                    if any("cut short" in f for f in again):
                        # The emulator was slower than the estimate. It is
                        # measured now, so the next attempt is right.
                        stretch = max(stretch, r.get("slowdown", stretch)) * 1.3
                    print("        run %d: %s -- trying again" % (run, again[0]))
                print("%4d  %-28s %3d keys %3ds  %2d inst %2d rm  worst %4d ms"
                      "  late %2d  %s"
                      % (run, "+".join(r["profiles"]), r["keys"], r["seconds"],
                         r["installs"], r["removes"], r["worst_frame_ms"],
                         r["late_total"], "ok" if r["ok"] else "FAIL"))
            for line in r["failures"]:
                print("        %s" % line)
            r["wall_s"] = round(time.time() - began, 1)
            results.append(r)
    finally:
        with lock:
            subprocess.run(["sh", RIG, "stop"])
            subprocess.run(["sh", RIG, "clean"])
            if not args.keep_real:
                subprocess.run(["sh", RIG, "build-real"])

    bad = [r for r in results if not r["ok"]]
    print("\n%d runs, %d failed, %.0f s of emulator"
          % (len(results), len(bad), sum(r["wall_s"] for r in results)))
    for r in bad:
        print("  %d-%d: %s" % (r["seed"], r["run"], r["failures"][0]))
        print("         kept under %s" % r.get("kept", "-"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
