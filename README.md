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

## In detail

<details>
<summary>Sources — catalogs, repositories and INBOX</summary>

The default source is `https://chriopter.github.io/pspdx-catalog/`. PSPDX
reads `catalog.json` for fast browsing. If it fails, PSPDX reads `catalog.txt`
from the same site and checks its listed repositories directly. If both fail,
it uses a saved catalog when available. A site with only `catalog.txt` also
works without a builder. Settings accepts additional sources:

| Input | Result |
|---|---|
| HTTPS catalog site URL, `catalog.json` or text repository list | Browse all listed apps. |
| GitHub URL or `owner/repo` under **Direct install** | Add the repository as a source and install its app. |
| `.pspdx` files in `PSP/PSPDX/INBOX/` | Validate and install selected files. |

Catalogs and repositories are validated before being added to `sources.txt`.
Adding a catalog does not install its apps; Direct install and INBOX import
do. Installed apps keep their own manifests and state if a source is removed.

</details>

<details>
<summary>Installation and updates — checks, sources and recovery</summary>

**Install:** select an app with **×** and confirm. PSPDX verifies that its
`.pspdx` source and `installdir` match the selected entry, then downloads the
author's release ZIP. It checks size, available SHA-256, ZIP integrity and
paths before installing the folder containing the single `EBOOT.PBP`.
An unmanaged folder in the way is never adopted or overwritten; PSPDX offers
to move it to `<dir>.bak` first.

**Check:** at startup, or through **Check for updates** in settings, PSPDX
compares each installed release's `published_at` with the available release.
Version strings are not used for update detection, except for PSPDX's own
first-start record, which has no timestamp yet.

| Case | Release lookup |
|---|---|
| A configured catalog has a usable entry | Use it; skip the GitHub release lookup. |
| No usable entry, including an unreachable or invalid catalog | Check the app's saved GitHub source; cached catalog data can still be browsed. |
| App added through INBOX or a repository | Check its saved GitHub source unless a configured catalog now covers it. |
| **□** on **Check for updates** | Query GitHub for installed apps, bypassing catalog release data. |
| **△ → Updates: original source** on one app | Always check that app's GitHub repository directly; **Updates: catalog first** restores the default. PSPDX itself starts in direct mode. |
| GitHub also unavailable | Keep the last known release; do not record a successful check. |

A reachable catalog is treated as current even if its publisher stopped
updating it. PSPDX says so when the catalog is a day old; press **□** on
**Check for updates** then. An old cached entry is not evidence of a current
release.

**Apply:** checking never installs an update. Confirm one with **×**. A
transaction journal covers the installed files, manifest and state so an
interrupted install can recover. INBOX batches skip conflicts; only successful
imports leave INBOX. Hold **○** to stop between packages. Self-updates run last;
restart PSPDX afterward.

**△** options · **○** back · **□** basket · **START** run · **L/R** or **←/→** tabs.

</details>

<details>
<summary>Network — HTTPS, TLS 1.3 and offline use</summary>

PSPDX uses the first saved PSP network profile. Catalogs, GitHub checks and
ZIP downloads use HTTPS. A catalog returns release data for many apps in one
request; direct checks read `.pspdx` from `raw.githubusercontent.com` and
releases from `api.github.com`. Missing author, summary or license fields may
require a repository metadata request. ZIPs follow GitHub asset redirects.
PSPDX briefly reuses TLS connections to the same host, including GitHub's
Raw and API hosts; a closed or expired connection is reopened automatically.

wolfSSL uses TLS 1.3, bundled CAs, certificate-chain and hostname checks,
with X25519 preferred. Analog-stick entropy and `CRYPTO/seed.bin` feed its
random generator. Certificate dates are not checked, because the PSP clock
cannot be trusted; the other checks remain. Packages are not signed. A
catalog SHA-256 checks ZIP integrity, not author identity.

Refresh runs in the background. Installation pauses network previews because
they share the network stack. Cached browsing works offline; fresh release
checks and downloads require a connection.

</details>

<details>
<summary>Catalog data — update indicators, previews and cache</summary>

The reference catalog's hourly workflow reads its `repos.txt` and publishes
it as `catalog.txt`. For a new release it reads `.pspdx`, hashes the ZIP and
extracts valid EBOOT media; unchanged releases reuse their entries. It
publishes `catalog.json` with app
metadata, source, install path, release timestamp, ZIP URL, size, hash and
media URLs. A push or manual run also picks up manifest-only edits; ordinary
hourly runs wait for a new release. A build with no valid apps leaves the live
site in place.

PSPDX compares each catalog release timestamp with the local app's
`installed.published_at` to mark updates. The catalog does not know what is
installed on your PSP. New updates appear after the catalog publishes and the
PSP refreshes; ZIPs still download from the author's release.

The catalog hosts optional `ICON0.PNG`, `PIC1.PNG`, `ICON1.PMF` and `SND0.AT3`
separately, avoiding a ZIP download for previews. Installed EBOOT media takes
priority, then cached/catalog media. Direct GitHub lookups use installed media,
existing cache or placeholders; they do not fetch new previews.

