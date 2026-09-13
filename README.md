# PSPDX

**PSP Download Index** — browse, install, update homebrew directly
on your PlayStation Portable. **[→ Download](https://github.com/chriopter/pspdx/releases/latest)**

The [PSPDX Catalog](https://chriopter.github.io/pspdx-catalog/) comes ready to
browse like a store. You can add other catalogs or `.pspdx` files directly.
Each installed app keeps its source, so updates still work if a catalog goes
away.

- **[Download PSPDX](https://github.com/chriopter/pspdx/releases/latest)** — the PSP app.
- **[PSPDX Catalog](https://github.com/chriopter/pspdx-catalog)** — [browse homebrew](https://chriopter.github.io/pspdx-catalog/) or copy its workflows to build your own catalog.
- **[Demo app](https://github.com/chriopter/pspdx-demo)** — a complete example for authors using the [open `.pspdx` format](#the-pspdx-standard).

<img width="480" alt="PSPDX App browsing homebrew and available updates" src="images/pspdx-app.png" />

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

## The PSPDX client

How sources, installation, updates and local files work:

<details>
<summary>Browse Catalogs</summary>

The PSPDX Catalog is preconfigured. Add more sources in settings; their
apps appear together in the browser:

| Action | Input | What it adds |
|---|---|---|
| **Add catalog** | HTTPS `catalog.json` URL | A collection of apps and release data |
| **Add GitHub repository** | Repository URL or `owner/repo` | One app, resolved directly from GitHub |
| **Read INBOX** | `.pspdx` files in `PSP/PSPDX/INBOX/` | Apps to validate and install after confirmation |

Catalogs and repositories are checked before being saved in `sources.txt`.
Text repository lists are also supported. Adding a source does not install
its apps; INBOX import does. Installed apps remain tracked through their
saved manifests and state, even if their catalog is removed.

</details>

<details>
<summary>Install &amp; Update Brews</summary>

**Install:** select an app and press **×**, then confirm. PSPDX obtains its
`.pspdx` if not already loaded and checks that its source and installation
folder match the selected entry. It downloads the release ZIP from GitHub,
checks size, SHA-256 when available, ZIP integrity and paths, then installs
the directory containing the single `EBOOT.PBP` into `installdir`.
The manifest and installed release are saved together for future updates.
Unmanaged folders are neither adopted nor overwritten.

**Check updates:** PSPDX refreshes sources automatically at startup;
**Update catalog** in settings repeats the check. It compares each installed
release's publication time with the available release; a newer timestamp
marks an update, rather than comparing version strings.

| Situation | Where release information comes from |
|---|---|
| A configured catalog returns a usable entry for the app | Use that entry; no separate GitHub release lookup is needed. |
| The catalog is unreachable, invalid or no longer lists the installed app | Check the GitHub source saved with the installed app. Cached catalog data remains available for browsing. |
| The app came from INBOX or a repository, never a catalog | Check its saved GitHub source when no usable catalog entry covers it. If a configured catalog later lists it, that entry can be used too. |
| **Check original sources** is selected | Check installed apps directly at GitHub, bypassing catalog release information. |
| GitHub is also unavailable | Keep the last known release and successful check time. No successful update check is recorded. |

A successfully fetched catalog is treated as current; its age is not checked.
If it stays online but stops publishing new releases, use **Check original
sources**. Previously cached information is not proof that an app is current.

**Apply updates:** checking does not install anything. Confirm with **×** to
download the selected release. A transaction journal covers files, manifest
and state so interrupted operations can be recovered. For INBOX batches,
conflicts are skipped and only successful imports leave INBOX; hold **○**
to stop between packages. Self-updates run last; restart PSPDX to run the new version.

**△** options · **○** back · **□** basket · **START** run · **L/R** or **←/→** tabs.

</details>

<details>
<summary>Connect — TLS 1.3</summary>

PSPDX connects through the first saved PSP network profile.
Catalog lookup, direct GitHub checks and package downloads all use HTTPS.
One catalog request supplies release data for many apps. Direct checks read
`.pspdx` from `raw.githubusercontent.com` and release data from
`api.github.com`; missing author/description/license fields may require a
repository metadata request. ZIP downloads follow GitHub asset redirects.
Each HTTP request opens its own connection; there is no keep-alive or reuse.

wolfSSL provides TLS 1.3 with bundled CAs, certificate-chain and hostname
verification, and X25519 preferred. Analog-stick entropy and `CRYPTO/seed.bin`
feed the random generator. Certificate date checks are bypassed for unset
PSP clocks; the other checks remain. Packages have no signature verification.
SHA-256 supplied by a catalog checks package integrity, not the author's identity.

Refresh runs in the background. Installation and preview loading share the
network stack, so previews pause during installation. Cached browsing works
offline; fresh release checks and ZIP downloads require a connection.

</details>

<details>
<summary>Catalog — releases, previews and caching</summary>

The catalog does work ahead of the PSP: it collects many repositories into
one small index, so browsing and update checks need fewer requests.

1. The PSPDX Catalog workflow checks listed repositories hourly. For changed
   releases it reads `.pspdx`, downloads the ZIP, hashes it and extracts EBOOT
   media. Unchanged releases reuse their previous entries.
2. It publishes `catalog.json`: app descriptions, source repositories,
   installation paths, versions, publication times, ZIP URLs, sizes, hashes
   and media URLs. The same data feeds the website and app pages.
3. PSPDX fetches this index and compares release timestamps with each app's `.state.json`
   to display available updates. The catalog does not know what is installed
   on your PSP. An update appears after the catalog build and your next refresh;
   the ZIP still downloads from the author's release.

Manifest-only edits need a new release or forced rebuild. A push or manual
workflow run forces a full read. A build with no valid apps keeps the live site.

**Previews:** the workflow hosts extracted `ICON0.PNG` (icon), `PIC1.PNG`
(background), `ICON1.PMF` (video) and `SND0.AT3` (sound) separately. The PSP
loads icons and selected-app previews without downloading whole app ZIPs.
Installed EBOOT media takes priority, followed by cache/catalog media.
Direct GitHub lookups fetch no separate previews: they use installed EBOOTs,
existing cache or placeholders. All media is optional.

**Caching:** catalog responses go to `CACHE/catalogs/`, media to `CACHE/media/`
under `PSP/PSPDX/`. A failed catalog request preserves its last usable copy;
installed apps fall back to their source as described under Install & Update.
Offline, only saved records and local media are available. Either cache can
be deleted without losing installation records.

</details>

<details>
<summary>Data Structure</summary>

Example on the startup device (`ms0:` or `ef0:`). App names and hashes are
illustrative; temporary and debug files appear only when used.

```text
ms0:/
└── PSP/
    ├── GAME/
    │   ├── PSPDX/
    │   │   └── EBOOT.PBP                  # The downloader
    │   ├── Cathedral/
    │   │   ├── EBOOT.PBP                  # Installed homebrew
    │   │   └── ...                        # Other files from its package
    │   ├── .pspdx-stage/                  # Temporary installation
    │   │   └── EBOOT.PBP
    │   └── Cathedral.old/                 # Temporary rollback backup
    │       └── EBOOT.PBP
    └── PSPDX/
        ├── sources.txt                   # Catalogs and direct repositories
        ├── INBOX/
        │   └── demo.pspdx                 # Awaiting import
        ├── INSTALLED/
        │   ├── io.github.chriopter.pspdx.pspdx
        │   ├── io.github.chriopter.pspdx.state.json
        │   ├── io.github.chriopter.pspcathedral.pspdx
        │   └── io.github.chriopter.pspcathedral.state.json
        ├── CACHE/
        │   ├── catalogs/
        │   │   └── <url-sha1>.json        # Cached catalog response
        │   └── media/
        │       ├── <app-id>-icon-<hash>.png
        │       ├── <app-id>-picture-<hash>.png
        │       ├── <app-id>-film-<hash>.pmf
        │       └── <app-id>-sound-<hash>.at3
        ├── TMP/
        │   ├── download.zip              # Package being installed
        │   └── transaction.json          # Recovery journal
        ├── LOGS/
        │   ├── pspdx.log                 # App log
        │   ├── http.txt                  # HTTP diagnostics
        │   └── wolf.log                  # TLS diagnostics
        ├── DEBUG/
        │   ├── PSPDX.BMP                 # Screenshot
        │   ├── PSPDX.KEYS                # Scripted test inputs
        │   ├── PSPDX.REPLAY               # Entropy replay
        │   ├── PSPDX.TRACE                # Entropy trace
        │   ├── PSPDX_REC/                # Entropy recordings
        │   ├── font/
        │   │   └── ltn8.pgf              # Optional test font
        │   └── ...                       # Other test markers/screenshots
        └── CRYPTO/
            └── seed.bin                  # Saved entropy seed
```

`sources.txt` stores subscriptions; `INBOX/` holds files awaiting import.
`INSTALLED/<app-id>.pspdx` preserves each installed app's source independently
of those subscriptions. Beside it, `<app-id>.state.json` contains only that
app's state, without an outer app-ID key:

| State | Stored fields |
|---|---|
| Identity | `source`, `added_from` (catalog, repository or INBOX) |
| `installed` | `version`, `published_at`, `installdir` |
| `latest` | `version`, `published_at`, `download_url`, `size`, optional `sha256`, `checked_at`, `checked_from` |

Installing an app creates its state file. Successful installs and updates
write `installed`; successful catalog/GitHub checks write `latest` without
changing `installed`. Browsing an uninstalled app creates no installation record.
PSPDX registers its own running version on startup.

For example, `installed.version = 1.0` and `latest.version = 1.1` means the
installed version is still 1.0. Only a successful update changes it to 1.1.
Update detection compares the corresponding `published_at` timestamps.
`added_from` records the original import route, not a required update source.
Each operation writes only the affected app's state file.

Recoverable writes may leave `.new` or `.bak` siblings until recovery.
Staging and rollback directories stay under `GAME/` because PSP directory
renames require the same parent. Keep `INSTALLED/` for update tracking;
`CACHE/` can be rebuilt. Do not remove a pending transaction's files.
Only per-app `.state.json` files are loaded; older combined state files are
ignored and left untouched. Corrupt app records are preserved and block writes.

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

<a id="format-specification"></a>

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
| `installdir` | Required; `PSP/GAME/` followed by 1–32 letters, digits, dots, underscores or hyphens; not `.` or `..` | — |
| `author` | Up to 39 characters | Repository owner |
| `summary` | Up to 60 characters | Repository description |
| `license` | SPDX identifier, up to 64 characters | Repository license metadata |

Version 1 installs only under `PSP/GAME/`. A plugin requiring `seplugins`
and changes to `plugins.txt` needs a different installation contract.
A future incompatible manifest format gets a new schema URL.

Release information is derived, never duplicated in the manifest:

| Value | Source |
|---|---|
| `id` | `io.github.<owner>.<repo>`; owner and repository lowercased and stripped to `[a-z0-9]`. Colliding identities must be rejected. |
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
names an aggregated catalog. `catalog.json` contains `schema`, `generated`
and an `apps` array. Each app carries `id`, `name`, `author`, `summary`,
`category`, `license`, `repo`, `installdir` and `release` (`rev`, `version`,
`url`, `size`, `sha256`); media URLs use `icon`, `screenshot`, `video` and
`sound` when present. Relative media URLs resolve against the catalog URL.

The [catalog schema](schema/catalog-v1.json) uses the identifier
`https://github.com/chriopter/pspdx/blob/master/schema/catalog-v1.json`.

The reference builder checks releases hourly, reuses unchanged entries and
publishes only when its index changes. Manifest-only edits require a new
release or a forced catalog rebuild. Anyone can reuse the builder or publish
a different catalog; the client retains installed apps independently.

</details>

## Development

<details>
<summary>Build, code layout and tests</summary>


With PSPDEV and its SDK libraries installed:

```sh
sh app/wolfssl-psp/build.sh
make -C app
```

The build produces `app/EBOOT.PBP`. It uses wolfSSL, cJSON, intraFont, libpng,
zlib and PSP SDK libraries; CI builds inside `pspdev/pspdev:latest`.
`dev/start` builds and launches the client in the configured PPSSPP setup;
`dev/start --no-build` launches an existing build. These scripts assume
Linux with Docker and the PPSSPP Flatpak. `dev/release <version> [notes]`
builds and publishes a GitHub release from a clean `master` checkout.


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

EBOOT media generation needs ffmpeg and a C compiler; audio also needs
`atracdenc`. Both scripts accept a start offset as their third argument;
`DURATION` controls length. Video defaults to six seconds at 144×80;
audio defaults to eighteen seconds of ATRAC3 at 132 kbps.

</details>
