#!/usr/bin/env python3
"""Regenerates app/network/ca_certs.h, the roots the client trusts.

The PSP has no usable CA store: Sony's is from 2007 and its roots expired years
ago. So the client carries its own. Carrying all ~150 Mozilla roots would be
200 KB of EBOOT and 150 chances to trust someone we never meant to, so this
takes the roots that anchor the hosts PSPDX actually talks to, plus their
siblings, and pins every one by SHA-256. A swapped CA store on the machine that
runs this script therefore cannot quietly change what a console trusts.

Which roots are needed is not a fixed list: jsDelivr is multi-CDN and answers
from GlobalSign at one point of presence and Sectigo at another, and GitHub has
moved between DigiCert, Sectigo and Let's Encrypt. An unknown root is a failed
handshake with "certificate verify failed" in the log, and the fix is to add it
below and re-run this.

    python3 dev/make-ca-bundle.py

Roots that are not yet in the host's store -- a new one, freshly cross-signed --
go in app/ca-extra/ as PEM. Each is checked against the host store before it is
included, so their trust still comes from the same place, not from whoever
served the file.
"""

import re
import subprocess
import sys
import tempfile
from pathlib import Path

# label, a substring that must appear in the subject, sha256 pin.
ROOTS = [
    # Let's Encrypt: github.io, and the githubusercontent hosts behind it.
    ("ISRG Root X1", "CN=ISRG Root X1",
     "96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6"),
    ("ISRG Root X2", "CN=ISRG Root X2",
     "69729b8e15a86efc177a57afb7171dfc64add28c2fca8cf1507e34453ccb1470"),
    # Sectigo and its predecessor names: github.com, one of jsDelivr's CDNs.
    ("Sectigo Root R46", "CN=Sectigo Public Server Authentication Root R46",
     "7bb647a62aeeac88bf257aa522d01ffea395e0ab45c73f93f65654ec38f25a06"),
    ("Sectigo Root E46", "CN=Sectigo Public Server Authentication Root E46",
     "c90f26f0fb1b4018b22227519b5ca2b53e2ca5b3be5cf18efe1bef47380c5383"),
    ("USERTrust RSA", "CN=USERTrust RSA Certification Authority",
     "e793c9b02fd8aa13e21c31228accb08119643b749c898964b1746d46c3d4cbd2"),
    ("USERTrust ECC", "CN=USERTrust ECC Certification Authority",
     "4ff460d54b9c86dabfbcfc5712e0400d2bed3fbc4d4fbdaa86e06adcd2a9ad7a"),
    # GlobalSign: what cdn.jsdelivr.net answered with from here.
    ("GlobalSign Root R3", "OU=GlobalSign Root CA - R3",
     "cbb522d7b7f127ad6a0113865bdf1cd4102e7d0759af635a7cf4720dc963c53b"),
    ("GlobalSign Root R6", "OU=GlobalSign Root CA - R6",
     "2cabeafe37d06ca22aba7391c0033d25982952c453647349763a3ab5ad6ccf69"),
    ("GlobalSign ECC R4", "OU=GlobalSign ECC Root CA - R4",
     "b085d70b964f191a73e4af0d54ae7a0e07aafdaf9b71dd0862138ab7325a24a2"),
    ("GlobalSign ECC R5", "OU=GlobalSign ECC Root CA - R5",
     "179fbc148a3dd00fd24ea13458cc43bfa7f59c8182d783a513f6ebec100c8924"),
    ("GlobalSign Root R46", "CN=GlobalSign Root R46",
     "4fa3126d8d3a11d1c4855a4f807cbad6cf919d3a5a88b03bea2c6372d93c40c9"),
    ("GlobalSign Root E46", "CN=GlobalSign Root E46",
     "cbb9c44d84b8043e1050ea31a69f514955d7bfd2e2c6b49301019ad61d9f5058"),
    # DigiCert: where GitHub was before, and half the internet besides.
    ("DigiCert Global Root G2", "CN=DigiCert Global Root G2",
     "cb3ccbb76031e5e0138f8dd39a23f9de47ffc35e43c1144cea27d46a5ab1cb5f"),
    ("DigiCert Global Root G3", "CN=DigiCert Global Root G3",
     "31ad6648f8104138c738f39ea4320133393e3a18cc02296ef97c2ac9ef6731d0"),
    # Amazon: whoever hosts a homebrew author's release next year.
    ("Amazon Root CA 1", "CN=Amazon Root CA 1",
     "8ecde6884f3d87b1125ba31ac3fcb13d7016de7f57cc904fe1cb97c6ae98196e"),
    ("Amazon Root CA 3", "CN=Amazon Root CA 3",
     "18ce6cfe7bf14e60b2e347b8dfe868cb31d02ebb3ada271569f50343b46db3a4"),
]

