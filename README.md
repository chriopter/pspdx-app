# PSPDX

**[Get PSPDX App →](https://github.com/chriopter/pspdx/releases/latest)**

**PSP Download Index** — a portable JSON file that points to PlayStation
Portable homebrew at its source.

Add a `.pspdx` file to PSPDX App on your PSP to download homebrew and keep
track of updates. Or add a catalog to browse multiple apps and keep them
up to date.

<img width="480" alt="PSPDX App browsing homebrew and available updates" src="images/pspdx-app.png" />

## Why?

PSP homebrew is scattered across repositories and websites. I wanted to find
and update it without relying on another catalog staying maintained.

PSPDX is decentralized: a homebrew author publishes a `.pspdx` in their
GitHub repository. Add that file directly to your PSPDX downloader, or use
a catalog that collects multiple apps. Anyone can publish a catalog.

Once added, your PSP can check the original source for updates and keep
your homebrew up to date, independently of the catalog.

- **[Downloader app](app/)** — install, run and update homebrew on your PSP. [Latest release](https://github.com/chriopter/pspdx/releases/latest).
- **[Reference catalog](https://chriopter.github.io/pspdx-catalog/)** — browse apps; or [build your own catalog](https://github.com/chriopter/pspdx-catalog).
- **[Demo app](https://github.com/chriopter/pspdx-demo)** — a complete example for homebrew authors.

## The file

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

Required: `schema`, `source`, `name`, `category`, `installdir`. Other fields
default to repository metadata. Releases provide versions and downloads;
the EBOOT provides media. Authors need not edit the file for every release.

Version 1 supports GitHub repositories with a root `.pspdx` and a published
release containing exactly one ZIP with exactly one `EBOOT.PBP`.

[Format specification](manifest.md) · [Schema](schema/v1.pspdx)

## Client files and sources

PSPDX keeps its own files under `PSP/PSPDX/` on the storage device it was
started from (`ms0:` or `ef0:`). Homebrew stays under `PSP/GAME/`.

```text
PSP/
├── GAME/
│   ├── PSPDX/
│   │   └── EBOOT.PBP
│   ├── <App-Directory>/
│   │   └── …
│   ├── .pspdx-stage/            # Temporary installation
│   └── <App-Directory>.old/     # Temporary backup
└── PSPDX/
    ├── sources.txt             # Subscribed catalogs, lists and repositories
    ├── INBOX/
    │   └── *.pspdx             # Files waiting to be installed
    ├── INSTALLED/
    │   ├── <App-ID>.pspdx      # Original manifest, including source
    │   └── state.json          # Installed and last known release information
    ├── CACHE/
    │   ├── catalogs/           # Last usable catalog JSON, for offline browsing
    │   └── media/              # Downloaded icons, pictures, films and sounds
    ├── TMP/
    │   ├── download.zip
    │   └── transaction.json    # Interrupted installation recovery
    ├── LOGS/
    │   └── pspdx.log           # HTTP and TLS diagnostics also live here
    ├── DEBUG/                  # Test controls, screenshots and emulator font
    └── CRYPTO/
        └── seed.bin
```

Use the settings tab to **Read INBOX**, **Add catalog**, or **Add GitHub
repository**. Repository input accepts a GitHub URL or `owner/repo`; the
repository must contain a valid root `.pspdx` and a release with one ZIP.
Catalog input accepts the HTTPS URL of its JSON index.

Reading INBOX validates the files and asks once before installing the valid
apps in sequence. Conflicting manifests are skipped. Successfully installed
files leave INBOX; failed or skipped files remain. Hold Circle to stop before
the next package. PSPDX's own update is installed last.

Each installed app keeps its original `.pspdx`. `state.json` stores:

```text
<App-ID>
├── source                      # Repository identity for the local record
├── added_from                  # Catalog URL, repository URL, or INBOX
├── installed
│   ├── version
│   ├── published_at
│   └── installdir              # Actual installation directory
└── latest
    ├── version
    ├── published_at
    ├── download_url
    ├── size
    ├── sha256                  # When available
    ├── checked_at
    └── checked_from            # Catalog or original repository
```

`latest` means the last successfully known release, not a promise that an
offline result is current. If a catalog cannot be reached or does not provide
an installed app, PSPDX asks that app's original GitHub source. **Check
original sources** also bypasses a reachable catalog. If GitHub fails too,
the previous information remains available. Direct source checks do not
fetch separate preview media: existing cached media or the installed EBOOT
is used, otherwise a placeholder.

The cache can be deleted without losing installed-app tracking. Installations
use the manifest's `installdir`; unmanaged existing directories are not
overwritten. An installation journal covers the app, manifest and state so
interrupted changes can be recovered together. Staging and backup directories
stay beside the app because PSP directory renames operate within one parent.
Only the journal's own paths are recovered, never unrelated `.old` folders.

This storage layout starts fresh: old database and cache files are not
migrated or automatically removed. Existing homebrew files stay untouched;
they are not adopted merely because their directory matches a manifest.

Client regression tests: `sh app/tests/run` (C compiler, Python, cJSON, zlib
and OpenSSL development files). They run the actual parser and installer
against a host filesystem adapter, including simulated interruptions after
file operations. PSP builds and emulator checks complement these tests;
real PSP storage, WLAN and power-loss behavior still require hardware testing.
