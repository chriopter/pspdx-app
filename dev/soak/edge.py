#!/usr/bin/env python3
"""Thirty edge cases, each with its own oracle.

The soak (scenarios.py) is a hundred people with errands, and it says what
the client does when nothing goes wrong. This file is the other half: the
thirty ways it can go wrong that anybody could think of, written down one at
a time -- a thumb on the pad for twenty seconds, thirty packages at once, a
server that answers 404 or stops answering at all, an archive with two
EBOOTs or an entry called "../../evil", a catalog with seventy apps in it,
and a stick that was left halfway through an install.

A scenario is a function returning a Scenario:

    name        what it is called, and what results/edge-<name>.json is named
    why         the sentence that says what it is for
    plant       what to put on the stick and on the mock site before boot
    script      PSPDX.KEYS, in the guest's milliseconds
    driver      hold.mjs's program, in the host's, for what a key file cannot
                do: four hundred presses in twenty seconds
    timeline    (host seconds after the catalog is up, what to do) -- the
                server stopped mid-run, a fault turned off again
    extra_args  dev/rig's, for --slow
    extra_s     emulator seconds past the end of the key script, for the
                installs that take minutes
    oracle      what the log must say and the stick must look like
    expect      lines that are allowed to appear even though they say
                "failed", "refusing" or "MISMATCH" -- the scenarios that
                break something on purpose name what they broke

Every scenario also gets, from run.py and without asking: the log is this
run's, the loop was still drawing at the end, the script's last two keys --
a `down` and a `shot` -- reached it, PSPDX1.BMP is there, and unless the
scenario says otherwise the stick is whole: every record has its directory,
every directory has its record, no staging tree and no .old left behind.

    python3 dev/soak/run.py --edge 30
    python3 dev/soak/run.py --edge 1 --only zip-2000-files
"""

import hashlib
import io
import json
from datetime import datetime, timezone
import os
import random
import shutil
import struct
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import model                                            # noqa: E402
import scenarios                                        # noqa: E402
from model import (TAB_GEAR, TAB_STICK, TAB_BASKET, NOT_INSTALLED,  # noqa: E402
                   CURRENT, UPDATE, INSTALL_MS, REMOVE_MS)

# shell.c's tab values: 0 is All, 1 to 5 the categories, and the three that
# are not categories are negative. model.TAB_ALL is the *count* of category
# tabs and not one of them, which is worth naming once here rather than
# tripping over.
TAB_EVERYTHING = 0

GAP_NAV = scenarios.GAP_NAV
GAP_ACT = scenarios.GAP_ACT
# Longer than the soak's: a refetch that runs past the wait puts the next
# key inside the window, where the client ORs it with the ones after it and
# a script stops meaning what it says. The key after the wait is a circle
# the planner adds on its own, so a refetch that was *quick* and let the
# shell fade costs nothing either.
REFRESH_MS = 12000

# keys_load() reads 4 KB and keeps 256 lines. A script that goes past either
# is silently cut in half, which would look like a client bug.
MAX_KEYS = 250
MAX_BYTES = 4000


# ------------------------------------------------------------------ the desk

