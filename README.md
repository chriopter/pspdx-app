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
