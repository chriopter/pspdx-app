#!/usr/bin/env python3
"""Forty entries out of the published catalog: each real app repeated under
numbered ids with its own assets copied in, so a long list can be scrolled
without forty real apps. Two of them are the shapes the published apps do
not have: one without tags, which stands in All alone, and one plugin, which
is listed and not installed. Usage: make-catalog.py <site dir>."""
import copy, json, os, sys, urllib.request

SRC = "https://chriopter.github.io/pspdx-catalog/"
site = sys.argv[1]
os.makedirs(site, exist_ok=True)
cat = json.load(urllib.request.urlopen(SRC + "catalog.json"))
apps = cat["apps"]

def current(a):
    """An entry in the shape catalog-v1.json gives it now, whichever
    shape the published catalog is still in: a single release becomes the
    first of the releases, a category the one tag, a screenshot the first of
    the screenshots."""
    if "release" in a:
        r = a.pop("release")
        d = r["download"]
        a["releases"] = [dict(tag=r["tag"], published_at=r["published_at"],
                              url=d["url"], size=d["size"], sha256=d["sha256"])]
    if "category" in a:
        a.setdefault("tags", [a.pop("category")])
    media = a.get("media", {})
    if "screenshot" in media:
        media["screenshots"] = [media.pop("screenshot")]
    return a

apps = [current(a) for a in apps]

def fetch(rel):
    out = os.path.join(site, rel)
    if os.path.exists(out): return
    os.makedirs(os.path.dirname(out), exist_ok=True)
    urllib.request.urlretrieve(SRC + rel, out)

for a in apps:
    media = a.get("media", {})
    for rel in [media.get("icon"), media.get("video")] + media.get("screenshots", [])[:1]:
        if rel: fetch(rel)

def manifest(a):
    return dict(schema="https://chriopter.github.io/pspdx/schema/pspdx-v1.json", source=a["source"],
                name=a["name"][:39], **{k: a[k] for k in ("type", "tags", "installdir") if k in a})

out = []
for i in range(40):
    a = copy.deepcopy(apps[i % len(apps)])
    a["id"] = "io.github.pspdxfixture.app%02d" % i
    a["source"] = "https://github.com/pspdxfixture/app%02d" % i
    a["installdir"] = "PSP/GAME/Fixture%02d" % i
    a["name"] = "%s %d" % (a["name"], i + 1)
    if i == 38:
        a.pop("tags", None)
    if i == 39:
        a["type"] = "plugin"
        a["tags"] = ["plugin"]
        a.pop("installdir")
    a["_test_manifest"] = manifest(a)
    out.append(a)

# Two more, for the installer: the same EBOOT in the two archive layouts the
# published apps do not use -- at the root of the archive, and one directory
# down beside a readme. Served from here with their own checksums.
import hashlib, io, zipfile
src = [a for a in apps if a.get("releases") and a.get("type", "homebrew") == "homebrew"][0]
raw = urllib.request.urlopen(src["releases"][0]["url"]).read()
z = zipfile.ZipFile(io.BytesIO(raw))
eboot = [n for n in z.namelist() if n.lower().endswith("eboot.pbp")][0]
payload = z.read(eboot)

def entry(id, name, members):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as w:
        for path, data in members: w.writestr(path, data)
    blob = buf.getvalue()
    rel = "pkgs/%s.zip" % id
    os.makedirs(os.path.join(site, "pkgs"), exist_ok=True)
    open(os.path.join(site, rel), "wb").write(blob)
    a = copy.deepcopy(src)
    a["id"] = id; a["name"] = name
    a["source"] = "https://github.com/chriopter/" + id.rsplit(".",1)[1]
    a["installdir"] = "PSP/GAME/" + id.rsplit(".",1)[1]
    a["_test_manifest"] = manifest(a)
    a["releases"] = [dict(a["releases"][0], url="https://127.0.0.1:8443/" + rel,
                          sha256=hashlib.sha256(blob).hexdigest(), size=len(blob))]
    return a

out.append(entry("io.github.chriopter.layoutroot", "Layout: EBOOT at the root",
                 [("EBOOT.PBP", payload), ("readme.txt", b"at the root\n")]))
out.append(entry("io.github.chriopter.layoutdir", "Layout: one directory down",
                 [("README.md", b"beside the package\n"), ("Demo Dir/EBOOT.PBP", payload),
                  ("Demo Dir/data/level.txt", b"data\n"), ("Demo Dir/data/more/deep.txt", b"deep\n")]))
cat["apps"] = out
json.dump(cat, open(os.path.join(site, "catalog.json"), "w"))
print("catalog: %d apps" % len(out))