class Ctx:
    """Where everything is, and what this scenario changed so it can be put
    back. The stick is shared with the desk's own emulator and with the
    user's real installs: anything touched outside the mock's own prefixes is
    saved first and restored afterwards."""

    def __init__(self, work, ms, app, world):
        self.work = work
        self.ms = ms
        self.app = app
        self.world = world
        self.site = os.path.join(work, "mock-site")
        self.saved = {}                 # path -> bytes, or None if it was absent
        self.made_dirs = []             # directories this scenario created
        self.notes = []                 # what the oracle wants to know later

    # ------------------------------------------------------------ paths
    def db_path(self, app_id):
        return os.path.join(self.ms, "PSP/PSPDX/db", app_id + ".json")

    def game_path(self, name):
        return os.path.join(self.ms, "PSP/GAME", name)

    # ------------------------------------------------------------ writes
    def keep(self, path):
        if path in self.saved:
            return
        try:
            with open(path, "rb") as fh:
                self.saved[path] = fh.read()
        except OSError:
            self.saved[path] = None

    def write(self, path, data):
        self.keep(path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(data if isinstance(data, bytes) else data.encode())

    def remove(self, path):
        self.keep(path)
        try:
            os.remove(path)
        except OSError:
            pass

    def plant_dir(self, name, files):
        """A directory under PSP/GAME with files in it, remembered so the
        scenario can take it away again."""
        base = self.game_path(name)
        self.made_dirs.append(base)
        for rel, data in files.items():
            path = os.path.join(base, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as fh:
                fh.write(data)
        return base

    def restore(self):
        """What this scenario wrote outside the mock's own prefixes, put
        back. The mock's records and directories are not restored here and
        do not need to be: `rig.sh plant` makes the site and the planted
        state again from nothing before the next scenario."""
        for path, data in self.saved.items():
            if data is None:
                try:
                    os.remove(path)
                except OSError:
                    pass
            else:
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "wb") as fh:
                    fh.write(data)
        self.saved = {}
        for path in self.made_dirs:
            shutil.rmtree(path, ignore_errors=True)
        self.made_dirs = []
        faults(self, [])


# --------------------------------------------------------------- the site

def catalog_read(ctx):
    with open(os.path.join(ctx.site, "catalog.json")) as fh:
        return json.load(fh)


def catalog_write(ctx, cat):
    with open(os.path.join(ctx.site, "catalog.json"), "w") as fh:
        json.dump(cat, fh)


def entry_of(cat, app_id):
    for entry in cat["apps"]:
        if entry["id"] == app_id:
            return entry
    raise KeyError(app_id)


def set_package(ctx, app_id, blob):
    """The bytes served as this package, and the catalog told what they
    hash to: a download that fails its own checksum is a scenario of its
    own and not an accident of this helper."""
    path = os.path.join(ctx.site, "pkgs", "%s.zip" % app_id)
    with open(path, "wb") as fh:
        fh.write(blob)
    cat = catalog_read(ctx)
    release = entry_of(cat, app_id)["release"]
    release["download"]["sha256"] = hashlib.sha256(blob).hexdigest()
    release["download"]["size"] = len(blob)
    catalog_write(ctx, cat)


def faults(ctx, rules):
    """What the server should break, or nothing. Read per request, so this
    works from a timeline while the emulator is running."""
    path = os.path.join(ctx.work, "faults.json")
    if not rules:
        try:
            os.remove(path)
        except OSError:
            pass
        return
    with open(path, "w") as fh:
        json.dump({"rules": rules}, fh)


def pkg_path(app_id):
    return "/pkgs/%s.zip" % app_id


def build_version(ctx):
    """What this EBOOT calls itself, out of the header the Makefile
    generates. The container build sees no .git and falls back to "dev"; the
    point is that the self record says whatever this is and not what was on
    the stick before it."""
    path = os.path.join(ctx.app, "util", "version.h")
    try:
        with open(path) as fh:
            text = fh.read()
    except OSError:
        return "dev"
    start = text.find('"')
    end = text.find('"', start + 1)
    return text[start + 1:end] if start >= 0 and end > start else "dev"


def eboot_bytes(ctx):
    """A real EBOOT out of the mock's asset cache, so a package that installs
    leaves something a PSP would start. Falls back to a stub: nothing in
    these scenarios executes it."""
    path = os.path.join(ctx.work, "mock-assets", "EBOOT.PBP")
    try:
        with open(path, "rb") as fh:
            return fh.read()
    except OSError:
        return b"\0PBP" + b"stub" * 64


# ---------------------------------------------------------------- archives

def zip_blob(entries, method=zipfile.ZIP_DEFLATED):
    """A zip of (name, bytes), with the names written exactly as given --
    zipfile's own sanitising would take the "../.." out of the one entry
    that is the point."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", method) as w:
        for name, data in entries:
            info = zipfile.ZipInfo(name)
            info.compress_type = method
            info.external_attr = (0o755 if name.endswith("/") else 0o644) << 16
            w.writestr(info, data)
    return buf.getvalue()


def patch_central_usize(blob, name, value):
    """One entry's uncompressed size, in the central directory only. The
    local header keeps the truth, so the two disagree -- which is what a
    zip written by something hostile looks like, and what zipread.c has to
    survive."""
    want = name.encode()
    out = bytearray(blob)
    i = 0
    while True:
        i = out.find(b"PK\x01\x02", i)
        if i < 0:
            raise ValueError("no central record for %s" % name)
        nlen = struct.unpack_from("<H", out, i + 28)[0]
        if bytes(out[i + 46:i + 46 + nlen]) == want:
            struct.pack_into("<I", out, i + 24, value)
            return bytes(out)
        i += 4


# ------------------------------------------------------------------ hands

class Hand(scenarios.Planner):
    """The soak's script writer without its budget: an edge case is allowed
    to be four minutes long and two hundred keys."""

    def room(self, keys=1, ms=0):
        return True


def hand(world, seed=11):
    return Hand(world, random.Random(seed))


def done(p):
    """Every script ends the same way: one more step down the list, which
    only a loop that is still reading the pad can take, and a settled
    photograph of where it ended up."""
    p.wait(1500)
    p.key("down", GAP_ACT)
    p.key("shot", GAP_ACT)
    p.sim.finish()
    return p


def install_here(p):
    """The entry under the cursor fetched, by whichever keys its state
    wants: X and yes, or the menu's Reinstall for one already current. The
    planner reads the state off the model, because X twice on a current row
    is X on Run."""
    if not p.install_here():
        raise SystemExit("edge: nothing to install under the cursor at %d" % p.t)


def install_app(p, index):
    if not p.goto_app(index):
        raise SystemExit("edge: cannot reach app %d" % index)
    install_here(p)


def remove_app(p, index):
    if not p.goto_app(index):
        raise SystemExit("edge: cannot reach app %d" % index)
    if not p.remove_here():
        raise SystemExit("edge: app %d cannot be removed" % index)


def refresh(p):
    """Over to the gear tab and "Update catalog"."""
    if not p.refresh_here(REFRESH_MS):
        raise SystemExit("edge: no gear tab to refresh from")


def browse(p, steps, gap=GAP_NAV):
    for _ in range(steps):
        p.key("down", gap)


# --------------------------------------------------------------- the world

def apps_where(world, state=None, category=None, has_release=True, skip=()):
    out = []
    for a in world["apps"]:
        if state and a.state != state:
            continue
        if category and a.category != category:
            continue
        if has_release and not a.has_release:
            continue
        if a.index in skip:
            continue
        out.append(a.index)
    return out


def pick(world, state, n=1, skip=()):
    """The same entries every time: a scenario that picked at random would
    not be the same scenario twice."""
    got = apps_where(world, state=state, skip=skip)
    if len(got) < n:
        raise SystemExit("edge: the mock catalog has no %d %s apps" % (n, state))
    return got[:n] if n > 1 else got[0]


def synth_app(index, suffix, name, category, rev, version, size=4096,
              dirname=None, state=NOT_INSTALLED):
    """A catalog entry that is not one of the mock's thirty, as both halves:
    the JSON the server will serve and the App the script writer navigates
    by. The two are written side by side here so they cannot drift."""
    app_id = "dev.pspdx.mock.%s" % suffix
    dirname = dirname or ("PSPDXMock%s" % suffix.replace(".", ""))
    app = model.App(index, app_id, name, category, rev, version, size, dirname)
    app.state = state
    return app


def entry_json(app, url_base, package=True):
    entry = {
        "id": app.id, "name": app.name, "author": "edge",
        "summary": "an edge case", "category": app.category,
        "license": "MIT",
    }
    if package:
        entry["release"] = {
            "published_at": datetime.fromtimestamp(app.rev, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "tag": "v" + app.version,
            "download": {"url": url_base + "pkgs/%s.zip" % app.id,
                         "sha256": "00" * 32, "size": app.size},
        }
    return entry


def url_base(ctx):
    return catalog_read(ctx)["apps"][0]["release"]["download"]["url"].rsplit("pkgs/", 1)[0]


def extend_world(world, extra):
    """The mock's thirty with more entries after them, in the order the
    client will keep them."""
    out = dict(world)
    out["apps"] = list(world["apps"]) + list(extra)
    return out


# ------------------------------------------------------------- the oracles

class Report:
    """What one run left behind, for an oracle to read."""

    def __init__(self, lines, records, dirs, ms, windows, free_kb):
        self.lines = lines
        self.text = "\n".join(lines)
        self.records = records          # every record under PSP/PSPDX/db
        self.dirs = dirs                # every name under PSP/GAME
        self.ms = ms
        self.windows = windows
        self.free_kb = free_kb

    def has(self, needle):
        return any(needle in line for line in self.lines)

    def count(self, needle):
        return sum(1 for line in self.lines if needle in line)

    def record(self, app_id):
        return self.records.get(app_id)

    def path(self, *rel):
        return os.path.join(self.ms, *rel)

    def exists(self, *rel):
        return os.path.exists(self.path(*rel))


def snapshot(ms):
    """Every record and every PSP/GAME directory, the mock's and everybody
    else's: an edge case plants records the mock's prefixes do not cover."""
    db_dir = os.path.join(ms, "PSP/PSPDX/db")
    game_dir = os.path.join(ms, "PSP/GAME")
    records = {}
    for name in sorted(os.listdir(db_dir)) if os.path.isdir(db_dir) else []:
        if not name.endswith(".json"):
            continue
        try:
            with open(os.path.join(db_dir, name)) as fh:
                records[name[:-5]] = json.load(fh)
        except (OSError, ValueError) as exc:
            records[name[:-5]] = {"unreadable": str(exc)}
    dirs = sorted(os.listdir(game_dir)) if os.path.isdir(game_dir) else []
    return records, dirs


def want_record(app, dirname=None, rev=None, version=None):
    return {"id": app.id, "rev": rev if rev is not None else app.rev,
            "dir": dirname or app.dir, "manifest": "",
            "version": version if version is not None else app.version}


def check_record(rep, app, fails, **kw):
    want = want_record(app, **kw)
    got = rep.record(app.id)
    if not got:
        fails.append("no record for %s" % app.id)
        return
    for field in ("id", "rev", "dir", "manifest", "version"):
        if got.get(field) != want[field]:
            fails.append("%s: %s is %r, expected %r"
                         % (app.id, field, got.get(field), want[field]))
    if want["dir"] not in rep.dirs:
        fails.append("%s: PSP/GAME/%s is not there" % (app.id, want["dir"]))


def check_absent(rep, app, fails):
    if rep.record(app.id):
        fails.append("%s has a record and should not" % app.id)


def model_oracle(world, script):
    """The plain one: the stick is what model.py says this script leaves.
    Used where the scenario is predictable -- the bulk runs and the basket."""
    pred = model.simulate(world, script)

    def oracle(ctx, rep):
        fails = []
        prefix = world["id_prefix"]
        records = {k: v for k, v in rep.records.items() if k.startswith(prefix)}
        dirs = [d for d in rep.dirs if d.startswith(world["dir_prefix"])]
        import run                                           # noqa: F401
        return fails + run.compare_state(pred, records, dirs)
    return oracle, pred


# ------------------------------------------------------------- a scenario

class Scenario:
    def __init__(self, name, why, script, oracle, plant=(), driver=None,
                 timeline=(), extra_args=(), extra_s=0, expect=(),
                 invariants=True, stretch=None, catalog_s=None):
        self.name = name
        self.why = why
        self.script = script
        self.oracle = oracle
        self.plant = list(plant)
        self.driver = driver
        self.timeline = list(timeline)
        self.extra_args = list(extra_args)
        self.extra_s = extra_s
        self.expect = list(expect)
        self.invariants = invariants
        self.stretch = stretch
        self.catalog_s = catalog_s
        text = scenarios.render(script)
        if len(script) > MAX_KEYS or len(text) > MAX_BYTES:
            raise SystemExit("edge: %s writes %d keys / %d bytes; keys_load() "
                             "keeps 256 lines of 4 KB"
                             % (name, len(script), len(text)))


# =========================================================== 1-5  storms

# What a scenario that breaks the network is allowed to say. The check is
# per line and looks for any of these in it, so none of them may be a word as
# broad as "failed" on its own: that would switch the check off.
NET_NOISE = ["Install failed", "connect failed", "handshake failed",
             "dns failed", "read error", "catalog unreachable", "cannot ",
             "png: rc=", "mp4: rc="]

# And a scenario that hands the unpacker something that is not a package.
UNPACK_NOISE = ["Install failed", "unpack: refusing", "unpack: failed",
                "unpack: cannot open archive", "no EBOOT.PBP",
                "two EBOOT.PBP", "zip: "]


# Every button that cannot, in any sequence, hand the console to another
# EBOOT or the pad to the firmware keyboard. START is the launch key. Cross
# and triangle are out with it: on an installed row either one opens the
# options menu with the cursor on Run, and the next cross is Run -- and
# from the gear tab, which left and right walk onto, cross on the second or
# third band row opens the keyboard, which reads the pad itself. Circle,
# square, SELECT and the four directions can open and close things, fill
# the basket and walk every tab, and none of that leaves the loop.
STORM_BUTTONS = ("circle", "square", "select", "left", "right", "up", "down")


def storm_mixed(world):
    """Twenty seconds of a thumb going at fifty milliseconds, every button
    that cannot end the run on its own or with the one after it.

    START, cross and triangle are left out on purpose and not by oversight:
    see STORM_BUTTONS. What a storm of X can do to the menu and the
    question is covered by storm-bands and storm-confirm-band instead."""
    rng = random.Random(0x5701)
    program = [{"op": "wait", "ms": 500}]
    storm_ms = 0
    while storm_ms < 20000:
        gap = rng.randint(40, 60)
        program.append({"op": "tap", "button": rng.choice(STORM_BUTTONS),
                        "ms": gap})
        storm_ms += gap
    p = hand(world)
    # The storm is timed in the host's clock and the script in the guest's;
    # a guest at its slowest still gets to the shot after the storm is over.
    p.wait(int(storm_ms / 1.2))
    done(p)

    def oracle(ctx, rep):
        fails = []
        if not rep.has("shot: PSPDX1.BMP"):
            fails.append("the shot after the storm was never taken")
        return fails
    return Scenario(
        "storm-mixed-keys",
        "twenty seconds of seven buttons at 40-60 ms, through the debugger",
        # Without cross nothing in the storm fetches, but square fills the
        # basket and the storm can leave the shell anywhere; the tail is
        # kept generous so the shot is judged on the shell and not on the
        # clock.
        p.script, oracle, driver=program, extra_s=90)


def storm_confirm(world):
    """The confirm band opened and cancelled thirty times. Nothing may be
    installed by that, and nothing may be left half-installed."""
    index = pick(world, NOT_INSTALLED)
    p = hand(world)
    if not p.goto_app(index):
        raise SystemExit("edge: cannot reach the row")
    for _ in range(30):
        p.key("cross", 220)             # "Install X?"
        p.key("circle", 220)            # and away again
    done(p)
    oracle, _pred = model_oracle(world, model.parse_script(scenarios.render(p.script)))
    return Scenario(
        "storm-confirm-band",
        "open and cancel the install question thirty times",
        p.script, oracle)


def storm_bands(world):
    """The options menu and the info band, opened and closed thirty times
    each. The menu is triangle on something installed and circle shuts it;
    the band is the gear tab, one step left of the stick tab, and circle
    steps off it onto the stick tab again. Neither may fetch anything, and
    the catalog may not be asked for twice."""
    index = pick(world, CURRENT)
    p = hand(world)
    if not p.goto_app(index):
        raise SystemExit("edge: cannot reach the row")
    for _ in range(30):
        p.key("triangle", 220)          # Run / Reinstall / Delete / ...
        p.key("circle", 220)
    for _ in range(30):
        if not p.goto_tab(TAB_GEAR):    # the info band
            raise SystemExit("edge: no gear tab")
        if not p.sim.info:
            raise SystemExit("edge: the gear tab did not open the band")
        p.key("circle", 220)            # and off it again
    done(p)
    script = model.parse_script(scenarios.render(p.script))
    base, _pred = model_oracle(world, script)

    def oracle(ctx, rep):
        fails = base(ctx, rep)
        n = rep.count("30 apps, 30 usable")
        if n != 1:
            fails.append("the catalog was parsed %d times; hammering the "
                         "bands fetched something" % n)
        return fails
    return Scenario(
        "storm-bands",
        "the options menu and the info band, thirty open-and-close each",
        p.script, oracle)


def storm_refresh(world):
    """The gear tab, "Update catalog", five times in a row."""
    p = hand(world)
    browse(p, 3)
    for _ in range(5):
        refresh(p)
    browse(p, 4)
    done(p)

    def oracle(ctx, rep):
        fails = []
        n = rep.count("30 apps, 30 usable")
        if n != 6:
            fails.append("the catalog was parsed %d times, expected 6 "
                         "(the first and five refetches)" % n)
        if rep.count("net up") < 1:
            fails.append("no 'net up' in the log at all")
        return fails
    return Scenario(
        "storm-refresh-five",
        "five catalog refetches back to back",
        p.script, oracle)


def storm_refresh_basket(world):
    """A refetch with five packages in the basket. view_rebuild()
    drops the basket whole, because the indices in it point into an array
    the fetch has just rewritten -- so the basket tab goes with it.

    The assertion is not "the basket looks empty", which is a thing on
    screen. Taking "Update catalog" steps off the gear tab onto the stick
    tab, and the refetch lands there; one step right from it is then All
    if the basket tab is gone and the basket tab if it is not. A walk down
    All to a package the model chose, and X twice, install that one
    package; on the basket tab the same presses land on one of the five
    set aside, or on the action row and all five, and the stick says
    which. Nothing on the basket tab is installed, so no press there can
    be Run."""
    want = pick(world, NOT_INSTALLED, 5)
    p = hand(world)
    for index in want:
        if not p.goto_app(index, prefer_category=True):
            raise SystemExit("edge: cannot reach a basket row")
        if not p.basket_here():
            raise SystemExit("edge: square did not fill the basket")
    if not p.goto_tab(TAB_BASKET) or not p.goto_row(1):
        raise SystemExit("edge: no basket tab")
    refresh(p)
    if p.sim.tab_kind() != TAB_STICK:
        raise SystemExit("edge: the refetch did not land on the stick tab")
    p.key("right", GAP_NAV)
    if p.sim.tab_kind() != TAB_EVERYTHING:
        raise SystemExit("edge: one right of the stick tab is not All")
    target = pick(world, NOT_INSTALLED, skip=tuple(want))
    if not p.goto_row(p.sim.view_row(target)):
        raise SystemExit("edge: cannot reach the row after the refetch")
    install_here(p)
    done(p)
    oracle, pred = model_oracle(world, model.parse_script(scenarios.render(p.script)))
    if pred["installs"] != 1:
        raise SystemExit("edge: the basket scenario should install exactly one")
    return Scenario(
        "storm-refresh-basket",
        "a refetch with five in the basket: the basket and its tab go",
        p.script, oracle)


# ============================================================ 6-9  bulk

def bulk_basket_all(world):
    """All thirty set aside and fetched in one press."""
    p = hand(world)
    p.goto_tab(TAB_EVERYTHING)
    for row in range(len(world["apps"])):
        p.goto_row(row)
        p.basket_here()
    if not p.goto_tab(TAB_BASKET) or not p.goto_row(0):
        raise SystemExit("edge: no basket tab")
    install_here(p)                     # "Install 30 apps?"
    done(p)
    base, _pred = model_oracle(world, model.parse_script(scenarios.render(p.script)))

    def oracle(ctx, rep):
        fails = base(ctx, rep)
        if not rep.has("30 of 30 installed"):
            fails.append("the log never said '30 of 30 installed'")
        return fails
    return Scenario(
        "bulk-basket-thirty",
        "thirty packages in the basket and Download all",
        p.script, oracle, extra_s=30)


def bulk_update_all(world):
    """The stick tab and its action row: every update waiting, at once. The
    row is only there while something waits, and it takes the updates
    alone -- what is merely installed stays as it is."""
    waiting = len(apps_where(world, state=UPDATE))
    p = hand(world)
    if not p.goto_tab(TAB_STICK) or not p.goto_row(0):
        raise SystemExit("edge: no stick tab")
    if p.sim.view_index(0) != model.ROW_ACTION:
        raise SystemExit("edge: no Update all row on the stick tab")
    install_here(p)                     # "Update N apps?"
    browse(p, 4)
    done(p)
    base, _pred = model_oracle(world, model.parse_script(scenarios.render(p.script)))

    def oracle(ctx, rep):
        fails = base(ctx, rep)
        if not rep.has("%d of %d installed" % (waiting, waiting)):
            fails.append("the log never said '%d of %d installed'"
                         % (waiting, waiting))
        return fails
    return Scenario(
        "bulk-update-all",
        "every update waiting, taken in one press",
        p.script, oracle, extra_s=20)


def bulk_remove_all(world):
    """Every installed package off the stick, one at a time, down the All
    tab: triangle, down to Delete, X, yes, twenty times."""
    p = hand(world)
    p.goto_tab(TAB_EVERYTHING)
    for row, app in enumerate(world["apps"]):
        if app.state == NOT_INSTALLED:
            continue
        p.goto_row(row)
        if not p.remove_here():
            raise SystemExit("edge: row %d cannot be removed" % row)
    done(p)
    base, _pred = model_oracle(world, model.parse_script(scenarios.render(p.script)))
    installed = len([a for a in world["apps"] if a.state != NOT_INSTALLED])

    def oracle(ctx, rep):
        fails = base(ctx, rep)
        left = [d for d in rep.dirs if d.startswith(world["dir_prefix"])]
        if left:
            fails.append("PSP/GAME still holds %d of the mock's directories: %s"
                         % (len(left), ", ".join(left[:4])))
        if rep.count("uninstalled ") < 1:
            fails.append("no 'uninstalled' line in the log at all")
        return fails
    if installed != 20:
        raise SystemExit("edge: expected 20 installed to start, found %d" % installed)
    return Scenario(
        "bulk-remove-twenty",
        "all twenty installed packages removed one by one",
        p.script, oracle, extra_s=15)


def bulk_cycle(world):
    """The first fifteen installed, removed, and installed again: the long
    one, and the only scenario that sees a package installed over its own
    remains twice. Fifteen and not thirty because Delete lives in the menu
    now and a removal is five keys, and thirty of everything would be three
    hundred lines against keys_load()'s 256."""
    p = hand(world)
    n = 15

    def basket_everything():
        p.goto_tab(TAB_EVERYTHING)
        for row in range(n):
            p.goto_row(row)
            p.basket_here()
        if not p.goto_tab(TAB_BASKET) or not p.goto_row(0):
            raise SystemExit("edge: no basket tab")
        install_here(p)                 # "Install 15 apps?"

    basket_everything()
    p.goto_tab(TAB_EVERYTHING)
    for row in range(n):
        p.goto_row(row)
        if not p.remove_here():
            raise SystemExit("edge: row %d cannot be removed" % row)
    basket_everything()
    done(p)
    base, _pred = model_oracle(world, model.parse_script(scenarios.render(p.script)))
    said = "%d of %d installed" % (n, n)

    def oracle(ctx, rep):
        fails = base(ctx, rep)
        if rep.count(said) != 2:
            fails.append("expected two '%s' lines, saw %d"
                         % (said, rep.count(said)))
        return fails
    return Scenario(
        "bulk-install-remove-install",
        "fifteen installed, fifteen removed, fifteen installed again",
        p.script, oracle, extra_s=60)


# ======================================================= 10-17  network

def net_server_killed(world):
    """The server stopped while the client is browsing: the install that
    follows must fail cleanly and change nothing, and the one after the
    server comes back must work."""
    a, b = pick(world, NOT_INSTALLED, 2)
    app_a, app_b = world["apps"][a], world["apps"][b]
    p = hand(world)
    p.wait(2000)
    install_app(p, a)                   # with nothing listening
    p.wait(14000)                       # the server comes back in here
    install_app(p, b)
    done(p)

    def oracle(ctx, rep):
        fails = []
        if not rep.has("Install failed"):
            fails.append("the install with no server did not report a failure")
        check_absent(rep, app_a, fails)
        check_record(rep, app_b, fails)
        if rep.exists("PSP/GAME/.pspdx-stage"):
            fails.append("a staging directory was left behind")
        return fails
    return Scenario(
        "net-server-killed",
        "the catalog's server stopped mid-run, then started again",
        p.script, oracle,
        timeline=[(3, lambda ctx: server(ctx, False)),
                  (22, lambda ctx: server(ctx, True))],
        expect=NET_NOISE + ["Install failed"])


def net_404_500(world):
    """One package answered 404 and one 500. Both installs fail, the record
    each one would have replaced is untouched, and a third package still
    installs from the same server."""
    a, b, c = pick(world, NOT_INSTALLED, 3)
    app_a, app_b, app_c = (world["apps"][i] for i in (a, b, c))
    p = hand(world)
    install_app(p, a)
    install_app(p, b)
    install_app(p, c)
    done(p)

    def plant(ctx):
        faults(ctx, [{"path": pkg_path(app_a.id), "mode": "status", "status": 404},
                     {"path": pkg_path(app_b.id), "mode": "status", "status": 500}])

    def oracle(ctx, rep):
        fails = []
        for text in ("status=404", "status=500"):
            if not rep.has(text):
                fails.append("no download line with %s" % text)
        if rep.count("Install failed") < 2:
            fails.append("expected two failed installs, saw %d"
                         % rep.count("Install failed"))
        check_absent(rep, app_a, fails)
        check_absent(rep, app_b, fails)
        check_record(rep, app_c, fails)
        return fails
    return Scenario(
        "net-404-and-500",
        "a release that answers 404 and one that answers 500",
        p.script, oracle, plant=[plant],
        expect=NET_NOISE + ["Install failed"])


def net_truncated_body(world):
    """Content-Length says one thing and the connection closes after
    another. The download must not be taken for a complete one."""
    a, b = pick(world, NOT_INSTALLED, 2)
    app_a, app_b = world["apps"][a], world["apps"][b]
    p = hand(world)
    install_app(p, a)
    install_app(p, b)
    done(p)

    def plant(ctx):
        faults(ctx, [{"path": pkg_path(app_a.id), "mode": "truncate",
                      "after": 8000}])

    def oracle(ctx, rep):
        fails = []
        if not rep.has("Install failed"):
            fails.append("a truncated download was not reported as a failure")
        check_absent(rep, app_a, fails)
        check_record(rep, app_b, fails)
        if rep.exists("PSP/GAME/.pspdx-stage"):
            fails.append("a staging directory was left behind")
        return fails
    return Scenario(
        "net-truncated-body",
        "a body cut short under a Content-Length that promised more",
        p.script, oracle, plant=[plant], expect=NET_NOISE + ["Install failed"])


def net_bad_sha(world):
    """The archive arrives whole and is not the archive the catalog
    described. The install must stop at the checksum, and the package it was
    an update for must still be the version it was."""
    a = pick(world, UPDATE)
    app_a = world["apps"][a]
    b = pick(world, NOT_INSTALLED)
    app_b = world["apps"][b]
    p = hand(world)
    install_app(p, a)
    install_app(p, b)
    done(p)

    def plant(ctx):
        cat = catalog_read(ctx)
        # Sixty-four hex characters and not one more: catalog.c holds the
        # folded-in release to the manifest's rules, so a hash of the wrong
        # *length* is no release at all and the entry is dropped before it
        # can be installed -- which is right, and not what this is testing.
        wrong = "de" + "ad" * 31
        assert len(wrong) == 64
        entry_of(cat, app_a.id)["release"]["download"]["sha256"] = wrong
        catalog_write(ctx, cat)

    def oracle(ctx, rep):
        fails = []
        if not rep.has("30 apps, 30 usable"):
            fails.append("the entry with the wrong hash was dropped from the "
                         "catalog instead of failing its download")
        if not rep.has("sha256 MISMATCH"):
            fails.append("the checksum mismatch was never logged")
        if not rep.has("Install failed"):
            fails.append("the install did not report a failure")
        # The record is the one the stick had before: an update that failed
        # its checksum may not have moved the version on.
        check_record(rep, app_a, rev=app_a.local_rev, version=app_a.local_version,
                     fails=fails)
        check_record(rep, app_b, fails=fails)
        if rep.exists("PSP/GAME/.pspdx-stage"):
            fails.append("a staging directory was left behind")
        return fails
    return Scenario(
        "net-wrong-sha256",
        "a package whose bytes do not hash to what the catalog says",
        p.script, oracle, plant=[plant],
        expect=NET_NOISE + ["Install failed", "MISMATCH"])


def net_stall_resume(world):
    """The download goes quiet for twenty-two seconds and comes back.
    STALL_TIMEOUT_MS is thirty, per read and not per body, so this one has
    to finish rather than be given up on."""
    a = pick(world, NOT_INSTALLED)
    app_a = world["apps"][a]
    p = hand(world)
    install_app(p, a)
    p.wait(4000)
    browse(p, 3)
    done(p)

    def plant(ctx):
        faults(ctx, [{"path": pkg_path(app_a.id), "mode": "stall",
                      "after": 8000, "seconds": 22}])

    def oracle(ctx, rep):
        fails = []
        if rep.has("read stalled"):
            fails.append("a twenty-two second pause was given up on; the "
                         "timeout is thirty and per read")
        check_record(rep, app_a, fails)
        return fails
    return Scenario(
        "net-stall-then-resume",
        "a download that goes quiet for 22 s and then finishes",
        p.script, oracle, plant=[plant], extra_s=40)


def net_offline_start(world):
    """Nothing listening when the client starts: Offline, X to try again,
    and the catalog when the server comes back. The key script's clock
    starts at the *failed* sync, which is when the client loads it.

    Each X is followed by a circle, because the X's that come after the
    catalog has arrived land on row 0 of All, which is an installed package:
    X there is the options menu on Run, and a second X would be Run. Circle
    closes whatever the X opened, and does nothing on the Offline screen."""
    p = hand(world)
    for _ in range(5):                  # X on the offline screen
        p.key("cross", 6000)
        p.key("circle", 500)
    p.wait(6000)
    browse(p, 5)
    done(p)

    def plant(ctx):
        server(ctx, False)

    def oracle(ctx, rep):
        fails = []
        # A fetch that came back with nothing is "catalog: 0 of 1 sources"
        # now that the catalog is read through sources.txt.
        if rep.count("catalog: 0 of") < 2:
            fails.append("expected at least two failed fetches, saw %d"
                         % rep.count("catalog: 0 of"))
        if not rep.has("30 apps, 30 usable"):
            fails.append("the catalog never arrived after the server came back")
        if not os.path.exists(os.path.join(rep.ms, "PSPDX.BMP")):
            fails.append("no PSPDX.BMP: the Offline screen was never drawn")
        return fails
    return Scenario(
        "net-offline-at-start",
        "the server down at boot, X to retry, then the catalog arrives",
        # Once the catalog does arrive the crosses that are left open and
        # close the menu on row 0; the browsing after that is the proof
        # that the client came all the way back.
        p.script, oracle, plant=[plant], extra_s=45,
        timeline=[(30, lambda ctx: server(ctx, True))],
        expect=NET_NOISE)


def net_catalog_truncated(world):
    """The catalog itself cut off mid-body. Nothing is parsed from half a
    JSON document, and the retry after the fault is lifted must work. The
    circle after each X is for the same reason as in net_offline_start:
    row 0 of All is installed, and X twice there would be Run."""
    p = hand(world)
    for _ in range(4):
        p.key("cross", 8000)
        p.key("circle", 500)
    p.wait(4000)
    browse(p, 5)
    done(p)

    def plant(ctx):
        faults(ctx, [{"path": "/catalog.json", "mode": "truncate", "after": 4000}])

    def oracle(ctx, rep):
        fails = []
        if not rep.has("catalog: 0 of"):
            fails.append("a truncated catalog was not reported as a fetch "
                         "that came back with nothing")
        if rep.has("30 apps, 30 usable") is False:
            fails.append("the catalog never arrived after the fault was lifted")
        return fails
    return Scenario(
        "net-catalog-truncated",
        "the catalog cut off mid-body, then served whole",
        p.script, oracle, plant=[plant], extra_s=45,
        timeline=[(16, lambda ctx: faults(ctx, []))],
        expect=NET_NOISE)


def net_slow_link(world):
    """Sixty kilobytes a second, which is a third of what a PSP-1004's radio
    managed, with a hand on the pad through the whole of it. Keys pressed
    while the loop is away downloading are ORed into one frame when it comes
    back, which is why the two installs come first and the hammering
    after."""
    a, b = pick(world, NOT_INSTALLED, 2)
    app_a, app_b = world["apps"][a], world["apps"][b]
    p = hand(world)
    install_app(p, a)
    p.wait(3000)
    install_app(p, b)
    # Now the hand, on keys that cannot install anything.
    rng = random.Random(0x5704)
    for _ in range(40):
        p.key(rng.choice(("down", "up", "down", "down")), 120)
    done(p)

    def oracle(ctx, rep):
        fails = []
        if not rep.has("network paced to 60 KB/s"):
            fails.append("the client did not read PSPDX.SLOW")
        check_record(rep, app_a, fails)
        check_record(rep, app_b, fails)
        return fails
    return Scenario(
        "net-slow-link",
        "60 KB/s, two installs, and the pad hammered throughout",
        p.script, oracle, extra_args=["--slow", "60"], extra_s=40,
        catalog_s=30)


# ==================================================== 18-23  bad payloads

def payload_no_eboot(world):
    """An archive with nothing to start, and one with two things to start at
    the same depth. Neither is a package, and the record each would have
    replaced stays as it was."""
    a = pick(world, UPDATE)                       # keeps its old record
    b = pick(world, NOT_INSTALLED)
    c = pick(world, NOT_INSTALLED, skip=(b,))
    app_a, app_b, app_c = (world["apps"][i] for i in (a, b, c))
    p = hand(world)
    install_app(p, a)
    install_app(p, b)
    install_app(p, c)
    done(p)

    def plant(ctx):
        set_package(ctx, app_a.id, zip_blob([
            ("%s/README.md" % app_a.dir, b"no eboot here\n"),
            ("%s/data.bin" % app_a.dir, b"\0" * 512)]))
        set_package(ctx, app_b.id, zip_blob([
            ("one/EBOOT.PBP", eboot_bytes(ctx)),
            ("two/EBOOT.PBP", eboot_bytes(ctx))]))

    def oracle(ctx, rep):
        fails = []
        if not rep.has("unpack: no EBOOT.PBP in archive"):
            fails.append("an archive without an EBOOT was not named as such")
        if not rep.has("two EBOOT.PBP at the same depth"):
            fails.append("two EBOOTs at one depth were not refused")
        check_record(rep, app_a, rev=app_a.local_rev,
                     version=app_a.local_version, fails=fails)
        check_absent(rep, app_b, fails)
        check_record(rep, app_c, fails)
        if rep.exists("PSP/GAME/.pspdx-stage"):
            fails.append("a staging directory was left behind")
        return fails
    return Scenario(
        "zip-no-eboot-and-two-eboots",
        "an archive with no EBOOT.PBP, and one with two at the same depth",
        p.script, oracle, plant=[plant],
        expect=UNPACK_NOISE)


def payload_escapes(world):
    """An entry called ../../evil and one with an absolute path, both beside
    a real EBOOT at the archive root so that they are inside the package and
    not beside it. Nothing may be written outside PSP/GAME."""
    a, b = pick(world, NOT_INSTALLED, 2)
    app_a, app_b = world["apps"][a], world["apps"][b]
    p = hand(world)
    install_app(p, a)
    install_app(p, b)
    done(p)

    def plant(ctx):
        set_package(ctx, app_a.id, zip_blob([
            ("EBOOT.PBP", eboot_bytes(ctx)),
            ("../../PSPDX-EVIL.TXT", b"escaped\n")]))
        set_package(ctx, app_b.id, zip_blob([
            ("EBOOT.PBP", eboot_bytes(ctx)),
            ("/PSP/GAME/PSPDX-EVIL2.TXT", b"escaped\n")]))

    def oracle(ctx, rep):
        fails = []
        if rep.count("unpack: refusing") < 2:
            fails.append("expected two refusals, saw %d"
                         % rep.count("unpack: refusing"))
        for rel in ("PSPDX-EVIL.TXT", "PSP/PSPDX-EVIL.TXT",
                    "PSP/GAME/PSPDX-EVIL2.TXT"):
            if rep.exists(rel):
                fails.append("%s was written outside the package" % rel)
        check_absent(rep, app_a, fails)
        check_absent(rep, app_b, fails)
        if rep.exists("PSP/GAME/.pspdx-stage"):
            fails.append("a staging directory was left behind")
        return fails
    return Scenario(
        "zip-path-escapes",
        "entries called ../../evil and /PSP/GAME/evil, both refused",
        p.script, oracle, plant=[plant],
        expect=UNPACK_NOISE)


def payload_header_lies(world):
    """A central directory that disagrees with the local header about a
    size, and a body that is not a zip at all but hashes to what the catalog
    promised. Neither may install; the entry's own crc and size checks are
    the only thing between them and a stick full of somebody's choosing."""
    a, b = pick(world, NOT_INSTALLED, 2)
    app_a, app_b = world["apps"][a], world["apps"][b]
    p = hand(world)
    install_app(p, a)
    install_app(p, b)
    done(p)

    def plant(ctx):
        blob = zip_blob([("%s/EBOOT.PBP" % app_a.dir, eboot_bytes(ctx)),
                         ("%s/README.md" % app_a.dir, b"x" * 400)])
        set_package(ctx, app_a.id,
                    patch_central_usize(blob, "%s/README.md" % app_a.dir, 4000))
        set_package(ctx, app_b.id, b"this is not a zip, and never was.\n" * 64)

    def oracle(ctx, rep):
        fails = []
        if not (rep.has("zip: ") or rep.has("unpack: failed")):
            fails.append("the size the central directory lied about was "
                         "never noticed")
        if not rep.has("unpack: cannot open archive"):
            fails.append("a body that is not a zip was opened anyway")
        check_absent(rep, app_a, fails)
        check_absent(rep, app_b, fails)
        if rep.exists("PSP/GAME/.pspdx-stage"):
            fails.append("a staging directory was left behind")
        return fails
    return Scenario(
        "zip-header-lies",
        "a central directory that disagrees with its local header, and a "
        "body that is not a zip",
        p.script, oracle, plant=[plant],
        expect=UNPACK_NOISE)


def payload_stored(world):
    """A stored archive -- method 0, no deflate -- carrying a file of no
    bytes at all and a directory entry. All three are things a real release
    has been seen to contain, and all three have to install."""
    a = pick(world, NOT_INSTALLED)
    app_a = world["apps"][a]
    p = hand(world)
    install_app(p, a)
    browse(p, 3)
    done(p)

    def plant(ctx):
        set_package(ctx, app_a.id, zip_blob([
            ("%s/" % app_a.dir, b""),
            ("%s/EBOOT.PBP" % app_a.dir, eboot_bytes(ctx)),
            ("%s/EMPTY.BIN" % app_a.dir, b""),
            ("%s/sub/" % app_a.dir, b""),
            ("%s/sub/NOTE.TXT" % app_a.dir, b"stored\n")],
            method=zipfile.ZIP_STORED))

    def oracle(ctx, rep):
        fails = []
        check_record(rep, app_a, fails)
        empty = rep.path("PSP/GAME", app_a.dir, "EMPTY.BIN")
        if not os.path.exists(empty):
            fails.append("the zero-byte file was not written")
        elif os.path.getsize(empty) != 0:
            fails.append("the zero-byte file is %d bytes" % os.path.getsize(empty))
        if not rep.exists("PSP/GAME", app_a.dir, "sub/NOTE.TXT"):
            fails.append("the subdirectory was not written")
        return fails
    return Scenario(
        "zip-stored-and-empty-file",
        "a method-0 archive with a zero-byte file and a subdirectory",
        p.script, oracle, plant=[plant])


def payload_many_files(world):
    """Two thousand tiny files. What is being measured is that the unpack
    finishes at all and that the record is still the right record after it;
    how long it took goes in the report."""
    a = pick(world, NOT_INSTALLED)
    app_a = world["apps"][a]
    p = hand(world)
    install_app(p, a)
    p.wait(8000)
    done(p)

    def plant(ctx):
        entries = [("%s/EBOOT.PBP" % app_a.dir, eboot_bytes(ctx))]
        for i in range(2000):
            entries.append(("%s/f%04d.txt" % (app_a.dir, i),
                            b"%04d\n" % i))
        set_package(ctx, app_a.id, zip_blob(entries))

    def oracle(ctx, rep):
        fails = []
        check_record(rep, app_a, fails)
        if not rep.has("unpack: 2001 files"):
            fails.append("the unpack did not report 2001 files")
        got = rep.path("PSP/GAME", app_a.dir)
        n = len(os.listdir(got)) if os.path.isdir(got) else 0
        if n != 2001:
            fails.append("PSP/GAME/%s holds %d files, expected 2001"
                         % (app_a.dir, n))
        return fails
    return Scenario(
        "zip-two-thousand-files",
        "an archive of 2000 tiny files, unpacked and recorded",
        p.script, oracle, plant=[plant], extra_s=200)


def payload_deep_eboot(world):
    """The EBOOT four directories down. The shallowest EBOOT decides where
    the package is, and the last part of that path is what the directory on
    the stick is called -- not the id, and not the top of the archive."""
    a = pick(world, NOT_INSTALLED)
    app_a = world["apps"][a]
    deep = app_a.dir + "Deep"
    p = hand(world)
    install_app(p, a)
    browse(p, 3)
    done(p)

    def plant(ctx):
        set_package(ctx, app_a.id, zip_blob([
            ("README.md", b"beside the package\n"),
            ("src/main.c", b"int main(void){return 0;}\n"),
            ("build/out/%s/EBOOT.PBP" % deep, eboot_bytes(ctx)),
            ("build/out/%s/DATA.BIN" % deep, b"\0" * 64)]))

    def oracle(ctx, rep):
        fails = []
        check_record(rep, app_a, dirname=deep, fails=fails)
        if not rep.exists("PSP/GAME", deep, "EBOOT.PBP"):
            fails.append("PSP/GAME/%s/EBOOT.PBP is not there" % deep)
        if rep.exists("PSP/GAME", deep, "README.md"):
            fails.append("what sat beside the package was installed with it")
        if not rep.has("-> PSP/GAME/%s" % deep):
            fails.append("the log does not say where the package was found")
        return fails
    return Scenario(
        "zip-deep-eboot",
        "an EBOOT four directories down, beside a source tree",
        p.script, oracle, plant=[plant])


# ===================================================== 24-26  bad catalog

def catalog_bad_entries(world):
    """Six entries that are not entries, among the thirty that are: an id
    with spaces in it, an id with "..", a rev past what an unsigned holds, a
    size of zero, an entry with neither a release nor a manifest, and one
    with no name. Each is dropped and the count says so; the good entry
    after them still installs."""
    extra_good = synth_app(len(world["apps"]), "91.goodextra", "Good Extra",
                           "apps", 1789900000, "1.0.0")
    ext = extend_world(world, [extra_good])
    p = hand(ext)
    install_app(p, extra_good.index)
    browse(p, 4)
    done(p)

    def plant(ctx):
        cat = catalog_read(ctx)
        base = url_base(ctx)
        bad = []
        spaced = synth_app(0, "92.spaced", "Spaced", "apps", 1789900001, "1.0.0")
        spaced.id = "dev.pspdx.mock.92 with spaces"
        bad.append(entry_json(spaced, base))
        dotted = synth_app(0, "93.dotted", "Dotted", "apps", 1789900002, "1.0.0")
        dotted.id = "dev.pspdx.mock.../93.dotted"
        bad.append(entry_json(dotted, base))
        big = synth_app(0, "94.bigrev", "Big Rev", "apps", 1789900003, "1.0.0")
        big_json = entry_json(big, base)
        big_json["release"]["published_at"] = "bad date"
        bad.append(big_json)
        nosize = synth_app(0, "95.nosize", "No Size", "apps", 1789900004, "1.0.0")
        nosize_json = entry_json(nosize, base)
        nosize_json["release"]["download"]["size"] = 0
        bad.append(nosize_json)
        naked = synth_app(0, "96.naked", "Naked", "apps", 1789900005, "1.0.0")
        bad.append(entry_json(naked, base, package=False))
        nameless = synth_app(0, "97.nameless", "", "apps", 1789900006, "1.0.0")
        bad.append(entry_json(nameless, base))

        good = entry_json(extra_good, base)
        cat["apps"] = cat["apps"] + bad + [good]
        catalog_write(ctx, cat)
        blob = zip_blob([("%s/EBOOT.PBP" % extra_good.dir, eboot_bytes(ctx))])
        with open(os.path.join(ctx.site, "pkgs", "%s.zip" % extra_good.id), "wb") as fh:
            fh.write(blob)
        cat = catalog_read(ctx)
        release = entry_of(cat, extra_good.id)["release"]
        release["download"]["sha256"] = hashlib.sha256(blob).hexdigest()
        release["download"]["size"] = len(blob)
        catalog_write(ctx, cat)

    def oracle(ctx, rep):
        fails = []
        want = "37 apps, 31 usable"
        if not rep.has(want):
            got = [l for l in rep.lines if l.startswith("catalog:")]
            fails.append("expected %r, saw %r" % (want, got[:3]))
        check_record(rep, extra_good, fails)
        for dropped in ("92", "93", "94", "95", "96", "97"):
            for app_id in rep.records:
                if app_id.startswith("dev.pspdx.mock.%s" % dropped):
                    fails.append("%s got a record and should have been dropped"
                                 % app_id)
        return fails
    return Scenario(
        "catalog-bad-entries",
        "six unusable entries among the thirty, each dropped and counted",
        p.script, oracle, plant=[plant])


def catalog_hostile_strings(world):
    """A name of two hundred characters, a summary of three hundred, a
    version of forty, two entries sharing one id, and one whose icon,
    screenshot and film are all 404. Every one of them is a string the
    client copies into a fixed field or a URL it fetches."""
    long_name = "Nebula " * 28 + "End"                 # 199 characters
    long_summary = ("A summary that does not stop. " * 10 + "Fin")[:300]
    long_version = "1.0.0-rc1+build.20260911.0123456789abcdef"[:40]
    idx = len(world["apps"])
    big = synth_app(idx, "81.longstrings", long_name[:200], "apps",
                    1789810000, long_version)
    dup1 = synth_app(idx + 1, "82.twinned", "Twin One", "apps", 1789820000, "1.0.0")
    dup2 = synth_app(idx + 2, "82.twinned", "Twin Two", "apps", 1789820000, "1.0.0")
    dup2.id = dup1.id
    dup2.dir = dup1.dir
    missing = synth_app(idx + 3, "83.noassets", "No Assets", "apps",
                        1789830000, "1.0.0")
    ext = extend_world(world, [big, dup1, dup2, missing])
    p = hand(ext)
    if not p.goto_app(missing.index):                  # let the media thread try
        raise SystemExit("edge: cannot reach the 404-asset row")
    p.wait(4000)
    install_app(p, big.index)
    if not p.goto_app(dup1.index):
        raise SystemExit("edge: cannot reach the twin")
    p.wait(3000)
    done(p)

    def plant(ctx):
        cat = catalog_read(ctx)
        base = url_base(ctx)
        entries = []
        for app in (big, dup1, dup2, missing):
            entry = entry_json(app, base)
            entry["name"] = app.name
            entries.append(entry)
        entries[0]["summary"] = long_summary
        # Shaped like the catalog's own paths, one directory an app, so what
        # the client is handed here is a real path that answers with a 404
        # and not a path it would never meet.
        gone = "apps/%s/" % entries[3]["id"]
        entries[3]["media"] = {"icon": gone + "icon-00000000.png",
                                "screenshot": gone + "picture-00000000.png",
                                "video": gone + "film-00000000.pmf"}
        cat["apps"] = cat["apps"] + entries
        catalog_write(ctx, cat)
        for app in (big, dup1, missing):
            blob = zip_blob([("%s/EBOOT.PBP" % app.dir, eboot_bytes(ctx))])
            with open(os.path.join(ctx.site, "pkgs", "%s.zip" % app.id), "wb") as fh:
                fh.write(blob)
            cat = catalog_read(ctx)
            for entry in cat["apps"]:
                if entry["id"] == app.id:
                    entry["release"]["download"]["sha256"] = hashlib.sha256(blob).hexdigest()
                    entry["release"]["download"]["size"] = len(blob)
            catalog_write(ctx, cat)

    def oracle(ctx, rep):
        fails = []
        want = "34 apps, 34 usable"
        if not rep.has(want):
            got = [l for l in rep.lines if l.startswith("catalog:")]
            fails.append("expected %r, saw %r" % (want, got[:3]))
        # struct manifest's version is 32 bytes, so what reaches the record
        # is the first thirty-one characters of the forty.
        check_record(rep, big, version=long_version[:31], fails=fails)
        # assets.c logs the entry's id and not the URL it asked for, so what
        # a 404 looks like in the log is "png: rc=0 status=404, <id>".
        if not any("status=404" in l and missing.id in l for l in rep.lines):
            fails.append("no 404 was logged for %s: its assets were never "
                         "fetched, or a missing one is not reported"
                         % missing.id)
        return fails
    return Scenario(
        "catalog-hostile-strings",
        "a 200-character name, a 300-character summary, a 40-character "
        "version, a duplicated id and three assets that 404",
        p.script, oracle, plant=[plant])


def catalog_seventy(world):
    """Seventy entries against MAX_APPS 64. The client takes sixty-four and
    the log says how many there were."""
    extra = []
    for i in range(40):
        extra.append(synth_app(len(world["apps"]) + i, "7%02d.filler" % i,
                               "Filler %d" % i, "demos", 1789700000 + i,
                               "1.0.%d" % i))
    kept = extend_world(world, extra[:34])             # 30 + 34 = the 64 kept
    p = hand(kept)
    install_app(p, pick(world, NOT_INSTALLED))
    browse(p, 6)
    done(p)
    app_a = world["apps"][pick(world, NOT_INSTALLED)]

    def plant(ctx):
        cat = catalog_read(ctx)
        base = url_base(ctx)
        cat["apps"] = cat["apps"] + [entry_json(app, base) for app in extra]
        catalog_write(ctx, cat)

    def oracle(ctx, rep):
        fails = []
        want = "70 apps, 64 usable"
        if not rep.has(want):
            got = [l for l in rep.lines if l.startswith("catalog:")]
            fails.append("expected %r, saw %r" % (want, got[:3]))
        check_record(rep, app_a, fails)
        for app in extra[34:]:
            if app.id in rep.records:
                fails.append("%s is past the 64th entry and got a record"
                             % app.id)
        return fails
    return Scenario(
        "catalog-seventy-apps",
        "seventy entries where MAX_APPS is sixty-four",
        p.script, oracle, plant=[plant])


# ==================================================== 27-29  stick state

def stick_broken_records(world):
    """Four states the database can be found in that no install ever wrote:
    a record of garbage, a record cut off mid-JSON, a record whose directory
    is gone, and a directory with no record at all."""
    garbage, truncated, no_dir = pick(world, CURRENT, 3)
    no_record = pick(world, CURRENT, skip=(garbage, truncated, no_dir))
    g, t, n, w = (world["apps"][i] for i in (garbage, truncated, no_dir, no_record))
    p = hand(world)
    # The planner navigates by the model, and the model has to see the
    # stick the way the client will: a record it cannot read is no record,
    # so those three rows are offered as not installed, and X on them is
    # the install question rather than the options menu.
    for index in (garbage, truncated, no_record):
        p.sim.apps[index].state = NOT_INSTALLED
        p.sim.apps[index].local_rev = 0
        p.sim.apps[index].local_version = ""
        p.sim.db.pop(p.sim.apps[index].id, None)
    p.sim.view_rebuild()
    install_app(p, garbage)             # offered as not installed: install it
    remove_app(p, no_dir)               # a record whose files are already gone
    install_app(p, no_record)           # a directory nothing remembers
    done(p)

    def plant(ctx):
        ctx.write(ctx.db_path(g.id), b"\x00\x01\x02not json at all\xff\xfe")
        ctx.write(ctx.db_path(t.id),
                  ('{"id":"%s","rev":178900' % t.id).encode())
        shutil.rmtree(ctx.game_path(n.dir), ignore_errors=True)
        ctx.remove(ctx.db_path(w.id))

    def oracle(ctx, rep):
        fails = []
        if rep.count("is not json") < 2:
            fails.append("expected two unreadable records to be named, saw %d"
                         % rep.count("is not json"))
        # The garbage record was read as no record at all, so the row offered
        # an install, and the install wrote a record over the garbage.
        check_record(rep, g, fails)
        # The truncated one was never touched: nothing rewrites a record the
        # user did not act on.
        if t.id not in rep.records:
            fails.append("the truncated record was removed; nothing should "
                         "have touched it")
        # A record whose directory was already gone still removes cleanly.
        if n.id in rep.records:
            fails.append("%s was not removed" % n.id)
        if n.dir in rep.dirs:
            fails.append("PSP/GAME/%s came back" % n.dir)
        # A directory with no record is installed over, and gets one.
        check_record(rep, w, fails)
        return fails
    return Scenario(
        "stick-broken-records",
        "garbage, half a JSON document, a record with no directory and a "
        "directory with no record",
        p.script, oracle, plant=[plant], invariants=False,
        expect=["is not json"])


def stick_interrupted_install(world):
    """What the battery leaves behind: a staging tree under PSP/GAME and two
    .old directories, one whose replacement made it and one whose did not.
    install_recover() runs before anything reads the database and has to
    settle all three."""
    kept, restored = pick(world, CURRENT, 2)
    k, r = world["apps"][kept], world["apps"][restored]
    p = hand(world)
    browse(p, 4)
    install_app(p, pick(world, NOT_INSTALLED))
    done(p)

    def plant(ctx):
        # A staging tree from an install that never committed.
        ctx.plant_dir(".pspdx-stage", {"EBOOT.PBP": b"half an install\n",
                                       "sub/DATA.BIN": b"\0" * 32})
        # One where the new copy is already in place: the .old is the loser.
        ctx.plant_dir("%s.old" % k.dir, {"EBOOT.PBP": b"the old copy\n"})
        # One where it is not: the .old is all there is, and must come back.
        shutil.move(ctx.game_path(r.dir), ctx.game_path("%s.old" % r.dir))
        ctx.made_dirs.append(ctx.game_path("%s.old" % r.dir))

    def oracle(ctx, rep):
        fails = []
        if not rep.has("recovered: dropped %s.old" % k.dir):
            fails.append("the .old beside a live directory was not dropped")
        if not rep.has("recovered: restored %s" % r.dir):
            fails.append("the .old with no live directory was not restored")
        if rep.exists("PSP/GAME/.pspdx-stage"):
            fails.append("the staging tree survived the recovery")
        left = [d for d in rep.dirs if d.endswith(".old")]
        if left:
            fails.append("PSP/GAME still holds %s" % ", ".join(left))
        if r.dir not in rep.dirs:
            fails.append("PSP/GAME/%s was not restored" % r.dir)
        if not rep.exists("PSP/GAME", r.dir, "EBOOT.PBP"):
            fails.append("the restored directory has no EBOOT.PBP")
        return fails
    return Scenario(
        "stick-interrupted-install",
        "a leftover staging tree and two .old directories, recovered at boot",
        p.script, oracle, plant=[plant], expect=["recovered:"])


def stick_orphan_and_self(world):
    """A record for a package the catalog has never heard of, and PSPDX's
    own record claiming a version this build is not.

    The orphan is the one to read the code for before asserting: parse()
    walks the catalog and asks the database about each entry, and nothing
    anywhere walks the database. So an orphan is invisible -- not shown, not
    offered, not removed -- and it keeps its directory forever. That is what
    this asserts, and the report says why it is worth looking at again."""
    ghost_id = "dev.pspdx.mock.98.ghost"
    ghost_dir = "PSPDXMock98Ghost"
    self_id = "io.github.chriopter.pspdxapp"
    p = hand(world)
    browse(p, 5)
    install_app(p, pick(world, NOT_INSTALLED))
    done(p)

    def plant(ctx):
        ctx.write(ctx.db_path(ghost_id), json.dumps(
            {"id": ghost_id, "rev": 1789980000, "dir": ghost_dir,
             "manifest": "", "version": "3.2.1"}).encode() + b"\n")
        ctx.plant_dir(ghost_dir, {"EBOOT.PBP": b"a package nobody lists\n"})
        ctx.write(ctx.db_path(self_id), json.dumps(
            {"id": self_id, "rev": 0, "dir": "PSPDX",
             "manifest": "", "version": "9.9.9"}).encode() + b"\n")

    def oracle(ctx, rep):
        fails = []
        ghost = rep.record(ghost_id)
        if not ghost:
            fails.append("the orphan record was removed by something")
        elif ghost.get("rev") != 1789980000 or ghost.get("dir") != ghost_dir:
            fails.append("the orphan record was rewritten: %r" % ghost)
        if ghost_dir not in rep.dirs:
            fails.append("the orphan's directory was removed")
        mine = rep.record(self_id)
        if not mine:
            fails.append("PSPDX's own record is gone")
        elif mine.get("version") != build_version(ctx):
            fails.append("the self record says %r; this build is %r"
                         % (mine.get("version"), build_version(ctx)))
        elif not rep.has("self: record now says"):
            fails.append("the self record changed without a line saying so")
        return fails
    return Scenario(
        "stick-orphan-and-self-record",
        "a record for an id the catalog does not have, and a self record "
        "with the wrong version",
        p.script, oracle, plant=[plant], invariants=False)


# ========================================================= 30  long idle

def idle_six_minutes(world):
    """Six minutes of the guest's own clock, going down the list at reading
    speed with a film looping behind every row that has one. Nothing is
    fetched and nothing is installed, so every ten-second window has to be
    clean: no late frame anywhere, and the free memory at the end within
    64 KB of the free memory at the start."""
    rng = random.Random(0x1D1E)
    p = hand(world)
    while p.t < 6 * 60 * 1000:
        gap = rng.randint(1800, 3000)
        if rng.random() < 0.10:
            p.key(rng.choice(("left", "right")), gap)
        else:
            p.key(rng.choices(("down", "up"), weights=(5, 1))[0], gap)
    done(p)
    base, _pred = model_oracle(world, model.parse_script(scenarios.render(p.script)))

    def oracle(ctx, rep):
        fails = base(ctx, rep)
        late = [(i, w["late"]) for i, w in enumerate(rep.windows)
                if w["late"] and not w["busy"]]
        for i, n in late:
            fails.append("window %d had %d late frames with nothing fetching"
                         % (i, n))
        if len(rep.free_kb) < 4:
            fails.append("only %d memory readings in six minutes"
                         % len(rep.free_kb))
        else:
            first, last = rep.free_kb[0], rep.free_kb[-1]
            if first - last > 64:
                fails.append("free memory went from %d KB to %d KB, %d KB "
                             "gone over six minutes" % (first, last, first - last))
        if len(rep.windows) < 25:
            fails.append("only %d ten-second windows in a six-minute run"
                         % len(rep.windows))
        return fails
    return Scenario(
        "idle-six-minutes",
        "six minutes of browsing at reading speed, films looping",
        p.script, oracle)


# ------------------------------------------------------------------ server

def server(ctx, up):
    """The mock server stopped and started under the emulator, which is how
    a scenario breaks something a client cannot break for itself."""
    import subprocess
    rig = os.path.join(HERE, "rig.sh")
    subprocess.run(["sh", rig, "serve" if up else "stop"], check=False,
                   stdout=subprocess.DEVNULL)


# --------------------------------------------------------------- the list

BUILDERS = [
    storm_mixed, storm_confirm, storm_bands, storm_refresh, storm_refresh_basket,
    bulk_basket_all, bulk_update_all, bulk_remove_all, bulk_cycle,
    net_server_killed, net_404_500, net_truncated_body, net_bad_sha,
    net_stall_resume, net_offline_start, net_catalog_truncated, net_slow_link,
    payload_no_eboot, payload_escapes, payload_header_lies, payload_stored,
    payload_many_files, payload_deep_eboot,
    catalog_bad_entries, catalog_hostile_strings, catalog_seventy,
    stick_broken_records, stick_interrupted_install, stick_orphan_and_self,
    idle_six_minutes,
]


def build_all(world):
    return [make(world) for make in BUILDERS]


def main():
    import argparse
    ap = argparse.ArgumentParser(description="print the edge scenarios")
    ap.add_argument("--site", default=None)
    ap.add_argument("--only", default=None)
    ap.add_argument("--keys", action="store_true", help="print the key script")
    args = ap.parse_args()
    world = model.load_world(args.site)
    for i, sc in enumerate(build_all(world), 1):
        if args.only and args.only not in sc.name:
            continue
        print("%2d  %-32s %3d keys  %6.1f s of guest  %s"
              % (i, sc.name, len(sc.script), sc.script[-1][0] / 1000.0, sc.why))
        if args.keys:
            print(scenarios.render(sc.script), end="")


if __name__ == "__main__":
    main()
