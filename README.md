# PSPDX

**PSP Download Index** — the missing package manager for PSP homebrew.

Gives you a [catalog](https://chriopter.github.io/pspdx-catalog/) of sick & current Brews for the PSP including updating them from a central place!

**[Browse the catalog →](https://chriopter.github.io/pspdx-catalog/)**

- **[Format & schema](https://github.com/chriopter/pspdx-schema)** — what a `.pspdx` says, and the schema every one of them names.
- **[Reference catalog](https://chriopter.github.io/pspdx-catalog/)** — browse apps; or [build your own catalog](https://github.com/chriopter/pspdx-catalog).
- **[Demo app](https://github.com/chriopter/pspdx-demo)** — a complete example for homebrew authors.

<img src="pspdx.webp" width="480" alt="Start, the entropy sweep, the catalog, three moves down the list, the info menu">

The same twenty seconds [with sound](pspdx.mp4): the tune, and the notes the list plays.

Runs under PPSSPP; nobody has put it on real hardware yet.

The keys are the system's own: **×** installs or updates what the cursor is
on, **△** opens the options (Run, Reinstall, Delete, Add to basket,
Information), **○** goes back, **□** sets a row aside in the basket, **START**
runs an installed app. The shoulders and left/right walk the tabs: the gear
(the session: connection, entropy, frames, *Update catalog*), the stick (what
is installed, updates first, *Update all* while any wait), the basket, then
the categories. Left alone for ten seconds the shell fades out and leaves the
water and the film; any key brings it back.

Under PPSSPP the keys are what `dev/ppsspp/controls.ini` says, which
dev/start installs: × S, ○ D, □ A, △ W, START Enter, SELECT Space, L Q,
R E, the d-pad on the arrows, the stick on I J K L. There is no HOME;
Esc is the emulator's own pause menu.

To get your open source licensed brew listed: a `.pspdx` file in your
repository's root, a GitHub release with one zip that has an `EBOOT.PBP`
in it, and one line in
[pspdx-catalog](https://github.com/chriopter/pspdx-catalog)'s `repos.txt`.
[pspdx-demo](https://github.com/chriopter/pspdx-demo) is the whole of it,
done right, in a hello world.

## What an app is

A repository that says so. The format lives in its own repository now, at
[pspdx-schema](https://github.com/chriopter/pspdx-schema):
[manifest.md](https://github.com/chriopter/pspdx-schema/blob/master/manifest.md) is
what a `.pspdx` says,
[`schema/v1.pspdx`](https://github.com/chriopter/pspdx-schema/blob/master/schema/v1.pspdx)
the schema every one of them names. [architecture.md](architecture.md) is
this client's why.

<details>
<summary><b><code>.pspdx</code></b> — the author's consent and words. Everything that changes comes from the release and the EBOOT.</summary>

```json
{
  "schema":   "https://github.com/chriopter/pspdx-schema/blob/master/schema/v1.pspdx",
  "source":     "https://github.com/chriopter/pspdx-demo",
  "name":     "PSPDX Demo",
  "summary":  "Hello, PSP: the smallest thing PSPDX can list.",
  "category": "demo",
  "license":  "MIT",
  "installdir": "PSP/GAME/PSPDXDemo"
}
```

Required: `schema`, `source`, `name`, `category`, `installdir`. The source
URL lets a standalone file point back to its project. Optional summary,
licence and author default to GitHub metadata. Pictures, film and sound
come from the EBOOT. Version, date, download size and hash come from the
release; the id is derived from the repository URL.

</details>

<details>
<summary><b>Lists and the catalog</b> — where the console finds apps: a text file of repositories, and a cache anyone can build.</summary>

A *list* is a text file, one GitHub repository a line. A `cache` line at
the top names a `catalog.json` that has read every `.pspdx`, verified every
release and mirrored the pictures; the console takes the cache when it
answers and reads at the origin what it did not cover: the file and the
pictures from `raw.githubusercontent.com`, the release from GitHub's API.
An installed app is pictured from its own EBOOT on the stick either way.

[pspdx-catalog](https://github.com/chriopter/pspdx-catalog) is the list the
console ships with, `repos.txt`, and the workflow that rebuilds its cache
every hour. Anyone can publish another; the console keeps its lists in
`PSP/PSPDX/sources.txt` and takes more through the gear tab, where a single
repository typed as `owner/repo` is a list of one.

</details>

<details>
<summary><b>The id, and how an update is noticed</b> — one name that is a URL, a cache file and a record on the stick; one number that says whether the stick is behind.</summary>

The id is the repository URL in the reverse-domain style of Flatpak ids:
`io.github.chriopter.psprustraytracer` for
`github.com/chriopter/psp-rust-raytracer` (the dashes go). Nobody types
it, so it cannot be wrong. Lowercase letters, digits, dots and dashes,
at most eighty characters, no `..`. The rules are strict because the same
string becomes a directory on the catalog (`apps/<id>/icon-<sha8>.png`), a
file in the cache (`PSP/PSPDX/cache/<id>-icon-<sha8>.png`) and a record on
the stick, so it has to be a path component everywhere. It is not the directory under `PSP/GAME`: that
one comes out of the archive, and the record remembers it.

What is installed lives in `PSP/PSPDX/db/<id>.json`, one file per app,
one line:

```json
{"id":"io.github.chriopter.rustraytracer","rev":1789071038,"dir":"RustRaytracer","manifest":"","version":"0.4.0"}
```

`rev` is the release's publication time as a number, taken from GitHub by
the scanner; `dir` is the directory actually written under `PSP/GAME`,
which removing and replacing need; `version` is only for display. The
record is the last thing an install writes, after the staging directory
has been renamed into place — to a temporary name first and then over the
old record, so a battery that dies between the two leaves the previous
record rather than an empty file. Removing an app deletes the tree first
and the record second, so a directory left behind can never be silently
overwritten later.

An update is a comparison, nothing more: at start, and again on *Update
catalog* under the gear tab, the client reads every record and holds each `rev` against the
`release.rev` in the catalog. A larger number in the catalog marks the
row with the turning arrows and offers *Update to <version>*; the same
number is a tick; no record is *not installed*. Updating is the install
path over again — download, checksum, unpack into staging, rename — with
the previous copy stepping aside as `<dir>.old` until the new one is in
place, then gone.

PSPDX is an entry like any other and updates itself the same way. Its first
start writes its own record from nothing but the build: the directory it was
started from and the version the build was made with, with rev 0, since a rev
is the moment GitHub published a release and a build cannot know its own. The
first catalog it sees settles that: the same version (or a build past that
tag, as `git describe` names it) means the stick is running the release, and
the catalog's rev goes into the record; a different one is an update, taken
through the ordinary path — the running EBOOT is in RAM, the directory under
it is replaced, and the band says to press START to restart. It refuses to
remove itself. The version comes from `git describe --tags` at build time;
`dev/release 0.1.0` builds with it set, packs the EBOOT and publishes the
release, and the catalog derives the rest on its next build.

</details>


## What is listed

| | |
|---|---|
| [Extreme Tux Racer](https://github.com/chriopter/psp-tuxracer) | a real port: 44 MB, 639 files, GPL-2.0 |
| [Rust Raytracer](https://github.com/chriopter/psp-rust-raytracer) | a real demo, and the smallest thing that still looks like something |

[The catalog page](https://chriopter.github.io/pspdx-catalog/) is the current
list.

## App Tech

The on-device client lives in `app/`. It collects entropy, fetches the catalog
over TLS 1.3, and installs or updates a package from its manifest. File paths
below are relative to `app/`.

- One request gets the whole index: [pspdx-catalog](https://github.com/chriopter/pspdx-catalog) is folded into a single `catalog.json`. The PSP pays per TLS handshake, not per byte.
- Downloads come from the author's own release. PSPDX hosts nothing and mirrors nothing.
- The truth is the author's `.pspdx`, release and EBOOT; the index is a cache of what they say. A list without a cache still works: the console reads each app at the origin itself.
- TLS 1.3, with the seed collected off the analog stick at startup, because the PSP has no usable PRNG.
- Every manifest sits on the same GitHub host, so one handshake covers the whole update check.

<details>
<summary><b>How it is laid out</b> — one directory per layer, and what each one is responsible for.</summary>

One directory per layer; every include names its directory, so the layer a
header comes from is visible at the include line.

| Directory | What |
|---|---|
| `app/main.c` | the controller: input loop, screens, nothing else |
| `app/gui/` | the browser on the GE: `shell` lays out, `lattice` is the moving backdrop, `preview` owns what is on the card, `gfx` draws, `font` is the PSP's own face; the entropy sweep floods the same surface and leaves it behind as the backdrop |
| `app/video/` | the film on the card: `mp4` finds the H.264 in the catalog's clip, `psmf` wraps it the way the PSP's decoder wants it, `player` runs that decoder; only `player.c` knows it is on a PSP |
| `app/audio/` | a piano and a glass, the tune, and the sounds the interface makes; only `audio.c` knows it is on a PSP |
| `app/logic/` | the entropy pool |
| `app/update/` | the catalog, what is out of date, and the pictures it links to |
| `app/install/` | manifest, download, verify, unpack, the on-stick database |
| `app/network/` | HTTPS and the compiled-in roots |
| `app/util/` | logging and the millisecond clock |

Dependencies run one way: `gui` → `update` → `install` → `network`, with
`util` under all of them. Nothing in `update/`, `install/` or `network/`
draws or plays a note: `gui/preview.c` asks `update/assets.c` for bytes and
turns them into a texture itself, and the shell posts sounds into a queue
that the audio thread empties. The code that has to be right and the code
that has to look good do not share a file.

The card the picture sits on is the one thing drawn in real perspective; it
stands still, and a sweep of light crosses it now and then. Once the still
is up, the entry's film fades in over it and loops. Each row of the list
carries its bundle's icon, fetched one at a time behind the card for the
rows on screen, and every selection lights the room a colour drawn by lot.

</details>

<details>
<summary><b>The TLS 1.3 client</b> — wolfSSL on <code>sceNetInet</code> sockets: what it offers, and where the randomness under it comes from.</summary>

wolfSSL over `sceNetInet` sockets, TLS 1.3 only. It offers X25519 ahead of
P-256 and ChaCha20-Poly1305 ahead of AES-128-GCM, because this CPU has no AES
instruction; `network/bench.c` measures the gap on the device itself. The key
share rides along with the ClientHello, so no HelloRetryRequest and one
handshake covers the whole catalog.

Its randomness is the sweep: one bit for every turn of the stick onto a
newly touched point of an invisible 250x250 field, until the pool holds the
128 that X25519 and ChaCha20-Poly1305 stand on. A straight stroke pays once,
however far it runs. The pool goes on taking packet arrival times and
battery readings for the rest of the run, uncounted, and reaches `PSPDX.SEED`
once, on the way out through HOME.

</details>

<details>
<summary><b>What it trusts</b> — 17 roots compiled in, because the PSP has no CA store worth using.</summary>

The chain is verified against `app/network/ca_certs.h`, 17 roots compiled in. The
PSP has no CA store worth using, so the client carries its own; regenerate it with

```sh
python3 app/tools/make-ca-bundle.py
```

A host whose CA is not in there fails the handshake and names the CA in
`PSPDX.LOG`. That is the signal to add it.

A root that no distribution ships yet goes in `app/ca-extra/` as one PEM per
file. The script refuses any file there that is not itself signed by a root in
the host's own store, so putting a certificate in it does not add trust -- it
only carries trust that already exists to a console that has no store. What is
in there now is `isrg-root-yr.pem`, ISRG's 2026 root, cross-signed by ISRG Root
X1: GitHub Pages serves a chain ending in it, and it is newer than the
ca-certificates package on most machines.

</details>

<details>
<summary><b>The film</b> — an MP4 on the way in, a PSMF on the way out, decoded on the Media Engine.</summary>

The film is the app's own `ICON1.PMF`, a PSMF out of the EBOOT, and the
decoder, `sceMpeg` on the Media Engine, plays it as it is. Older entries
serve a plain MP4 -- H.264 baseline -- and the PSP's decoder does not read
MP4: it wants the same H.264 in a PSMF, Sony's envelope -- a header, then
an MPEG-2 program stream in 2048-byte packs. So the client wraps such a film
itself, in RAM, on the way in:
`video/mp4.c` finds the samples and the parameter sets, `video/psmf.c`
packs them, `video/player.c` feeds the result through a ring buffer and
decodes straight into the card's texture, thirty pictures a second by the
clock. Nothing Sony-shaped ever touches the catalog or the network.

The wrapping runs on a desk too, where ffmpeg can check it:

```sh
cd app
cc -I. video/mp4.c video/psmf.c tools/mp4-to-psmf.c
./a.out video.mp4 video.psmf && ffprobe video.psmf
```

Two things the decoder does that the code has to know. It writes every
pixel with alpha zero, so the film's texture is drawn as colour only. And
on PPSSPP it takes a little longer than real time per picture, which is why
the film is paced by the clock rather than by the frame: a slow frame is
followed by two pictures, not by drift.

The PSMF header carries what PPSSPP's own parser reads from real files; a
PSP has not run it yet.

The tune renders on a desk with the same code the PSP runs, which is how it
gets listened to:

```sh
cd app
cc -I. audio/synth.c audio/music.c audio/cues.c tools/render-music.c -lm
./a.out music.wav
```

</details>

<details>
<summary><b>Building</b> — two Docker lines, or a pspdev tarball and no Docker at all.</summary>

```sh
cd app
docker run --rm -v "$PWD:/src" -w /src pspdev/pspdev:latest sh wolfssl-psp/build.sh
docker run --rm -v "$PWD:/src" -w /src pspdev/pspdev:latest make
```

The first line builds wolfSSL into `wolfssl-psp/prefix` (a few minutes, once);
the second produces `EBOOT.PBP`. Copy it to `ms0:/PSP/GAME/pspdx/`, or open it
in PPSSPP.

Without Docker, the same works with a pspdev release tarball unpacked
anywhere: set `PSPDEV` to it and put its `bin/` on `PATH`. CMake is needed for
the wolfSSL step.

</details>

<details>
<summary><b>Running it on a desk</b> — one command to build it and open it in PPSSPP.</summary>

```sh
dev/start           # build, then open it in PPSSPP, one instance, straight to the catalog
dev/start-reset     # the same without the seed: the sweep runs, as on a new PSP
dev/start --quiet   # without speakers
dev/start --slow    # the network held to a PSP-1004's 180 KB/s
dev/start --mock    # a made-up catalog of thirty served from the host, a third of it installed
```

Both plant what a run needs on the emulator's stick -- the system font,
the clips from the catalog repo next door, a seed -- and stop whatever
instance was running before, since two at once write the same files and
play the same tune slightly apart.

</details>

<details>
<summary><b>Running it without a screen</b> — headless, for a rig: it writes what it did to the stick.</summary>

The app writes what it did to the memory stick, so a run needs no window:

```sh
sh app/run-ppsspp.sh                          # 25 seconds, straight to the catalog
sh app/run-ppsspp.sh 30 --sweep               # replay the recorded sweep first
sh app/run-ppsspp.sh 30 --keys keys.txt       # scripted input, see PSPDX.KEYS below
sh app/tools/localcat/run 60 --keys keys.txt  # the same against forty apps served from the host
python3 app/tools/soak/run.py --runs 100      # a hundred customers, the stick checked against a model
python3 app/tools/soak/run.py --perf 20       # twenty runs of held keys, stick and film, every late frame named
python3 app/tools/soak/run.py --edge 30       # thirty edge cases: storms, bulk, network faults, bad archives, bad catalogs, a broken stick
```

A rig run gets no speakers: the emulator is started without its audio
socket, and the client's own stream is checked through PPSSPP's `DumpAudio`
when it has to be. Holding a button is beyond a keys file; for that the
emulator's WebSocket debugger takes `input.buttons.press` with a duration
in frames -- and this PPSSPP build aborts when that socket is closed, so a
driver keeps it open until the emulator is gone.

It copies the EBOOT to the emulator's memory stick and leaves `PSPDX.LOG` and
`shot.png` beside itself. By default it also plants a fixed seed, so the
client skips the sweep the way it does on a PSP after its first run and is
at the catalog a few seconds in; `--sweep` replays `app/testdata/sweep.trace`
through the entropy screen instead. A replayed sweep never writes a seed,
because replayed input is not entropy.

That trace is one entry per frame, `{ u8 lx, u8 ly, u16 buttons }`, and
comes out of `app/tools/sweep-trace.py`: a hand drawn from `/dev/urandom`,
heading by heading, until the screen's own rule has paid its 128 bits, then
X, about six seconds of it. `sweep-full.trace` is 1608 samples of a human
actually moving the stick and stays as what the rate was measured on. To replay it by hand instead,
copy it to `PSPDX.TRACE` on the emulator's stick and touch `PSPDX.REPLAY` next
to it; remove that file to go back to collecting. A replayed run says so on
screen and never writes a seed: the path is public, so it is a development
aid and not a source of randomness.

PPSSPP does not mount `flash0:` for the guest, so the script also copies the
emulator's own copy of the system font to where the client's fallback path
looks. On hardware the font comes out of the firmware.

PPSSPP only flushes an emulated file to the host on close, which is why the log
is written in one go at the end rather than line by line.

The final screen is also dumped to `PSPDX.BMP`. It is read back through the
GE rather than straight out of VRAM: on PPSSPP the memory behind a GE-drawn
frame is not kept current, and a CPU read hands back a frame that is long
gone. Copying through the GE is what games do for their save icons, and it
is the path the emulator keeps honest. That is also the only screenshot path
that works on a real PSP, and on a host whose desktop is locked.

</details>

<details>
<summary><b>Files it leaves on the stick</b> — every path the client writes, and what is in it.</summary>

| Path | What |
|---|---|
| `PSPDX.SEED` | 20 bytes of pool state, so the sweep happens once; rewritten on the way out through HOME |
| `PSPDX.TRACE` | every pad sample of the last sweep, for replay |
| `PSPDX.REPLAY` | if present, the sweep replays `PSPDX.TRACE` instead of reading the stick |
| `PSPDX.LOG` | what the run did |
| `PSPDX.HTTP` | the raw response of the last fetch |
| `PSPDX.BMP` | the catalog screen, 480x272, 24-bit |
| `PSPDX1.BMP` | the screen a scripted `shot` key asked for |
| `PSPDX2.BMP` | the screen after an install |
| `PSPDX.KEYS` | scripted input, one `<ms> <key>` per line from the moment the catalog is up: `up`, `down`, `left`, `right`, `cross`, `circle`, `square`, `triangle`, `ltrigger`, `rtrigger`, `select`, `start`, or `shot` |
| `PSPDX.INSTALL` | a repository URL here installs that app unattended, for testing |
| `PSP/PSPDX/sources.txt` | the lists, one URL a line, the built-in one first |
| `PSP/PSPDX/cache/<served name>` | a picture once fetched, so it costs one handshake per stick, not per run. The catalog keeps an app's files in `apps/<id>/` and calls them `icon-<sha8>.png`, `picture-`, `film-`, `sound-`; the id goes in front of a served name that does not already carry it, so the file here is `<id>-icon-<sha8>.png`. The hash is in the name, so a changed picture arrives as a file this stick has never seen and the old one is simply never asked for again. An entry that links nothing is looked up as `<id>.<ext>`, which is where the rig plants a clip |
| `PSP/PSPDX/font/ltn8.pgf` | never written by the client: where it looks for the system font when `flash0:` has none |
| `PSP/PSPDX/db/<id>.json` | what was installed: rev, directory, manifest URL |

</details>

## Open

- **Real hardware.** It has only ever run in PPSSPP.
- **Nothing is signed**, so the index is trusted completely. Fine while one
  person writes it; less fine now that a bot does.
  - Support multiple server / Game server
  - solution for branch support / release tag
