# PSPDX

**PSP Download Index** — browse, install and update homebrew directly
on your PlayStation Portable. **[→ Download](https://github.com/chriopter/pspdx/releases/latest)**

<img width="480" alt="PSPDX starting, browsing the catalog, installing an update and two apps from the basket" src="assets/pspdx-app.webp" />

[The same as a video](assets/pspdx-app.mp4) (480×272, 34 s).

## How to use

Just install, start and install homebrew! Updates are shown automatically.

Needs [ARK-5](https://github.com/PSP-Arkfive/ARK-5) and a saved Wi-Fi
connection. Extract
[`pspdx.zip`](https://github.com/chriopter/pspdx/releases/latest) to the root
of your Memory Stick.

Tested in PPSSPP so far. Running it on a real PSP?
[Tell us how it went](https://github.com/chriopter/pspdx/issues).

## How it works

**The PSPDX app**

- Browses catalogs, installs the release ZIP, checks for updates at start.
- Saves the `.pspdx` file on install, so it can update directly from the
  source. No mirror.

**The standard**

- I propose the **[`.pspdx` standard](#the-pspdx-standard)**. A small file
  that points to your homebrew.
- To support it, just add a `.pspdx` file to your repo. Done!
  [Example](https://github.com/chriopter/pspdx-demo/blob/master/.pspdx)

**The catalog**

- Multiple `.pspdx` files make a catalog,
  [like here](https://github.com/chriopter/pspdx-catalog/blob/master/repos.txt).
  It's just like a phonebook.
- OSS repos can be added to the main catalog.
  [Here](https://github.com/chriopter/pspdx-catalog).
- Or start your own catalog. It's just a bunch of GitHub workflows that
  build a static page. [Please steal it](https://github.com/chriopter/pspdx-catalog)
  and share the link!

**The magic**

- The catalog is only a shortcut. The `.pspdx` is what counts: a portable
  file, independent from any catalog.
  - **Catalog up** → PSP reads the catalog → every app, release and artwork
    in one request, *very fast*.
  - **Catalog gone** → PSP reads its saved `.pspdx` → asks each app's own
    repository.
  - **No catalog at all** → install `.pspdx` files directly.

## The PSPDX standard

A complete example is the [demo app](https://github.com/chriopter/pspdx-demo):

```json
{
  "schema":     "https://chriopter.github.io/pspdx/schema/pspdx-v1.json",
  "source":     "https://github.com/chriopter/pspdx-demo",
  "name":       "PSPDX Demo",
  "tags":       ["demo"],
  "author":     "chriopter",
  "summary":    "Hello, PSP. A demo listing for PSPDX.",
  "license":    "MIT",
  "installdir": "PSP/GAME/PSPDXDemo"
}
```

All fields and rules: [PSPDX standard](https://chriopter.github.io/pspdx/)

I want this to work 10 years forward, without another mirror going down or an
abandoned installer being a hurdle. The `.pspdx` file should live on its own!

<details>
<summary><b>Format specification</b> · releases, derived values, lists</summary>

#### What v1 needs

- A root `.pspdx`, and a published release with exactly one ZIP holding one `EBOOT.PBP`
- Releases give versions and downloads; EBOOTs give media (`ICON0.PNG`, `PIC1.PNG`, `ICON1.PMF`, `SND0.AT3`, all optional)
- No manifest edit per release
- `homebrew` installs under `PSP/GAME/`; `plugin` and `iso` are listed, not installed yet

#### Derived, never written

- `id` → GitHub: `io.github.<owner>.<repo>`; elsewhere the host of `listed_by` reversed, without `www.`, then the name; every part lowercased to `[a-z0-9]`
- `installdir` left out → `PSP/GAME/<repository name>` on GitHub, `PSP/GAME/<name>` elsewhere, cut to 32 allowed characters
- Update → the SHA-256 of `releases[0]` differs from the installed ZIP's; dates and version strings are not compared
- Readers ignore fields they don't know; the schema still names only the real ones, so your editor flags a typo

#### Text list

```text
cache https://chriopter.github.io/pspdx-catalog/catalog.json
https://github.com/chriopter/pspdx-demo
https://github.com/someone/project@v1.2
https://example.com/lists/project.pspdx
```

- One repository URL per line; `@tag` pins a release
- `cache` names an aggregated catalog, read first; repos it doesn't cover are asked directly
- A URL ending in `.pspdx` → a substitute for a GitHub repo without its own: needs `listed_by`, and the repo's own file wins as soon as it has one

#### Checking a `.pspdx` in VS Code

```json
"files.associations": { "*.pspdx": "json" },
"json.schemas": [{ "fileMatch": ["*.pspdx"],
  "url": "https://chriopter.github.io/pspdx/schema/pspdx-v1.json" }]
```

</details>

## Under the hood

<details>
<summary><b>Sources</b> · catalogs, repositories, INBOX</summary>

#### Where apps come from

| Source | Added through | Result |
|---|---|---|
| Catalog site URL, `catalog.json`, text list or a list's `.pspdx` | **Add sources** | Browse all listed apps |
| GitHub URL or `owner/repo` | **Direct install** | Add the repository as a source and install its app |
| `.pspdx` files in `PSP/PSPDX/INBOX/` | **Direct install** | Validate and install the selected files |

- Preset sources, in this order: `https://chriopter.github.io/pspdx-catalog/`, `https://wijsman.de/psp-homebrew-database/`
- Presets ship as `PSP/GAME/PSPDX/presets.txt`, same lines as `sources.txt`; the EBOOT carries a copy for a stick without one
- Each preset lands once and is noted in `PSP/PSPDX/presets.seen`: removed stays removed, a new one in an update arrives
- A source that does not load is marked *unreachable* in the list; the rest load as usual
- A substitute `.pspdx` is used only while its repo has none; the install remembers where it came from (`manifest_url`), so updates find it again
- Sources are validated before they land in `PSP/PSPDX/sources.txt`
- Adding a catalog installs nothing; Direct install and INBOX do
- Removing a source keeps installed apps, their manifests and state

#### Presets

```text
start -> presets.txt (or the built-in list) -> in presets.seen? skip
                                            -> append to sources.txt unless there -> note in presets.seen
```

#### Reading a catalog site

```text
catalog.json  ->  ok: browse
   | fail
catalog.txt   ->  ok: check each listed repository directly
   | fail
saved copy    ->  ok: browse the last snapshot
```

A site with only `catalog.txt` works without a builder.

</details>

<details>
<summary><b>Install and update</b> · checks, recovery</summary>

#### Install

```text
x on app -> confirm -> verify .pspdx (repo's own, else the list's substitute): source + installdir
         -> download release ZIP -> check size, SHA-256, ZIP, paths
         -> stage -> swap into PSP/GAME/<dir> -> write state
```

- Only the folder holding the single `EBOOT.PBP` is installed
- An unmanaged folder in the way is never adopted or overwritten; PSPDX offers to move it to `<dir>.bak`
- A transaction journal covers files, manifest and state; an interrupted install recovers at the next start

#### Check

Runs at startup and on **Check for updates**, the top row of the stick tab.
An update is a newest release whose SHA-256 differs from the installed one.
Dates and version strings are not compared; a record from before SHA-256 was
kept falls back to a newer `published_at`.

**×** on **Check for updates** runs this; **□** runs it forced.

```text
1. Fetch every source in sources.txt.

2. For each installed app:
     in a catalog that answered and is under 24 h old?
       yes, not forced   ->  take the catalog entry, no GitHub request
       otherwise         ->  step 3

3. Direct check due?
     due if forced, never asked, last answer came from a catalog,
     or the last direct answer is older than 6 h
       due               ->  .pspdx          raw.githubusercontent.com   no limit
                             (none there: the substitute at manifest_url)
                             latest release  api.github.com              1 of 60 an hour
       not due           ->  the last answer saved in the record

4. releases[0] SHA-256 differs         ->  "Update to x.y"

5. Catalog older than 24 h             ->  the status line names its host
```

- A check against a maintained catalog is one request and no API call
- An app added by **Direct install** or from INBOX costs one API call, then none for six hours
- Only GitHub sources are asked directly; an app from anywhere else updates through a catalog
- Nothing is topped up from GitHub: a `.pspdx` without author is by the account in its URL, a missing summary or licence stays empty

#### Apply

- Checking never installs; confirm an update with **×**
- **○** during download or unpack cancels the install and puts the stick back; in a batch it also stops the rest
- INBOX skips conflicts; only successful imports leave INBOX
- Self-updates run last; restart PSPDX afterward

#### Keys

**×** install, confirm · **△** options · **○** back · **□** basket · **START** run · **L/R** or **←/→** tabs

**△** → **Information**: version, author, tags, size, id, then summary and description; **↑/↓** scroll, **L/R** a page

</details>

<details>
<summary><b>Network</b> · HTTPS, TLS 1.3, offline</summary>

#### Requests

```text
catalog   ->  one request, gzip asked for, release data for every app
direct    ->  raw.githubusercontent.com   .pspdx, up to 16 KB
          ->  api.github.com              latest release
download  ->  release ZIP, following GitHub asset redirects
```

- First saved PSP network profile
- Everything over HTTPS
- Only a catalog asks for gzip, inflated as it arrives into a 512 KB buffer; `pspdx.log` says bytes on the wire and inflated. A ZIP is never asked for compressed
- TLS connections to the same host are briefly reused; a closed or expired one is reopened

#### TLS

- Not a perfect TLS 1.3, on purpose: a PSP has no trustworthy clock, no root store, no way to update either, and may have sat in a drawer for years. The goal is that it still connects, not maximum security
- Done by [pspkit-https](https://github.com/chriopter/pspkit-https): TLS 1.3 only, ChaCha20-Poly1305 first, all of Mozilla's roots, certificate dates held only against the day it was built. Its README says how and why
- An expired certificate, or one from an unknown issuer, asks "connect anyway?" instead of failing; a bad signature or the wrong hostname always fails
- The real guarantee: the ZIP you install is the one the catalog names (SHA-256), not that this connection is as safe as your browser's
- Packages are not signed; a catalog SHA-256 checks ZIP integrity, not author identity
- Seed: a sweep of the analog stick or smashed buttons when there is none yet, `CRYPTO/seed.bin` afterwards, renewed with Renew TLS Seed

#### Offline

- Refresh runs in the background; installing pauses network previews, they share the network stack
- Cached browsing works offline; release checks and downloads need a connection

</details>

<details>
<summary><b>Catalog</b> · updates, previews, cache</summary>

#### What the builder does

```text
hourly:  repos.txt -> catalog.txt
         new release?  -> read .pspdx (or the list's substitute)
                       -> hash up to the 20 newest release ZIPs, each once -> extract EBOOT media
         unchanged?    -> reuse the entry
         -> publish catalog.json
```

- `catalog.json` carries metadata, source, install path, up to the 20 newest releases (date, ZIP URL, size, hash) and media URLs; the console reads `releases[0]`
- An entry built from a substitute carries `manifest_url`, so an install from the catalog still finds the file
- A push or manual run also picks up manifest-only edits; hourly runs wait for a release
- A build with no valid apps leaves the live site in place

#### How an update shows

- PSPDX compares the SHA-256 of the catalog's newest release with the installed one
- The catalog does not know what is on your PSP
- The update appears after the catalog publishes and the PSP refreshes; the ZIP still comes from the author's release

#### Previews

- The catalog hosts `ICON0.PNG`, `PIC1.PNG`, `ICON1.PMF` and `SND0.AT3` separately: no ZIP download for a preview; of several screenshots the first is shown
- Priority: installed EBOOT media -> cached or catalog media -> placeholder
- Direct GitHub lookups fetch no new previews

#### Cache

- Catalogs in `PSP/PSPDX/CACHE/catalogs/`, media in `CACHE/media/`
- Failed fetch: the last usable catalog stays for browsing; installed apps from GitHub ask their saved source, at most every six hours
- Offline: saved records and media only
- Both caches can be deleted without losing installation state

</details>

<details>
<summary><b>Memory Stick</b> · manifests, state, cache</summary>

#### Layout

Example on the startup device (`ms0:` or `ef0:`); app IDs are illustrative.
Temporary and debug files appear only when used.

```text
ms0:/
└── PSP/
    ├── GAME/
    │   ├── PSPDX/                       # Downloader
    │   │   ├── EBOOT.PBP
    │   │   ├── .pspdx                   # Bundled for offline first start
    │   │   └── presets.txt              # Sources offered once
    │   ├── Cathedral/                   # Installed homebrew
    │   │   ├── EBOOT.PBP
    │   │   └── ...
    │   ├── .pspdx-stage/                # Temporary install
    │   ├── Cathedral.old/               # Rollback backup
    │   └── Cathedral.bak/               # Unmanaged folder moved aside
    └── PSPDX/
        ├── sources.txt                  # Subscriptions
        ├── presets.seen                 # Presets already offered
        ├── INBOX/demo.pspdx             # Awaiting import
        ├── INSTALLED/
        │   ├── io.github.chriopter.pspdx.pspdx
        │   ├── io.github.chriopter.pspdx.state.json
        │   ├── io.github.chriopter.pspcathedral.pspdx
        │   └── io.github.chriopter.pspcathedral.state.json
        ├── CACHE/
        │   ├── catalogs/<url-sha1>.json
        │   └── media/                   # Named after the served file
        │       ├── <app-id>-<file>.png
        │       ├── <app-id>-<file>.mp4      # or .pmf, as served
        │       └── <app-id>-<file>.at3
        ├── TMP/
        │   ├── download.zip
        │   └── transaction.json
        ├── LOGS/
        │   ├── pspdx.log
        │   └── http.txt
        ├── DEBUG/
        │   ├── PSPDX.BMP
        │   ├── PSPDX.KEYS
        │   ├── PSPDX.REPLAY
        │   ├── PSPDX.TRACE
        │   ├── PSPDX_REC/
        │   ├── font/ltn8.pgf
        │   └── ...
        └── CRYPTO/seed.bin
```

#### App state

`INSTALLED/<app-id>.pspdx` keeps the source independently of `sources.txt`.
`<app-id>.state.json` next to it has no outer app-ID key:

| Field | Contents |
|---|---|
| `source`, `added_from` | Original repository and import route |
| `manifest_url` | The list's substitute `.pspdx`, only when the repo had none |
| `installed` | Version, `published_at`, SHA-256, install directory |
| `latest` | Version, `published_at`, download URL, size, SHA-256, last successful check time and source |
| `update_check` | Written by older versions; read and ignored |

```text
install or update       ->  writes installed
catalog or GitHub check ->  writes latest, leaves installed alone
browse                  ->  writes nothing
```

- `installed 1.0` with `latest 1.1` means 1.0 is still installed
- PSPDX registers its own version at startup and copies its bundled `.pspdx`, no request needed
- `added_from` does not steer future checks; each operation writes only its app's state

#### Recovery and housekeeping

- Recoverable writes may leave `.new` or `.bak` files
- Staging and backup stay under `GAME/`: PSP renames need the same parent
- Keep `INSTALLED/`; `CACHE/` is disposable; never delete a pending transaction's files
- Only per-app state files load; older combined state files are ignored
- Corrupt records are preserved and block writes

</details>

## Development

<details>
<summary><b>Build and run</b> · Docker, PPSSPP</summary>

#### Build

Inside the `pspdev/pspdev:latest` container, or with `$PSPDEV`, cmake and
wget on the host:

```sh
git submodule update --init --recursive
sh app/lib/pspkit-https/tools/build-wolfssl
make -C app
```

- Output: `app/EBOOT.PBP`
- Libraries: pspkit-https (wolfSSL), cJSON, intraFont, libpng, zlib, PSP SDK
- CI builds inside `pspdev/pspdev:latest`

#### Run in the emulator

```text
dev/start             ->  build in Docker -> install on the PPSSPP stick -> launch
dev/start --no-build  ->  launch the existing build (a mock-catalog build is rebuilt anyway)
dev/start --mock      ->  the same against the local mock catalog
dev/release <version> [notes]  ->  clean master -> build -> pspdx.zip -> gh release
```

- Needs Linux, Docker, the PPSSPP Flatpak, Python, a systemd user session and Wayland; `dev/release` also `gh`
- `dev/start` installs `dev/ppsspp/controls.ini`

| PSP | Key |
|---|---|
| × ○ □ △ | S D A W |
| START, SELECT | Enter, Space |
| L, R | Q, E |
| d-pad | arrow keys |
| analog stick | I J K L |

</details>

<details>
<summary><b>Code layout</b> · app, dev, tools</summary>

| Location | Responsibility |
|---|---|
| `app/main.c` | Startup and the frame loop: the pad, the tabs and the cursor, the sync coming back, calls into `session/`, and the rig's hooks (scripted keys, screenshots, the film, the cipher benchmark) |
| `app/text.h` | Every word the screen shows, in one place |
| `app/gui/` | Browser, rendering, icons, previews and firmware keyboard |
| `app/gui/files_view.c` | Manage Data on screen: the two columns, the raw band, its keys, and the film or sound handed to the media thread |
| `app/session/view.c` | The browser's model, with nothing of the drawing in it: the tabs, the catalog filtered to the open one, the basket, what the action row would fetch, the gear's rows |
| `app/session/actions.c` | What the session does: installs, one or a run of them, removing, launching, fetching the catalog again, the sweep, the cache and the reset |
| `app/session/questions.c` | The questions that stand before those: put into words, and answered from the pad, X doing the thing and O leaving it |
| `app/session/options.c` | The one menu the shell draws: the package's options on triangle and the popups under the gear, their rows built and walked here |
| `app/update/` | Manifests, sources, catalogs, INBOX, media cache and synchronization |
| `app/install/` | ZIP reader, installation transactions and persistent app state |
| `app/network/` | The cipher benchmark the rig asks for |
| [`app/lib/pspkit-https/`](https://github.com/chriopter/pspkit-https) | HTTPS, the entropy pool and the sweep's stick step, as a submodule |
| `app/audio/`, `app/video/` | Audio and video playback |
| `app/util/` | Storage paths, PBP access and runtime helpers |
| `app/util/files.c` | What Manage Data reads off the stick: sources, installs, INBOX, the client's own files, filled into the view `gui/files_view.c` draws |
| `dev/tests/` | Host tests: the parsers, the records and the installer, with power cuts |
| `dev/` | Developing PSPDX: `start`, `release`, the mock catalog, the emulator settings, and the generators (`marks/`, `render-music.c`, `sweep-trace.py`) |
| `dev/rig` | The rig: one emulator run with scripted keys, leaving the log and screenshots |
| `dev/soak/` | Soak campaigns in the emulator, checked against a model of the client |
| `dev/localcat/` | The loopback catalog with its throwaway CA |
| `dev/testdata/` | Input traces for the entropy screen |
| `dev/tools/` | The EBOOT media scripts and the PSMF wrapper, for any PSP app's ICON1 and SND0 |

</details>

<details>
<summary><b>Tests</b> · host, emulator soak</summary>

#### Host tests

```sh
sh dev/tests/run
```

- Needs a C compiler, Python, cJSON, zlib and OpenSSL development files
- Runs the client code with address and undefined-behavior sanitizers
- A filesystem adapter simulates power cuts

#### Emulator soak

```sh
python3 dev/soak/run.py --runs 100 --seed 1
python3 dev/soak/run.py --perf 20
python3 dev/soak/run.py --edge 30
python3 dev/soak/scenarios.py --seed 1 --run 7   # print one input sequence
```

```text
mock catalog -> fixture build with a local CA -> scripted input in PPSSPP -> compare the stick with the model
```

- Needs Docker, the PPSSPP Flatpak and user systemd services
- Failures land under `dev/soak/results/`
- Fixture builds are for local testing only
- Neither replaces real PSP storage, WLAN and power-loss testing

</details>

<details>
<summary><b>Icons and EBOOT media</b> · glyphs, ICON1, SND0</summary>

From the repository root:

```sh
sh dev/marks/render.sh                                     # SVG -> PNG
python3 dev/marks/embed.py                                 # PNG -> app/gui/marks_data.h
sh dev/tools/eboot-media/make-icon1.sh demo.mp4 app/assets/icon1.pmf
sh dev/tools/eboot-media/make-snd0.sh theme.wav app/assets/snd0.at3
```

`dev/tools/` is for anyone packaging a PSP app, PSPDX included: the two scripts make the ICON1 and SND0 a PBP carries, and `dev/tools/mp4-to-psmf.c` wraps an MP4 the way the client does.

#### GUI glyphs

- Sources in `app/assets/marks/src/`
- Rendering needs `rsvg-convert` and ImageMagick; packing needs only Python
- SVGs, PNGs and the atlas header are committed, so normal builds need no graphics tools
- A new glyph needs matching entries in the generator's `ORDER` and `enum mark`

#### EBOOT media

| | Video (`ICON1.PMF`) | Audio (`SND0.AT3`) |
|---|---|---|
| Needs | ffmpeg, C compiler | ffmpeg, Python, `atracdenc` |
| Default | 6 s at 144×80 | 18 s ATRAC3 at 132 kbps |
| Knobs | `DURATION`, `FPS` | `DURATION`, `BITRATE` 132 or 66 |

Both scripts take a start offset as their third argument.

</details>

## Links

- [PSPDX Catalog](https://github.com/chriopter/pspdx-catalog) — the main catalog and its builder; [browse it](https://chriopter.github.io/pspdx-catalog/)
- [Demo app](https://github.com/chriopter/pspdx-demo) — a complete homebrew with a `.pspdx`
- [Releases](https://github.com/chriopter/pspdx/releases/latest) — `pspdx.zip`

**Feedback is very welcome!**
