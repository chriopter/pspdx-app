#!/usr/bin/env python3
"""What the client would do, worked out on a desk.

A key script is a list of button presses at known times. The client's input
loop -- app/main.c, with the view and the tabs from app/gui/shell.c -- turns
that into a cursor walking a filtered list, questions answered, packages
installed and removed. This file is that loop again in Python, close enough
that the set of records under PSP/PSPDX/db and the directories under PSP/GAME
can be predicted before the emulator is started, and then held against what
the run actually left.

It is a model of the *decisions*, not of the machine: it assumes every
install and every fetch succeeds, because a failure is what the harness is
looking for and a model that predicted failures would have nothing to catch.

Where main.c and shell.c are mirrored line for line the comment says so.
Three things a script can press are refused here rather than modelled,
because the client would leave the loop and never come back to the script:
Run (sceKernelLoadExec ends the program), the two typing rows of the info
band (the firmware keyboard owns the pad) and the entropy sweep (it reads
the pad itself, and a script's keys are never seen by it).
"""

import importlib.machinery
import importlib.util
import json
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

# ----------------------------------------------------------------- buttons

# Names as app/main.c's button_named() spells them. START is deliberately
# absent from every script this harness writes: launch_app() hands the PSP to
# the package with sceKernelLoadExec and the run is over, log and all.
KEYS = ("up", "down", "left", "right", "cross", "circle", "square",
        "triangle", "select", "ltrigger", "rtrigger", "shot")

# ------------------------------------------------------------------ timing

# What the loop is away for, and therefore how long a script has to leave
# before its next key. The planners wait exactly these and the model counts
# the idle clock from them, so they live here and nowhere else.
#
# The mock packages are 85 KB, but every install is a fresh handshake, a
# download, a hash, an unpack and two renames, and a key that lands inside
# that window comes out the other side ORed with whatever else queued up.
# Measured on this rig at well under two seconds; the margin is deliberate
# and cheap.
INSTALL_MS = 2600
REMOVE_MS = 900
# The whole catalog again, and keys_pressed() is not even consulted until it
# lands.
REFRESH_MS = 9000

# main.c: ten seconds without a key and the shell fades; the first key after
# that only lifts the veil and is otherwise swallowed. Scripted keys count as
# keys, the stick does not, and the clock starts again whenever the loop has
# been away (an install, a refetch, a settled screenshot).
IDLE_MS = 10000

# -------------------------------------------------------------------- tabs

# shell.c: TAB_NAME / TAB_KEY, and the three tabs that are not categories.
TAB_KEY = ("", "game", "demo", "app", "emulator", "plugin")
TAB_ALL = 6
TAB_GEAR = -3                   # the band about the session; always leftmost
TAB_STICK = -2                  # what is installed, updates first
TAB_BASKET = -1
ROW_ACTION = -2

NOT_INSTALLED, CURRENT, UPDATE = "none", "current", "update"

# catalog.h: the one package the client will not delete, because it is the
# one running.
PSPDX_SELF_ID = "io.github.chriopter.pspdx"

# main.c: enum choice, the five rows of the options menu in the order drawn.
(CHOICE_RUN, CHOICE_REINSTALL, CHOICE_DELETE, CHOICE_BASKET,
 CHOICE_DETAILS) = range(5)
CHOICE_COUNT = 5

# shell.h: SHELL_INFO_ACTIONS, the rows at the foot of the info band.
INFO_REFRESH, INFO_ADD_SOURCE, INFO_FROM_GITHUB, INFO_SWEEP = range(4)
INFO_ACTIONS = 4

# --------------------------------------------------------------- the world

class App:
    """One catalog entry, plus what the stick says about it."""

    def __init__(self, index, id, name, category, rev, version, size, dirname):
        self.index = index
        self.id = id
        self.name = name
        self.category = category
        self.rev = rev                  # release.rev in the catalog
        self.version = version          # release.version in the catalog
        self.size = size
        self.dir = dirname              # the directory its archive unpacks to
        self.has_release = bool(rev and size)
        self.state = NOT_INSTALLED
        self.local_rev = 0
        self.local_version = ""


