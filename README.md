# PSPDX

**PSP Download Index** — browse, install and update homebrew directly
on your PlayStation Portable. **[→ Download](https://github.com/chriopter/pspdx/releases/latest)**

Install it, start it, install homebrew. Updates show up on their own.

<img width="480" alt="PSPDX starting, browsing the catalog, installing an update and two apps from the basket" src="images/pspdx-app.webp" />

[The same as a video](images/pspdx-app.mp4) (480×272, 34 s).

## Getting started

You need a PSP running [ARK-5](https://github.com/PSP-Arkfive/ARK-5) with
WPA2 support and a saved Wi-Fi connection. PSPDX uses the first saved profile.

1. Download [`pspdx.zip`](https://github.com/chriopter/pspdx/releases/latest)
   and extract it to the root of your Memory Stick or internal storage.
2. Enable WLAN, launch PSPDX from **Game**, and follow the first-launch prompt.
3. Choose an app in the catalog and press **×** to install it. Press **START**
   to run it.

PSPDX checks for updates at startup. Select an available update and press
**×** to install it.

## How it works

- **A `.pspdx` file points to your homebrew.** It sits in the app's GitHub
  repository and names the app and where it installs; releases supply the
  ZIP. That is the [`.pspdx` standard](#the-pspdx-standard), and the
  [demo app](https://github.com/chriopter/pspdx-demo) is a complete example.
- **A catalog is a phonebook of such repositories.** The
  [PSPDX Catalog](https://github.com/chriopter/pspdx-catalog) is the default;
  [browse it](https://chriopter.github.io/pspdx-catalog/). Its GitHub
  workflows cache release details and artwork extracted from each EBOOT, so
  lists load fast. Copy it to run your own.
- **No mirror.** PSPDX saves each app's `.pspdx` on install and can check the
  original repository directly. If a catalog goes away, your apps still
  update. `.pspdx` files can also be installed directly, without any catalog.

The aim is a setup that still works in ten years, without a mirror going
down or an abandoned installer in the way. The `.pspdx` file lives on its own.

**Add your app:** put a `.pspdx` in your repository
([example](https://github.com/chriopter/pspdx-demo/blob/master/.pspdx)) and
publish a release. Open-source apps can be added to the
[main catalog](https://github.com/chriopter/pspdx-catalog); or start your
own catalog and share the link.

## The PSPDX standard

Add a `.pspdx` to your repository to make your homebrew installable and
updatable through PSPDX; the [demo app](https://github.com/chriopter/pspdx-demo)
is a complete example.

```json
{
  "schema":     "https://github.com/chriopter/pspdx/blob/master/schema/v1.pspdx",
  "source":     "https://github.com/chriopter/psp-cathedral",
  "name":       "Lux Aeterna",
  "author":     "chriopter",
  "summary":    "Ten churches, and the sun through their glass.",
  "category":   "demo",
  "license":    "BSD-3-Clause",
  "installdir": "PSP/GAME/Cathedral"
}
```

<details>
<summary>Format specification (fields, releases, catalogs)</summary>

Version 1: a GitHub repository with a root `.pspdx` and a published release
containing exactly one ZIP with one `EBOOT.PBP`. Releases supply versions
and downloads; EBOOTs supply media. No manifest edit is needed per release.

[Manifest schema](schema/v1.pspdx) · [Catalog schema](schema/catalog-v1.json).
The manifest's `schema` identifies its version; unknown fields are rejected.

| Field | Rule | Default if omitted |
|---|---|---|
| `schema` | Required; the exact v1 schema URL | — |
| `source` | Required; an HTTPS GitHub repository URL | — |
| `name` | Required; 1–39 characters | — |
| `category` | Required; `game`, `emulator`, `app`, `plugin` or `demo` | — |
| `installdir` | Required; `PSP/GAME/` followed by 1–32 letters, digits, dots, underscores or hyphens; not `.`, `..` or `.pspdx-stage` | — |
| `author` | Up to 39 characters | Repository owner |
| `summary` | Up to 60 characters | Repository description |
| `license` | SPDX identifier, up to 64 characters | Repository license metadata |

Version 1 installs only under `PSP/GAME/`. A plugin requiring `seplugins`
and changes to `plugins.txt` needs a different installation contract.
A future incompatible manifest format gets a new schema URL.

Release information is derived, never duplicated in the manifest:

| Value | Source |
|---|---|
| `id` | `io.github.<owner>.<repo>`; owner and repository lowercased and stripped to `[a-z0-9]`. A later entry with a colliding identity is dropped. |
| `version` | Release tag with its leading `v` removed |
| `rev` | Release `published_at`, in Unix seconds; a higher value signals an update |
| Download URL and size | The release's single ZIP asset |
| SHA-256 | The downloaded ZIP, hashed by the catalog builder |
| Package contents | The directory containing the ZIP's single `EBOOT.PBP`, including its files and subdirectories |
| Media | `ICON0.PNG`, `PIC1.PNG`, `ICON1.PMF` and `SND0.AT3` inside the EBOOT; all optional |

The default source lookup uses GitHub's latest published, non-prerelease
release. A text list can pin a repository to a release tag:

```text
cache https://chriopter.github.io/pspdx-catalog/catalog.json
https://github.com/chriopter/pspdx-demo
https://github.com/someone/project@v1.2
```

Each list contains one repository URL per line. Its optional `cache` line
names an aggregated catalog. `catalog.json` contains `schema`, `generated_at`
and an `apps` array. Each app carries `id`, `source`, `name`, `author`,
`summary`, `category`, `license`, `installdir` and `release`: the original
`tag`, `published_at` and a `download` object with `url`, `size` and `sha256`.
Optional `media` holds `icon`, `screenshot`, `video` and `sound` URLs.
Relative media URLs resolve against the catalog URL. `generated_at` is the
snapshot publication time, even if individual apps failed to build. The
client tolerates entries without `author`, `summary`, `license` or `sha256`.

The [catalog schema](schema/catalog-v1.json) uses the identifier
`https://github.com/chriopter/pspdx/blob/master/schema/catalog-v1.json`.

The reference builder checks releases hourly, reuses unchanged entries and
publishes a fresh snapshot each hour. Manifest-only edits require a new
release or a forced catalog rebuild. Anyone can reuse the builder or publish
a different catalog; the client retains installed apps independently.

</details>

## Under the hood

<details>
<summary>Sources (catalogs, repositories, INBOX)</summary>

#### Where apps come from

| Source | Added through | Result |
|---|---|---|
| Catalog site URL, `catalog.json` or text repository list | **Add sources** | Browse all listed apps |
| GitHub URL or `owner/repo` | **Direct install** | Add the repository as a source and install its app |
| `.pspdx` files in `PSP/PSPDX/INBOX/` | **Direct install** | Validate and install the selected files |

- Default source: `https://chriopter.github.io/pspdx-catalog/`
- Sources are validated before they land in `PSP/PSPDX/sources.txt`
- Adding a catalog installs nothing; Direct install and INBOX do
- Removing a source keeps installed apps, their manifests and state

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
<summary>Install and update (checks, recovery)</summary>

#### Install

```text
x on app -> confirm -> verify .pspdx source + installdir
         -> download release ZIP -> check size, SHA-256, ZIP, paths
         -> stage -> swap into PSP/GAME/<dir> -> write state
```

- Only the folder holding the single `EBOOT.PBP` is installed
- An unmanaged folder in the way is never adopted or overwritten; PSPDX offers to move it to `<dir>.bak`
- A transaction journal covers files, manifest and state; an interrupted install recovers at the next start

#### Check

Runs at startup and on **Check for updates**. An update is a newer
`published_at` than the installed one. Version strings are not compared,
except for PSPDX's own first-start record, which has no timestamp yet.

```text
catalog entry usable?  -> yes: use it, skip GitHub
                       -> no:  ask the app's saved GitHub source
GitHub unavailable too -> keep the last known release, record no check
```

| Override | Effect |
|---|---|
| **□** on **Check for updates** | Ask GitHub for every installed app, bypassing catalog data |
| **△ → Updates: original source** on one app | Always ask that app's repository; **Updates: catalog first** restores the default. PSPDX itself starts in direct mode |

A reachable catalog counts as current even if its publisher stopped updating
it. PSPDX says so once the catalog is a day old; press **□** then.

#### Apply

- Checking never installs; confirm an update with **×**
- Batches: hold **○** to stop between packages; INBOX skips conflicts, only successful imports leave INBOX
- Self-updates run last; restart PSPDX afterward

#### Keys

**×** install, confirm · **△** options · **○** back · **□** basket · **START** run · **L/R** or **←/→** tabs

</details>

<details>
<summary>Network (HTTPS, TLS 1.3, offline)</summary>

#### Requests

```text
catalog   ->  one request, release data for every app
direct    ->  raw.githubusercontent.com   .pspdx
          ->  api.github.com              latest release
                                          + repository metadata when author, summary or license are missing
download  ->  release ZIP, following GitHub asset redirects
```

- First saved PSP network profile
- Everything over HTTPS
- TLS connections to the same host are briefly reused; a closed or expired one is reopened

#### TLS

- wolfSSL, TLS 1.3 only, X25519 preferred
- Bundled CAs, certificate-chain and hostname checks
- Certificate dates are not checked: the PSP clock cannot be trusted
- Entropy: analog-stick sweeps and `CRYPTO/seed.bin`
- Packages are not signed; a catalog SHA-256 checks ZIP integrity, not author identity

#### Offline

- Refresh runs in the background; installing pauses network previews, they share the network stack
- Cached browsing works offline; release checks and downloads need a connection

</details>

<details>
<summary>Catalog (updates, previews, cache)</summary>

#### What the builder does

```text
hourly:  repos.txt -> catalog.txt
         new release?  -> read .pspdx -> hash ZIP -> extract EBOOT media
         unchanged?    -> reuse the entry
         -> publish catalog.json
```

- `catalog.json` carries metadata, source, install path, release timestamp, ZIP URL, size, hash and media URLs
- A push or manual run also picks up manifest-only edits; hourly runs wait for a release
- A build with no valid apps leaves the live site in place

#### How an update shows

- PSPDX compares the catalog's release timestamp with `installed.published_at`
- The catalog does not know what is on your PSP
- The update appears after the catalog publishes and the PSP refreshes; the ZIP still comes from the author's release

#### Previews

- The catalog hosts `ICON0.PNG`, `PIC1.PNG`, `ICON1.PMF` and `SND0.AT3` separately: no ZIP download for a preview
- Priority: installed EBOOT media -> cached or catalog media -> placeholder
- Direct GitHub lookups fetch no new previews

#### Cache

- Catalogs in `PSP/PSPDX/CACHE/catalogs/`, media in `CACHE/media/`
- Failed fetch: the last usable catalog stays for browsing; installed apps ask their saved GitHub source
- Offline: saved records and media only
- Both caches can be deleted without losing installation state

</details>

<details>
<summary>Memory Stick (manifests, state, cache)</summary>

#### Layout

Example on the startup device (`ms0:` or `ef0:`); app IDs are illustrative.
Temporary and debug files appear only when used.

```text
ms0:/
└── PSP/
    ├── GAME/
    │   ├── PSPDX/                       # Downloader
    │   │   ├── EBOOT.PBP
    │   │   └── .pspdx                   # Bundled for offline first start
    │   ├── Cathedral/                   # Installed homebrew
    │   │   ├── EBOOT.PBP
    │   │   └── ...
    │   ├── .pspdx-stage/                # Temporary install
    │   ├── Cathedral.old/               # Rollback backup
    │   └── Cathedral.bak/               # Unmanaged folder moved aside
    └── PSPDX/
        ├── sources.txt                  # Subscriptions
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
        │   ├── http.txt
        │   └── wolf.log
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
| `installed` | Version, `published_at`, install directory |
| `latest` | Version, `published_at`, download URL, size, optional SHA-256, last successful check time and source |
| `update_check` | `auto` (catalog first) or `source` (GitHub first); PSPDX itself defaults to `source` |

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
<summary>Build and run (Docker, PPSSPP)</summary>

#### Build

Inside the `pspdev/pspdev:latest` container, or with `$PSPDEV`, cmake and
wget on the host:

```sh
sh app/wolfssl-psp/build.sh
make -C app
```

- Output: `app/EBOOT.PBP`
- Libraries: wolfSSL, cJSON, intraFont, libpng, zlib, PSP SDK
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
<summary>Code layout</summary>

| Location | Responsibility |
|---|---|
| `app/main.c` | Input, app actions and confirmation flow |
| `app/gui/` | Browser, rendering, icons, previews and firmware keyboard |
| `app/update/` | Manifests, sources, catalogs, INBOX, media cache and synchronization |
| `app/install/` | ZIP reader, installation transactions and persistent app state |
| `app/network/` | HTTPS and network diagnostics |
| `app/logic/` | Entropy pool |
| `app/audio/`, `app/video/` | Audio and video playback |
| `app/util/` | Storage paths, PBP access and runtime helpers |
| `app/ca-extra/` | Extra root certificates folded into the CA bundle |
| `app/testdata/` | Input traces for the entropy screen |
| `dev/`, `app/tools/` | Local builds, emulator fixtures and asset generators |

</details>

<details>
<summary>Tests (host, emulator soak)</summary>

#### Host tests

```sh
sh app/tests/run
```

- Needs a C compiler, Python, cJSON, zlib and OpenSSL development files
- Runs the client code with address and undefined-behavior sanitizers
- A filesystem adapter simulates power cuts

#### Emulator soak

```sh
python3 app/tools/soak/run.py --runs 100 --seed 1
python3 app/tools/soak/run.py --perf 20
python3 app/tools/soak/run.py --edge 30
python3 app/tools/soak/scenarios.py --seed 1 --run 7   # print one input sequence
```

```text
mock catalog -> fixture build with a local CA -> scripted input in PPSSPP -> compare the stick with the model
```

- Needs Docker, the PPSSPP Flatpak and user systemd services
- Failures land under `app/tools/soak/results/`
- Fixture builds are for local testing only
- Neither replaces real PSP storage, WLAN and power-loss testing

</details>

<details>
<summary>Icons and EBOOT media (assets)</summary>

From `app/`:

```sh
sh tools/marks/render.sh                                   # SVG -> PNG
python3 tools/marks/embed.py                               # PNG -> gui/marks_data.h
sh tools/eboot-media/make-icon1.sh demo.mp4 assets/icon1.pmf
sh tools/eboot-media/make-snd0.sh theme.wav assets/snd0.at3
```

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
