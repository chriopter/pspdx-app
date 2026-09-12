# PSPDX

**PSP Download Index** — browse, install, run and update homebrew directly
on your PlayStation Portable. **[→ Download PSPDX](https://github.com/chriopter/pspdx/releases/latest)**

PSPDX ships with a catalog. Add others or share `.pspdx` files directly:
their open format links to authors' GitHub repositories for downloads and
updates, even if the catalog disappears.

<img width="480" alt="PSPDX App browsing homebrew and available updates" src="images/pspdx-app.png" />

## Why?

I wanted one place to browse and update homebrew on the PSP without creating
a download mirror or a single point of failure.

PSPDX lets anyone share apps and build catalogs while downloads and updates
come from the authors' repositories. Catalogs help you find apps; keeping
them up to date does not depend on a catalog staying online.

- **[Downloader app](https://github.com/chriopter/pspdx/releases/latest)** — install, run and update homebrew on your PSP.
- **[Reference catalog](https://github.com/chriopter/pspdx-catalog)** — [browse apps](https://chriopter.github.io/pspdx-catalog/); or build your own catalog.
- **[Demo app](https://github.com/chriopter/pspdx-demo)** — a complete example for homebrew authors.

## The PSPDX client

PSPDX App lets you browse, install, run and update homebrew on your PSP.
Add `.pspdx` files through INBOX, subscribe to catalogs or enter a GitHub
repository URL; installed apps retain their source for future updates.

<details>
<summary>Browse Catalogs</summary>

- **Add catalog:** HTTPS `catalog.json` URL; the reference catalog is preconfigured.
- **Add GitHub repository:** repository URL or `owner/repo`.
- Sources are validated before saving to `sources.txt`; text lists also work.

</details>

<details>
<summary>Install &amp; Update Brews</summary>

**Read INBOX** validates `PSP/PSPDX/INBOX/*.pspdx` and installs valid apps
sequentially after one confirmation. Conflicts are skipped; only successful
imports leave INBOX. Hold **○** to stop between packages; self-updates run last.

One release ZIP, one `EBOOT.PBP`. Checks cover size, SHA-256 when supplied,
ZIP integrity and paths. The EBOOT's directory contents go into `installdir`;
unmanaged folders are neither adopted nor overwritten.

A transaction journal covers files, manifest and state. Staging and backups
stay beside the destination for PSP directory renames; recovery touches only
journaled paths. Old databases and caches are not migrated or removed.

On the startup device (`ms0:` or `ef0:`), `PSP/PSPDX/INSTALLED/` holds each
app's original `.pspdx` and `state.json`, keyed by app ID:

| State | Stored fields |
|---|---|
| Identity | `source`, `added_from` (catalog, repository or INBOX) |
| `installed` | `version`, `published_at`, `installdir` |
| `latest` | `version`, `published_at`, `download_url`, `size`, optional `sha256`, `checked_at`, `checked_from` |

**×** install/update · **△** options · **○** back · **□** basket ·
**START** run · **L/R** or **←/→** switch tabs.

A refresh uses fresh catalog entries; a newer release publication time marks
an update. **×** downloads and installs it from the author's release.

No fresh entry, invalid or missing catalog: check the installed app's saved
GitHub source directly. **Check original sources** forces this lookup for
installed apps even with a reachable catalog. Updates remain possible while
the original source and releases are available.

If GitHub also fails, retain the last release information and check time.
Cached results describe the last known state; they do not confirm freshness.

</details>

<details>
<summary>Connect — TLS 1.3</summary>

- **TLS:** wolfSSL, bundled CAs, chain/hostname verification, X25519 preferred.
  Analog-stick entropy and a saved seed feed the random generator.
  Certificate date checks are bypassed for unset PSP clocks; other checks remain.
- **Requests:** one catalog request supplies many apps' metadata. Direct lookup
  reads manifests from `raw.githubusercontent.com`, releases from `api.github.com`;
  downloads follow GitHub asset redirects. Each request opens a new connection;
  no keep-alive or connection reuse.
- **Scheduling:** background synchronization; media pauses while other work
  needs the network. No package signatures: sources must be trusted.

</details>

<details>
<summary>Catalog Caching</summary>

Catalog responses are cached for offline browsing alongside installed-app
records. Deleting catalog or media caches does not remove installation records.

The reference catalog extracts EBOOT media and hosts it separately:
`ICON0.PNG` (icon), `PIC1.PNG` (background), `ICON1.PMF` (video),
`SND0.AT3` (sound). Browsing fetches icons and selected-app previews into
`PSP/PSPDX/CACHE/media/`, without downloading app ZIPs.

Installed apps use their own EBOOT media first, then cache/catalog media.
Direct repository or INBOX additions fetch no separate GitHub previews:
before installation, only existing cache or placeholders are available.
Offline, only local EBOOTs and cache are used. All media is optional;
missing images use defaults, missing video/sound does not play.

</details>

## The PSPDX standard

Publish a `.pspdx` in your repository to make your homebrew available to
PSPDX; the [demo app](https://github.com/chriopter/pspdx-demo) is a complete example.

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
