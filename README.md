# PSPDX

**PSP Download Index**

Install and update homebrew directly on your PlayStation Portable, using the [PSPDX standard](https://chriopter.github.io/pspdx/).

**[→ Download PSPDX](https://github.com/chriopter/pspdx-app/releases/latest)**

<img width="480" alt="PSPDX starting, browsing the catalog, installing an update and two apps from the basket" src="assets/pspdx-app.webp" />

## How to use

Just install, start and search for homebrew! Updates are shown automatically. Needs ARK-5 for WPA2.

Issues? [Tell us how it went](https://github.com/chriopter/pspdx-app/issues).

## How it works

- **Catalogs:** PSPDX comes with the main catalog and a community list. Add your own in the gear tab.
- **Updates:** at start, PSPDX fetches the catalogs and asks GitHub about any app no catalog lists.
- **Your files stay:** an update replaces only the files the release ships; saves and settings next to the EBOOT are kept.
- **Memory:** every installed app's `.pspdx` stays on the Memory Stick.

## Under the hood

<details>
<summary><b>The PSPDX standard</b> · ids, updates, no .pspdx</summary>

PSPDX reads the [PSPDX standard](https://chriopter.github.io/pspdx/): a `.pspdx` next to each app and `catalog.json` lists.

- **Catalogs:** one `catalog.json` request lists every app and shows which have updates.
- **Downloads:** the ZIP comes straight from each app's release, never from the catalog.
- **Without a catalog:** the saved `.pspdx` of an installed app leads back to its repository.

What PSPDX does on top:

- **Ids:** GitHub → `io.github.<owner>.<repo>`; elsewhere the reversed host of `source` plus the name. A catalog's own `id` is kept only when it is well-formed and doesn't clash with an installed app. An installed app keeps the id of its record, whatever a list calls it later; one repository is one row.
- **Folder:** without `installdir` → `PSP/GAME/<repository name>`, cut to 32 characters. An installed app keeps its folder: a list naming another moves nothing, a folder you renamed (record too) stays, and only the author's `.pspdx` naming another folder moves it, with everything in it.
- **Updates:** the SHA-256 of `releases[0]` differs from the installed ZIP's. Versions and dates are not compared.
- **ZIP rule:** one `.zip` on the release, or of several exactly the one with `psp` in its name; otherwise the app is left out with the reason.
- **Pinned release:** `"release": {"tag": "v1.2"}` installs exactly that release, with no updates until the file changes.
- **No `.pspdx` at the source:** GitHub's own 404 (not one after a redirect elsewhere). PSPDX installs from the catalog entry, your INBOX file or, typed into Direct install, the repository's name. A live catalog keeps it updated; without one the latest release is asked at GitHub by the saved `.pspdx`, same ZIP rule and SHA-256. As soon as the author adds a `.pspdx`, that file counts.
- **Catalog text:** CR LF and tabs in `description` and `summary` are read as meant; an entry with a NUL in its text is dropped, and a `sha256` of zeros is no hash.
- **Types:** `homebrew` installs under `PSP/GAME/`; `plugin` and `iso` are listed, not installed yet.
- **`schema` field:** a `catalog.json` is read as v1 unless it names another PSPDX catalog version (`catalog-v2.json`); none, a relative path to a copy, or any other value is read as v1 and noted in the log.

#### Text list

```text
cache https://chriopter.github.io/pspdx-catalog/catalog.json
https://github.com/chriopter/pspdx-demo
https://github.com/someone/project@v1.2
```

- One repository URL per line; `@tag` pins a release
- `cache` names a catalog, read first; repositories it doesn't cover are asked directly

#### Checking a `.pspdx` in VS Code

```json
"files.associations": { "*.pspdx": "json" },
"json.schemas": [{ "fileMatch": ["*.pspdx"],
  "url": "https://chriopter.github.io/pspdx/schema/pspdx-v1.json" }]
```

</details>

<details>
<summary><b>Sources</b> · catalogs, repositories, INBOX</summary>

#### Where apps come from

| Source | Added through | Result |
|---|---|---|
| Catalog site URL, `catalog.json` or text list | **Manage sources** | Browse all listed apps |
| GitHub URL or `owner/repo` | **Direct install** | Add the repository as a source and install its app; without a `.pspdx` it installs from its latest release, named after the repository |
| `.pspdx` files in `PSP/PSPDX/INBOX/` | **Direct install** | Validate and install the selected files; for a repository without its own `.pspdx` your file is the app |

- Preset sources, in this order: `https://chriopter.github.io/pspdx-catalog/`, `https://wijsman.de/psp-homebrew-database/`
- Presets ship as `PSP/GAME/PSPDX/presets.txt`, same lines as `sources.txt`; the EBOOT carries a copy for a stick without one
- Each preset lands once and is noted in `PSP/PSPDX/presets.seen`: removed stays removed, a new one in an update arrives
- **Manage sources** lists each source with its address, kind, apps and when it last loaded; one that does not load is marked *unreachable*, one served only from its saved copy *offline copy*; the rest load as usual
- A line ending in `.pspdx`, left from an older version, is skipped with a line in the log
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

```mermaid
flowchart TD
  pick(["× on an app, an INBOX file, or Direct install"]) --> github{"Source on GitHub?"}
  github -- "no" --> outside["Only from a catalog entry<br>the ZIP must match its SHA-256<br>an INBOX file waits"]
  github -- "yes" --> pspdx{".pspdx at<br>raw.githubusercontent.com"}
  pspdx -- "200" --> own["The repository's own .pspdx wins"]
  pspdx -- "GitHub's 404" --> stand["The catalog entry, your INBOX file,<br>or the repository name from Direct install"]
  pspdx -- "no answer, 404 elsewhere" --> later(["Not installed, try again later"])
  own --> release{"Release: the catalog's,<br>a pinned tag, or GitHub's latest"}
  stand --> release
  release -- "none, rate limit, 5xx" --> norelease(["No release to install,<br>or GitHub didn't answer"])
  release -- "found" --> folder{"PSP/GAME/#lt;dir#gt; free<br>or already this app's?"}
  outside --> folder
  folder -- "another folder in the way" --> bak(["Offer to move it to #lt;dir#gt;.bak"])
  folder -- "yes" --> room{"Room for the ZIP<br>and for unpacking?"}
  room -- "no" --> space(["Status line: how many MB are needed"])
  room -- "yes" --> check{"Size, SHA-256,<br>ZIP and paths ok?"}
  check -- "no" --> refused(["Refused, the stick is untouched"])
  check -- "yes" --> installed{"Already installed?"}
  installed -- "no" --> fresh["Stage, then rename into PSP/GAME/#lt;dir#gt;"]
  installed -- "yes" --> overlay["Swap each shipped file,<br>your other files stay"]
  fresh --> state(["Write the record and keep the .pspdx"])
  overlay --> state
```

- Only the folder holding the single `EBOOT.PBP` is installed
- **Updates keep your files:** files the ZIP ships are replaced or added, everything else in the folder stays; files an older version shipped and the new one doesn't are left over. **Delete** removes the whole folder
- An author who moves `installdir` moves the folder with your files in it, then the release is laid over it
- Installed from a catalog entry, the saved `.pspdx` is the entry's words; from outside GitHub the ZIP must match the entry's SHA-256
- An unmanaged folder in the way is never adopted or overwritten; PSPDX offers to move it to `<dir>.bak`
- Not enough room: the status line says how many MB are needed, before anything is written
- A transaction journal covers every shipped file, manifest and state; a cut install or update recovers at the next start to the old or the new version
- A leftover that won't delete (a read-only file in `<dir>.old`) is retried at every start and blocks only that app; a folder deleted by hand installs fresh
- PSPDX never deletes the folder it runs from, whatever record names it

#### Check

Runs at startup and on **Check for updates**, the top row of the stick tab: **×** is the quick check, **□** the full one.
An update is a newest release whose SHA-256 differs from the installed ZIP's; versions and dates are not compared,
except for a record from before SHA-256 was kept.

```mermaid
flowchart TD
  start(["Start, or × Check for updates"]) --> fresh{"In a catalog that answered<br>and is under 24 h old?"}
  fresh -- "yes" --> entry["The catalog entry<br>no GitHub request"]
  fresh -- "no, or □ full check" --> github{"Source on GitHub?"}
  github -- "no" --> record["The catalog entry or the record<br>never asks anywhere else"]
  github -- "yes" --> due{"Asked in the last 6 h?"}
  due -- "yes, not □" --> saved["The last answer in the record"]
  due -- "no, or □" --> pspdx{".pspdx at<br>raw.githubusercontent.com"}
  pspdx -- "200" --> api["Latest release<br>api.github.com, 1 of 60 an hour"]
  pspdx -- "GitHub's 404" --> listed{"A live catalog lists it?"}
  listed -- "yes" --> entry
  listed -- "no" --> api
  pspdx -- "no answer, 404 elsewhere" --> saved
  api -- "rate limit, 5xx" --> backoff["Keep the record<br>asked again after 6 h"]
  api -- "release" --> zip["ZIP rule, GitHub's SHA-256"]
  entry --> differs
  record --> differs
  saved --> differs
  zip --> differs{"SHA-256 differs<br>from the installed ZIP?"}
  differs -- "yes" --> update(["Update to x.y"])
  differs -- "no" --> current(["Up to date"])
```

- A check against a maintained catalog is one request and no API call
- An app without `.pspdx` is asked by the saved one: GitHub's 404 on raw.githubusercontent.com, then the API
- A pinned release (`"release": {"tag": ...}`) is current until its file changes; the newest `.pspdx` GitHub answers with is saved
- A repository added by **Direct install** is a source: its `.pspdx` and one API call at every start; an app from INBOX is asked every six hours
- Rate limit or 5xx: the record stands, the check counts as asked, and **Direct install** says GitHub didn't answer
- Catalog older than 24 h: the status line names its host
- Nothing is topped up from GitHub: a `.pspdx` without author is by the account in its URL, a missing summary or licence stays empty

#### Apply

- Checking never installs; confirm an update with **×**
- **○** during download or unpack cancels the install and puts the stick back; in a batch it also stops the rest
- INBOX skips conflicts; only successful imports leave INBOX; up to 64 files that install are queued
- A pinned tag in an INBOX file must be the release's exactly (`0.1.3` is not `v0.1.3`); the repository's own `.pspdx` wins over your file
- For an installed app no catalog lists, PSPDX asks GitHub for exactly that tag (one API call)
- Self-updates run last; restart PSPDX afterward

#### Keys

**×** install, confirm · **△** options · **○** back · **□** basket · **START** run · **L/R** or **←/→** tabs

Tabs, as signs to the right, the open one named at the head of the list: **Memory Stick** installed, update arrows and a count while updates wait → **Homebrews** everything published → **UMDs** coming soon → **basket** while it holds something → | → **gear** settings at the right edge; Homebrew, the UMD and the gear keep their place, the stick and the basket hang off them and come and go

**△** → **Information**: version, author, licence, tags, size, id, last check, then summary and description; the **analog stick** scrolls, **○** back

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
- A catalog, `.pspdx` or API answer has 120 s in all; a response head 30 s
- Only a catalog asks for gzip, inflated as it arrives into a 512 KB buffer; `pspdx.log` says `fetch: <n> bytes gzipped, <m> inflated`. A ZIP is never asked for compressed
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
<summary><b>Catalog</b> · updates, previews, cache, memory</summary>

#### What the builder does

```text
hourly:  repos.txt -> catalog.txt
         new release?  -> read .pspdx -> hash ZIP: the 20 newest release ZIPs, each hashed once
                       -> extract EBOOT media from the newest
         unchanged?    -> reuse the entry
         -> publish catalog.json
```

- `catalog.json` carries metadata, tags, description, source, install path, up to the 20 newest releases (date, ZIP URL, size, hash, changelog) and media URLs; the console reads `releases[0]`
- The reference builder lists repos with their own `.pspdx`, and a repo without one from a file in its `listed/`
- A push or manual run also picks up manifest-only edits; hourly runs wait for a release
- A build with no valid apps leaves the live site in place

#### How an update shows

- PSPDX compares the SHA-256 of the catalog's newest release with the installed one
- The catalog does not know what is on your PSP
- The update appears after the catalog publishes and the PSP refreshes; the ZIP still comes from the author's release

#### Previews

- The catalog hosts `ICON0.PNG`, `PIC1.PNG`, `ICON1.PMF` and `SND0.AT3` separately: no ZIP download for a preview; of several screenshots the first is shown
- A picture over 512 px, up to 4096, is scaled down to fit, a row at a time
- Priority: installed EBOOT media -> cached or catalog media -> placeholder
- Direct GitHub lookups fetch no new previews

#### Cache

- Catalogs in `PSP/PSPDX/CACHE/catalogs/`, media in `CACHE/media/`
- Failed fetch: the last usable catalog stays for browsing; installed apps from GitHub ask their saved source, at most every six hours
- Offline: saved records and media only
- Both caches can be deleted without losing installation state

#### Memory

- Up to 64 apps in RAM; the catalog arrives in one 512 KB buffer
- A `.pspdx` (up to 16 KB) and a description sit on the heap, only for an app that has them
- **Information** breaks the description into lines once, when it opens, not every frame

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
    │   │   ├── EBOOT.PBP.pspdx-new      # During an update: the new file
    │   │   └── EBOOT.PBP.pspdx-old      # During an update: the old one
    │   ├── .pspdx-stage/                # Temporary first install
    │   ├── Cathedral.old/               # Folder being deleted
    │   └── Cathedral.bak/               # Unmanaged folder moved aside
    └── PSPDX/
        ├── sources.txt                  # Subscriptions
        ├── presets.seen                 # Presets already offered
        ├── INBOX/demo.pspdx             # Awaiting import
        ├── INSTALLED/
        │   ├── io.github.chriopter.pspdxapp.pspdx
        │   ├── io.github.chriopter.pspdxapp.state.json
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
        │   ├── transaction.json
        │   └── cleanup.txt              # Leftovers retried at start
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

`INSTALLED/<app-id>.pspdx` keeps the source independently of `sources.txt`;
for an app installed from a catalog entry, it is the entry's words.
`<app-id>.state.json` next to it has no outer app-ID key:

| Field | Contents |
|---|---|
| `source`, `added_from` | Original repository and import route |
| `installed` | Version, `published_at`, SHA-256, install directory, and `pspdx_installdir`: the folder the `.pspdx` named at install |
| `latest` | Version, `published_at`, download URL, size, SHA-256, last successful check time and source |
| `update_check`, `manifest_url` | Written by older versions; read and ignored |

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
- Staging stays under `GAME/` and an update's copies beside their files: PSP renames need the same parent
- Keep `INSTALLED/`; `CACHE/` is disposable; never delete a pending transaction's files
- Only per-app state files load; older combined state files are ignored
- Corrupt records are preserved and block writes; a NUL in a string or a SHA-256 of zeros makes a record corrupt
- A record PSPDX 0.5 kept of itself (`io.github.chriopter.pspdx`) is retired at start

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

- Needs a C compiler, Python, cJSON, zlib, OpenSSL and libpng development files
- Runs the client code and the PNG decoder with address and undefined-behavior sanitizers
- A filesystem adapter simulates power cuts, failed calls, a full or read-only stick and any network answer (`FAULT`, `FAILAT`, `STICK_BYTES`, `RO_MATCH`, `URL_MAP`)

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

- [PSPDX standard](https://github.com/chriopter/pspdx) — the `.pspdx` and `catalog.json` files PSPDX reads; [read the specification](https://chriopter.github.io/pspdx/)
- [PSPDX Catalog](https://github.com/chriopter/pspdx-catalog) — the main catalog: rebuilds itself every hour and fetches the latest releases; [browse it](https://chriopter.github.io/pspdx-catalog/)
- [Demo app](https://github.com/chriopter/pspdx-demo) — a minimal homebrew with its own `.pspdx`
- [Abandoned demo](https://github.com/chriopter/pspdx-demo-abandoned) — a homebrew without a `.pspdx`, listed by the catalog
- [Releases](https://github.com/chriopter/pspdx-app/releases/latest) — `pspdx.zip`

**Feedback is very welcome!**
