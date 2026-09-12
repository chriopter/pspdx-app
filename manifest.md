# .pspdx, lists, and the EBOOT

An app is a GitHub repository that says so: a `.pspdx` file in its root,
and a release that carries one zip with an `EBOOT.PBP` in it. The file is
the author's consent and their words; everything that changes is derived
from the release and the EBOOT and never written by hand.

## .pspdx

```json
{
  "schema":     "https://github.com/chriopter/pspdx/blob/master/schema/v1.pspdx",
  "name":       "Lux Aeterna",
  "author":     "chriopter",
  "summary":    "Ten churches, and the sun through their glass.",
  "category":   "demo",
  "license":    "BSD-3-Clause"
}
```

JSON, validated against [`schema/v1.pspdx`](schema/v1.pspdx); the `schema`
line names that file, so the file says what it is in ten years, and a
version 2 gets a new name without making a single old file wrong.

| field | | otherwise |
|---|---|---|
| `schema` | required | |
| `name` | required, under 40 characters | |
| `category` | required: `game`, `emulator`, `app`, `plugin`, `demo`, one word for what this one app is | |
| `summary` | one line, at most 60 characters | the repository's description |
| `license` | an SPDX identifier | what GitHub reports |
| `author` | a name | the repository's owner |

Unknown fields are refused, so a misspelt one is noticed rather than
ignored.

## Derived, never written

| | source |
|---|---|
| `id` | the repository URL: `io.github.<owner>.<repo>`, lower case, `[a-z0-9]` in the last part, dashes and dots dropped. Nobody types it, so it cannot be wrong, and a fork is its own app. |
| `version` | the release tag without its `v` |
| `rev` | the release's `published_at` as unix seconds. Integers compare; version strings do not. **The higher `rev` wins.** |
| `url`, `size` | the zip on the release, of which there must be exactly one |
| `sha256` | computed by whoever downloads the whole zip: the cache |
| the package | the directory in the zip that holds the one `EBOOT.PBP`, and its name is the folder the app gets under `PSP/GAME/`; its `PARAM.SFO` must say `CATEGORY` `MG`, a game or app for the Memory Stick |
| icon, picture, film, sound | `ICON0.PNG`, `PIC1.PNG`, `ICON1.PMF` and `SND0.AT3` inside the EBOOT, where Sony put them and where the XMB reads them |

A release that is a pre-release or a draft is not seen. Nothing is ever
compared but `rev`.

## The list

Where the console finds apps is a list: a text file, one GitHub repository
a line. Anyone can publish one.

```
# repos.txt: one repository a line; @tag pins a release
cache https://chriopter.github.io/pspdx-catalog/catalog.json
https://github.com/chriopter/pspdx-demo
https://github.com/someone/psp-thing@v1.2
```

A `cache` line names a catalog that has done the reading already. A
repository without a `.pspdx` is not listed, by any list: the file is the
consent.

## The cache

A workflow at the list's repository, hourly and on request, walks the
list: reads each `.pspdx`, asks GitHub for the repository and its release,
downloads the zip, hashes it, checks the EBOOT, takes the pictures and the
sound out of the PBP, and writes one `catalog.json` with the pictures
beside it under names that carry their bytes (`icons/<id>-<sha8>.png`).
Stateless: nothing is committed, the next run reads it all again. What
fails is reported with the reason and left out.

`catalog.json` is `{"schema", "generated", "apps": [...]}`; each app carries
`id`, `name`, `author`, `summary`, `category`, `license`, `repo`, `release`
(`rev`, `url`, `sha256`, `size`, `version`) and `icon`, `screenshot`,
`video`, `sound` where there was one, and `install` where the file had it.

## The console

`PSP/PSPDX/sources.txt` holds the lists, one URL a line, the built-in one
first; a repository URL is a list of one, and that is how a single app in no
catalog gets in, typed as `owner/repo` on the firmware's keyboard. For every
list: the cache is taken when it answers; every repository the cache did
not cover, and every repository when there is no cache, is read at the
origin: its `.pspdx` from `raw.githubusercontent.com`, its release from
GitHub's API. There are no pictures that way, since they sit in a package
it has not fetched. No hash that way: the console checks the
size of what it downloads, and the zip comes from the author's own account
over TLS, which is the trust there is. An installed app is pictured from
its own EBOOT on the stick whether or not any cache ever was.

An install fetches the zip, checks the size (and the hash when it has one),
and copies the contents of the package into `PSP/GAME/` under the
package's own name, subfolders and all. Whatever the zip holds outside
that directory, a licence or a readme at the top, is left behind, and
nothing from a zip is ever written anywhere else. An app whose package
directory is called something else in a later release moves on the stick:
the record says where the old one was, and the update takes it away. Then
it writes `PSP/PSPDX/db/<id>.json`:
`id`, `rev`, `dir`, `version`, `repo`. An update is a larger `rev` for the
same id from any source. The first list to name an id wins.

## What was left out

**The release in the file.** Version, date, URL and hash were in an earlier
shape of this file and went stale the first week: a file that has to be
touched at every release is touched at none. The release knows all of it,
and which release is current is GitHub's own answer: the latest one that
is not a draft and not a pre-release. A tag can still be frozen, by the
curator, with `@tag` on the list line.

**A glob for the package, and a field naming the folder.** In version 1 a
release carries exactly one zip, and that zip holds exactly one
`EBOOT.PBP` inside a directory, or the app is not listed. That directory
is the package and its name is the folder on the stick, which is what a
person copying the zip by hand gets too. Two zips, two EBOOTs, or an
EBOOT lying loose at the top are questions for the author.

**A place in the repository for the pictures.** The EBOOT already carries
them, every author already packs them, and a second copy in the repository
is a second copy to keep in step. A console reading a repository directly
therefore shows no picture until the app is installed, and then reads the
EBOOT on the stick; a catalog has the zip in its hands anyway.

**Anything outside `PSP/GAME/`.** A plugin that belongs in `seplugins` and
wants a line in `plugins.txt`, a folder of ROMs beside an emulator, a
savedata template: a different way of installing, with its own rules, and
a path field bent far enough to reach it would be a path field that can
reach anything. Version 2 can have a kind of its own for that.

**The repository in the file.** Whoever reads the file knows where it came
from, and a file that named a repository would let a fork claim the
original.

**Signatures.** The trust is in the release on the author's own account and
in whoever wrote the list.