def load_mock_module():
    """dev/mock-catalog, imported rather than re-implemented: the ids, the
    directory names and the seeded thirds are its business and copying them
    here would be a second source of truth to keep in step."""
    path = os.path.join(REPO, "dev", "mock-catalog")
    loader = importlib.machinery.SourceFileLoader("pspdx_mock_catalog", path)
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


def mock_plan():
    """The state mock-catalog plants for each row, recomputed from its own
    seed. make() draws the thirds before it draws anything else, so the same
    Random in the same order gives the same list."""
    mc = load_mock_module()
    rng = random.Random(mc.SEED)
    third = mc.COUNT // 3
    states = (["current"] * third + ["update"] * third +
              ["none"] * (mc.COUNT - 2 * third))
    rng.shuffle(states)
    rows = []
    for i, (name, category, _author, _kinds) in enumerate(mc.ROWS[:mc.COUNT]):
        rows.append({
            "index": i,
            "id": "%sapp%02d%s" % (mc.ID_PREFIX, i, mc.slug(name).lower()[:32]),
            "dir": "%s%02d%s" % (mc.DIR_PREFIX, i, mc.slug(name)[:21]),
            "name": name,
            "category": category,
            "rev": mc.REV_BASE + i * 1000,
            "version": "%d.%d.%d" % (1 + i // 10, i % 10, i % 3),
            "old_rev": mc.REV_BASE + i * 1000 - 500,
            "old_version": "%d.%d.%d" % (i // 10, i % 10, i % 3),
            "state": states[i],
        })
    return mc, rows


def load_world(site_dir=None):
    """The catalog as the client will parse it, and the stick as mock-catalog
    will have planted it. The sizes come from the generated site when it is
    there, so the confirm band's tally is the real one; without it the model
    only needs to know a size is not zero."""
    mc, rows = mock_plan()
    sizes = {}
    if site_dir:
        path = os.path.join(site_dir, "catalog.json")
        if os.path.exists(path):
            for entry in json.load(open(path))["apps"]:
                sizes[entry["id"]] = entry["release"]["size"]

    apps = []
    db = {}
    dirs = set()
    for row in rows:
        app = App(row["index"], row["id"], row["name"], row["category"],
                  row["rev"], row["version"], sizes.get(row["id"], 1), row["dir"])
        if row["state"] == "current":
            app.local_rev, app.local_version = row["rev"], row["version"]
        elif row["state"] == "update":
            app.local_rev, app.local_version = row["old_rev"], row["old_version"]
        if row["state"] != "none":
            # catalog.c's parse() marks it APP_UNKNOWN and check_updates()
            # then compares release.rev against the record's.
            app.state = UPDATE if app.rev > app.local_rev else CURRENT
            db[app.id] = {"id": app.id, "rev": app.local_rev, "dir": app.dir,
                          "manifest": "", "version": app.local_version}
            dirs.add(app.dir)
        apps.append(app)
    return {"apps": apps, "db": db, "dirs": dirs,
            "id_prefix": mc.ID_PREFIX, "dir_prefix": mc.DIR_PREFIX}


# ------------------------------------------------------------- the machine

class Sim:
    """app/main.c's loop and app/gui/shell.c's view, as far as a script can
    reach them. One press per call: the harness never puts two keys on one
    millisecond, because keys_pressed() ORs everything whose moment has
    passed into a single frame's `pressed` and two buttons in one frame is
    not a thing a hand does."""

    def __init__(self, world):
        self.apps = [App(a.index, a.id, a.name, a.category, a.rev, a.version,
                         a.size, a.dir) for a in world["apps"]]
        for mine, theirs in zip(self.apps, world["apps"]):
            mine.state = theirs.state
            mine.local_rev = theirs.local_rev
            mine.local_version = theirs.local_version
        self.db = {k: dict(v) for k, v in world["db"].items()}
        self.dirs = set(world["dirs"])

        self.basket = set()
        self.tabs = []
        self.tab_at = 0
        self.view = []
        self.view_action = 0
        self.cursor = 0

        self.question = None            # None / "install" / "remove" / "all"
        self.question_of = -1
        self.menu_open = False
        self.menu_cursor = 0
        self.menu_of = -1
        self.menu_on = [0] * CHOICE_COUNT
        self.info = False               # the band, open while the gear tab is
        self.info_action = 0
        self.details = False            # the band about one package

        self.installs = 0               # how many install_app() calls happened
        self.events = []                # ("install"|"remove"|"refresh", t, id)
        self.shots = 0
        self.swallowed = []             # (t, key) that only lifted the veil
        self.refresh_pending_at = None
        self.keep = ""

        # main.c: idle_since is set when the catalog comes up (count was 0
        # until then, which resets it every frame), which is the script's
        # own zero.
        self.last_key_t = 0
        self.hidden = False

        # main.c after sync_done(): cursor 0, then the view built afresh.
        self.cursor = 0
        self.view_rebuild()

    # ---------------------------------------------------------- shell.c

    def updates_waiting(self):
        return sum(1 for a in self.apps if a.state == UPDATE)

    def collect_tabs(self, keep):
        """shell.c collect_tabs(): the gear first and always, the stick while
        anything is installed, the basket while anything is in it, then the
        categories that have something. A tab that has gone is answered with
        All, not with whatever stands leftmost -- that is the band about the
        session and not a list at all."""
        found = False
        self.tabs = []
        self.tab_at = 0
        if not self.apps:
            return False
        self.tabs.append(TAB_GEAR)
        if any(a.state != NOT_INSTALLED for a in self.apps):
            self.tabs.append(TAB_STICK)
        if self.basket:
            self.tabs.append(TAB_BASKET)
        for t in range(TAB_ALL):
            has = not TAB_KEY[t]
            if not has:
                has = any(a.category == TAB_KEY[t] for a in self.apps)
            if has:
                self.tabs.append(t)
        for i, tab in enumerate(self.tabs):
            if tab == keep:
                self.tab_at = i
                found = True
        if not found:
            for i, tab in enumerate(self.tabs):
                if tab == 0:
                    self.tab_at = i
        return found

    def build_view(self):
        """shell.c build_view(): the stick lists what is installed with the
        updates first, in catalog order within each half; under the gear the
        list stays whole beneath the band; the basket is the basket. The
        action row stands on the basket, and on the stick only while an
        update waits."""
        tab = self.tabs[self.tab_at] if self.tabs else 0
        self.view = []
        self.view_action = 0
        if not self.apps or not self.tabs:
            return
        for i, a in enumerate(self.apps):
            if tab == TAB_STICK:
                take = a.state != NOT_INSTALLED
            elif tab == TAB_GEAR:
                take = True
            elif tab == TAB_BASKET:
                take = i in self.basket
            else:
                take = (not TAB_KEY[tab]) or a.category == TAB_KEY[tab]
            if take:
                self.view.append(i)
        if tab == TAB_STICK:
            waiting = [i for i in self.view if self.apps[i].state == UPDATE]
            rest = [i for i in self.view if self.apps[i].state != UPDATE]
            self.view = waiting + rest
        self.view_action = 1 if (tab == TAB_BASKET or
                                 (tab == TAB_STICK and
                                  self.updates_waiting() > 0)) else 0

    def view_rebuild(self):
        was = self.tabs[self.tab_at] if self.tabs else 0
        self.basket.clear()             # shell_view_rebuild() drops it whole
        self.collect_tabs(was)
        self.build_view()

    def tabs_refresh(self):
        was = self.tabs[self.tab_at] if self.tabs else 0
        kept = self.collect_tabs(was)
        self.build_view()
        return kept

    def view_count(self):
        return len(self.view) + self.view_action

    def view_index(self, row):
        if self.view_action and row == 0:
            return ROW_ACTION
        row -= self.view_action
        return self.view[row] if 0 <= row < len(self.view) else -1

    def view_row(self, index):
        for row, at in enumerate(self.view):
            if at == index:
                return row + self.view_action
        return -1

    def tab_kind(self):
        """The active tab as shell.c numbers it: a category index at or
        above zero, or one of TAB_GEAR / TAB_STICK / TAB_BASKET."""
        return self.tabs[self.tab_at] if self.tabs else 0

    def action_plan(self):
        """shell_action_plan(): on the stick the job is the updates alone
        and what is merely installed is not counted at all, not even as
        skipped."""
        plan = {"apps": 0, "again": 0, "skipped": 0, "bytes": 0, "updates": 0}
        if not self.view_action:
            return plan
        plan["updates"] = 1 if self.tab_kind() == TAB_STICK else 0
        for at in self.view:
            a = self.apps[at]
            if plan["updates"] and a.state != UPDATE:
                continue
            if not a.has_release or not a.size:
                plan["skipped"] += 1
                continue
            plan["apps"] += 1
            plan["bytes"] += a.size
            if a.state == CURRENT:
                plan["again"] += 1
        return plan

    def tab_move(self, step):
        if len(self.tabs) <= 1:
            return
        self.tab_at = (self.tab_at + step + len(self.tabs)) % len(self.tabs)
        self.build_view()

    # ----------------------------------------------------------- main.c

    def view_settled(self):
        at = self.view_index(self.cursor)
        if not self.tabs_refresh():
            self.cursor = 0
            return
        row = self.view_row(at) if at >= 0 else -1
        count = self.view_count()
        if row >= 0:
            self.cursor = row
        elif self.cursor >= count:
            self.cursor = count - 1 if count > 0 else 0

    def away(self, t, ms):
        """The loop gone for a while: main.c notices a frame that took more
        than 300 ms and starts the idle clock again when it is back, so the
        ten seconds count from the end of the wait and not from the key
        that started it. The wait is the planner's, which is the only clock
        this model has for it."""
        self.last_key_t = max(self.last_key_t, t + ms)

    def install_app(self, index, t):
        """install_app() with rc 0. A failure is what the rig is for."""
        a = self.apps[index]
        a.state = CURRENT
        a.local_rev = a.rev
        a.local_version = a.version
        # install.c db_write(): the release out of the catalog carries no
        # manifest URL, so the record's "manifest" is empty.
        self.db[a.id] = {"id": a.id, "rev": a.rev, "dir": a.dir,
                         "manifest": "", "version": a.version}
        self.dirs.add(a.dir)
        self.installs += 1
        self.events.append(("install", t, a.id))
        self.away(t, INSTALL_MS)

    def uninstall_app(self, index, t):
        a = self.apps[index]
        a.state = NOT_INSTALLED
        a.local_rev = 0
        a.local_version = ""
        self.db.pop(a.id, None)
        self.dirs.discard(a.dir)
        self.events.append(("remove", t, a.id))
        self.away(t, REMOVE_MS)

    def install_all(self, t):
        """main.c install_all(): the rows are read into a list before the
        first fetch, because an install moves the entry's state under a loop
        still walking the view. On the stick only the updates are taken."""
        picked = []
        on_stick = self.tab_kind() == TAB_STICK
        for row in range(self.view_count()):
            at = self.view_index(row)
            if at < 0:
                continue
            a = self.apps[at]
            if not a.has_release or not a.size:
                continue
            if on_stick and a.state != UPDATE:
                continue
            picked.append(at)
        for at in picked:
            self.install_app(at, t)
            self.basket.discard(at)     # shell_basket_forget on success
        self.away(t, len(picked) * INSTALL_MS)
        return len(picked)

    def ask_install(self, index):
        self.question = "install"
        self.question_of = index

    def ask_remove(self, index):
        a = self.apps[index]
        if a.id == PSPDX_SELF_ID:
            return                      # "PSPDX cannot remove itself"
        record = self.db.get(a.id)
        if not record or not record["dir"]:
            return                      # shell_status, and no question
        self.question = "remove"
        self.question_of = index

    def ask_all(self):
        if self.action_plan()["apps"] <= 0:
            return
        self.question = "all"
        self.question_of = -1

    def basket_toggle(self, index):
        if index in self.basket:
            self.basket.discard(index)
        else:
            self.basket.add(index)

    def menu_open_for(self, index):
        """main.c menu_open(): five rows, the same five for every package;
        what a row cannot do it says by being grey. The cursor starts on Run
        for anything installed and on the basket row otherwise."""
        a = self.apps[index]
        installed = a.state != NOT_INSTALLED
        self.menu_on = [1 if installed else 0,
                        1 if installed else 0,
                        1 if installed and a.id != PSPDX_SELF_ID else 0,
                        1, 1]
        self.menu_cursor = CHOICE_RUN if installed else CHOICE_BASKET
        self.menu_of = index
        self.menu_open = True

    def menu_move(self, by):
        """A greyed row is stepped over rather than landed on."""
        for _ in range(CHOICE_COUNT):
            self.menu_cursor = (self.menu_cursor + by + CHOICE_COUNT) % CHOICE_COUNT
            if self.menu_on[self.menu_cursor]:
                break

    def refresh_start(self, t):
        """X on the band's first row -> fetch again. By then main.c has
        already stepped off the gear tab onto the one after it, so the
        package to come back to is row 0 of *that* tab -- or nothing, when
        row 0 is an action row. sync_start() is called and `synced` goes to
        zero, which stops keys_pressed() from being consulted at all: every
        scripted key whose moment passes while the catalog is being fetched
        is ORed into one frame when it comes back. The scripts this harness
        writes leave that window empty; press() refuses one that does not."""
        at = self.view_index(self.cursor)
        self.keep = self.apps[at].id if at >= 0 else ""
        self.refresh_pending_at = t
        self.events.append(("refresh", t, self.keep))
        self.away(t, REFRESH_MS)

    def refresh_finish(self):
        """The second sync coming back: the catalog is parsed again from what
        is now on the stick, the view is rebuilt (which drops the basket) and
        the cursor goes back to the package it was on."""
        for a in self.apps:
            record = self.db.get(a.id)
            if record:
                a.local_rev = record["rev"]
                a.local_version = record["version"]
                a.state = UPDATE if a.rev > a.local_rev else CURRENT
            else:
                a.state = NOT_INSTALLED
                a.local_rev = 0
                a.local_version = ""
        self.cursor = 0
        self.view_rebuild()
        if self.keep:
            for a in self.apps:
                if a.id == self.keep:
                    row = self.view_row(a.index)
                    if row >= 0:
                        self.cursor = row
                    break
            self.keep = ""
        self.refresh_pending_at = None

    # ------------------------------------------------------------ input

    def nothing_open(self):
        """True when the key would reach the list itself: no question, no
        menu, no details band and not standing on the gear tab. These are
        also the only conditions under which the idle veil can come down."""
        return (self.question is None and not self.menu_open and
                not self.details and not self.info)

    def press(self, key, t=0, refresh_ms=8000):
        if self.refresh_pending_at is not None:
            if t - self.refresh_pending_at < refresh_ms:
                raise ValueError(
                    "key %r at %d falls inside the refetch window that started "
                    "at %d; the client would queue it and fire it with the "
                    "others" % (key, t, self.refresh_pending_at))
            self.refresh_finish()

        # main.c: the veil comes down after ten seconds with nothing standing
        # over the browser, and the first key after that lifts it and does
        # nothing else. A scripted shot is the exception in both directions:
        # it neither lifts the veil nor is swallowed, so the veil is a flag
        # and not a comparison -- it stays down across a shot. Either way
        # the key resets the clock.
        if self.nothing_open() and t - self.last_key_t > IDLE_MS:
            self.hidden = True
        self.last_key_t = t
        if self.hidden and key != "shot":
            self.hidden = False
            self.swallowed.append((t, key))
            return

        count = self.view_count() if self.apps else 0

        # main.c: modal is a question, the menu or the details band; the
        # info band is not among them, so the tabs still walk under it.
        modal = self.question is not None or self.menu_open or self.details

        # The triggers and left/right walk the tabs, and the list starts
        # again at the top. Walking onto the gear tab opens the band with
        # its cursor on the first action; walking off it closes it.
        if key in ("ltrigger", "rtrigger", "left", "right") and count > 0 and not modal:
            self.tab_move(1 if key in ("rtrigger", "right") else -1)
            self.cursor = 0
            count = self.view_count()
            self.info = self.tab_kind() == TAB_GEAR
            self.info_action = 0

        modal = modal or self.info
        if key == "down" and count > 0 and not modal:
            self.cursor = (self.cursor + 1) % count
        if key == "up" and count > 0 and not modal:
            self.cursor = (self.cursor + count - 1) % count
        if key == "shot":
            self.shots += 1

        if self.question is not None:
            if key == "cross":
                asked, index = self.question, self.question_of
                self.question = None
                if asked == "install":
                    self.install_app(index, t)
                elif asked == "all":
                    self.install_all(t)
                else:
                    self.uninstall_app(index, t)
                self.view_settled()
            elif key == "circle":
                self.question = None
        elif self.menu_open:
            # The keys the menu names work from inside it too: square does
            # its row's thing and takes the menu with it, START would too.
            index = self.menu_of
            if key == "square":
                self.menu_open = False
                self.basket_toggle(index)
                self.view_settled()
            elif key == "start" and self.apps[index].state != NOT_INSTALLED:
                raise ValueError("START in the menu at %d launches %s: a "
                                 "script must never" % (t, self.apps[index].id))
            if not self.menu_open:
                pass
            elif key == "down":
                self.menu_move(1)
            elif key == "up":
                self.menu_move(-1)
            elif key == "circle":
                self.menu_open = False
            elif key == "cross":
                chosen = self.menu_cursor
                self.menu_open = False
                if chosen == CHOICE_DELETE:
                    self.ask_remove(index)
                elif chosen == CHOICE_RUN:
                    raise ValueError("X on Run at %d launches %s: a script "
                                     "must never" % (t, self.apps[index].id))
                elif chosen == CHOICE_BASKET:
                    self.basket_toggle(index)
                    self.view_settled()
                elif chosen == CHOICE_DETAILS:
                    self.details = True
                else:
                    self.install_app(index, t)
                    self.view_settled()
        elif self.info:
            if key == "down":
                self.info_action = (self.info_action + 1) % INFO_ACTIONS
            if key == "up":
                self.info_action = (self.info_action + INFO_ACTIONS - 1) % INFO_ACTIONS
            # O steps off the band's tab onto the one after it, and so does
            # taking any of the band's actions.
            if key in ("circle", "cross"):
                self.info = False
                self.tab_move(1)
                self.cursor = 0
                count = self.view_count()
            if key == "cross":
                action = self.info_action
                if action == INFO_REFRESH:
                    self.refresh_start(t)
                elif action in (INFO_ADD_SOURCE, INFO_FROM_GITHUB):
                    # osk_read(): the firmware keyboard takes the pad and
                    # the script's keys go nowhere until it is dismissed by
                    # a hand. There is no modelling that.
                    raise ValueError("X on info action %d at %d opens the "
                                     "keyboard: a script cannot type" % (action, t))
                else:
                    # entropy_screen_run() reads the pad itself: the keys
                    # the script has left are never seen by it, and the
                    # sweep waits for a stick that nobody is moving.
                    raise ValueError("X on the sweep at %d takes the pad away "
                                     "from the script" % t)
        elif self.details:
            if key == "circle":
                self.details = False
        elif count > 0:
            at = self.view_index(self.cursor)
            if key == "cross":
                # X is the one thing there is to do to the package: have it,
                # or have the newer one. With nothing of that to do it opens
                # the options, as triangle does.
                if at == ROW_ACTION:
                    self.ask_all()
                elif at >= 0 and self.apps[at].state in (NOT_INSTALLED, UPDATE):
                    self.ask_install(at)
                elif at >= 0:
                    self.menu_open_for(at)
            if key == "triangle" and at >= 0:
                self.menu_open_for(at)
            if key == "square" and at >= 0:
                self.basket_toggle(at)
                self.view_settled()
            if key == "start" and at >= 0 and self.apps[at].state != NOT_INSTALLED:
                raise ValueError("START at %d launches %s: a script must never"
                                 % (t, self.apps[at].id))
            # SELECT does nothing on the browser any more.

    # ----------------------------------------------------------- result

    def finish(self, refresh_ms=8000):
        """Anything still in flight when the keys ran out."""
        if self.refresh_pending_at is not None:
            self.refresh_finish()

    def prediction(self):
        return {
            "db": {k: dict(v) for k, v in sorted(self.db.items())},
            "dirs": sorted(self.dirs),
            "installs": self.installs,
            "shots": self.shots,
            "events": list(self.events),
            "swallowed": list(self.swallowed),
        }


def parse_script(text):
    """A PSPDX.KEYS file back into (ms, key) pairs, the way keys_load() reads
    it: the first two whitespace-separated fields of every line that has
    them, in file order."""
    out = []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        out.append((int(parts[0]), parts[1]))
    return out


def simulate(world, script):
    """A key script against a freshly planted stick. Returns the prediction."""
    sim = Sim(world)
    for at, key in script:
        sim.press(key, at)
    sim.finish()
    return sim.prediction()