# Roots too new to be in a distribution's store yet. Each is verified against
# the host store below -- included only if a root already trusted there signed
# it -- so trusting it is the same act as trusting the store.
EXTRA_DIR = Path(__file__).resolve().parent.parent / "app" / "ca-extra"

SOURCES = [
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/pki/tls/certs/ca-bundle.crt",
    "/etc/ssl/cert.pem",
]

OUT = Path(__file__).resolve().parent.parent / "app" / "network" / "ca_certs.h"


def describe(pem):
    r = subprocess.run(["openssl", "x509", "-noout", "-subject", "-fingerprint", "-sha256"],
                       input=pem, capture_output=True, text=True)
    if r.returncode != 0:
        return None, None
    sub = re.search(r"subject=(.*)", r.stdout)
    fp = re.search(r"Fingerprint=([0-9A-Fa-f:]+)", r.stdout)
    return (sub.group(1).strip() if sub else None,
            fp.group(1).replace(":", "").lower() if fp else None)


def main():
    src = next((p for p in SOURCES if Path(p).exists()), None)
    if not src:
        sys.exit("no CA bundle found on this host")
    store = Path(src).read_text()
    pems = re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----\n", store, re.S)

    picked = []
    for label, needle, pin in ROOTS:
        hit = None
        for pem in pems:
            sub, fp = describe(pem)
            if sub and needle in sub:
                if fp != pin:
                    sys.exit(f"{label}: fingerprint {fp} does not match the pin")
                hit = pem
                break
        if hit is None:
            sys.exit(f"{label}: not in {src}")
        picked.append((label, hit))

    for extra in sorted(EXTRA_DIR.glob("*.pem")) if EXTRA_DIR.is_dir() else []:
        pem = extra.read_text()
        sub, fp = describe(pem)
        with tempfile.NamedTemporaryFile("w", suffix=".pem") as f:
            f.write(pem)
            f.flush()
            r = subprocess.run(["openssl", "verify", "-partial_chain", "-CAfile", src, f.name],
                               capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"{extra.name}: not signed by anything in {src}\n{r.stdout}{r.stderr}")
        picked.append((f"{sub}  (cross-signed, verified against the host store)", pem))

    lines = [
        "/* Generated by dev/make-ca-bundle.py. Do not edit.",
        " *",
        " * The roots PSPDX trusts, each pinned by SHA-256 in the generator. The",
        " * console has no usable CA store of its own, so the client carries the",
        " * handful it needs rather than all of Mozilla's.",
        " */",
        "#ifndef PSPDX_CA_CERTS_H",
        "#define PSPDX_CA_CERTS_H",
        "",
        "static const char PSPDX_CA_PEM[] =",
    ]
    for label, pem in picked:
        lines.append(f"    /* {label} */")
        for line in pem.splitlines():
            lines.append(f'    "{line}\\n"')
    lines += ["    ;", "", "#endif", ""]
    OUT.write_text("\n".join(lines))
    print(f"{len(picked)} roots, {OUT.stat().st_size} bytes -> {OUT}")


if __name__ == "__main__":
    main()