Responses live in `PSP/PSPDX/CACHE/catalogs/`, media in `CACHE/media/`. A failed
fetch keeps the last usable catalog for browsing; installed apps check their
saved GitHub source. Offline, only saved records and media are available. Both
caches can be deleted without losing installation state.

</details>

<details>
<summary>Files on the Memory Stick — manifests, state and cache</summary>

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

`INSTALLED/<app-id>.pspdx` preserves the source independently of
`sources.txt`. Its adjacent `<app-id>.state.json` has no outer app-ID key:

| Field | Contents |
|---|---|
| `source`, `added_from` | Original repository and import route. |
| `installed` | Version, `published_at`, install directory. |
| `latest` | Version, `published_at`, download URL, size, optional SHA-256, last successful check time and source. |
| `update_check` | Per-app lookup choice: `auto` (catalog first) or `source` (GitHub first). PSPDX defaults to `source`. |

Install creates the state file. Successful install/update writes `installed`;
a successful catalog/GitHub check writes `latest` without changing
`installed`. Thus `installed.version = 1.0` and `latest.version = 1.1` means
1.0 is still installed. Browsing creates no installation record. PSPDX
registers its own running version at startup and copies its bundled `.pspdx`
without an internet request. `added_from` does not determine
future update checks; each operation writes only the affected app's state.

Recoverable writes may leave `.new` or `.bak` files. Staging and backup stay
under `GAME/` because PSP directory renames require the same parent. Keep
`INSTALLED/` for updates; `CACHE/` is disposable. Do not delete a pending
transaction's files. Only per-app state files are loaded; older combined
state files are ignored. Corrupt app records are preserved and block writes.

</details>

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
<summary>Format specification — fields, releases and catalogs</summary>

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

## Development

<details>
<summary>Build, code layout and tests</summary>

Inside the `pspdev/pspdev:latest` container, or with `$PSPDEV`, cmake and
wget on the host:

```sh
sh app/wolfssl-psp/build.sh
make -C app
```

The build produces `app/EBOOT.PBP`. It uses wolfSSL, cJSON, intraFont, libpng,
zlib and PSP SDK libraries; CI builds inside `pspdev/pspdev:latest`.
`dev/start` builds and launches the client in the configured PPSSPP setup;
`dev/start --no-build` launches an existing build unless that build was made
against the mock catalog, which is rebuilt regardless. `dev/start --mock`
runs against the local mock catalog. These scripts assume Linux with Docker,
the PPSSPP Flatpak, Python, a systemd user session and Wayland.
`dev/release <version> [notes]` builds and publishes a GitHub release with
`gh` from a clean `master` checkout.

`dev/start` installs `dev/ppsspp/controls.ini`: × S, ○ D, □ A, △ W,
START Enter, SELECT Space, L Q, R E; arrow keys for the d-pad, I/J/K/L
for the analog stick.

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

```sh
sh app/tests/run
python3 app/tools/soak/run.py --runs 100 --seed 1
python3 app/tools/soak/run.py --perf 20
python3 app/tools/soak/run.py --edge 30
```

The host tests need a C compiler, Python, cJSON, zlib and OpenSSL development
files. They run the client code with address/undefined-behavior sanitizers
and a filesystem adapter that supports simulated power cuts.

The emulator soak tools use the local mock catalog, Docker, PPSSPP Flatpak
and user systemd services. They build a fixture-enabled client with a local
CA, run scripted inputs and compare the resulting installations with a
model. Failure artifacts are written under `app/tools/soak/results/`.
`scenarios.py --seed 1 --run 7` prints a reproducible input sequence.
Fixture builds are for local testing only. Host and emulator tests do not
replace real PSP storage, WLAN and power-loss testing.

</details>

<details>
<summary>Regenerating icons and EBOOT media</summary>

From `app/`:

```sh
sh tools/marks/render.sh
python3 tools/marks/embed.py
sh tools/eboot-media/make-icon1.sh demo.mp4 assets/icon1.pmf
sh tools/eboot-media/make-snd0.sh theme.wav assets/snd0.at3
```

The GUI glyph sources are in `app/assets/marks/src/`. Rendering needs
`rsvg-convert` and ImageMagick; packing the PNGs into `gui/marks_data.h`
needs only Python. SVGs, generated PNGs and the atlas header are committed,
so normal builds need no graphics-generation tools. A new glyph also needs
matching entries in the generator's `ORDER` and `enum mark`.

EBOOT media generation needs ffmpeg. Video also needs a C compiler; audio
also needs Python and `atracdenc`. Both scripts accept a start offset as
their third argument; `DURATION` controls length. Video defaults to six
seconds at 144×80, `FPS` adjustable; audio defaults to eighteen seconds of
ATRAC3 at 132 kbps, `BITRATE` 132 or 66.

</details>
