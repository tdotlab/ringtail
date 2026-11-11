#!/usr/bin/env python3
"""Writes disk/roots.pem, the roots fetch trusts: Mozilla's list as the
ca-certificates package installs it, one file a root, and nothing a
machine added on top of it (/usr/local/share/ca-certificates, a
company's proxy, a test CA).

    python3 tools/mkroots.py [DIR]    (DIR: /usr/share/ca-certificates/mozilla)
"""
import glob
import os
import subprocess
import sys

src = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/ca-certificates/mozilla"
files = sorted(glob.glob(os.path.join(src, "*.crt")))
if not files:
    sys.exit("mkroots: no certificates in " + src)
try:
    version = subprocess.run(["dpkg-query", "-W", "-f", "${Version}", "ca-certificates"],
                             capture_output=True, text=True).stdout.strip()
except OSError:
    version = ""
out = ["# Trusted roots for fetch: Mozilla's CA list (MPL 2.0), from the",
       "# ca-certificates package%s. Made by tools/mkroots.py;" % (" " + version if version else ""),
       "# text outside the BEGIN/END lines is ignored.", ""]
for f in files:
    pem = open(f).read().strip()
    assert pem.startswith("-----BEGIN CERTIFICATE-----") and pem.count("BEGIN") == 1, f
    out += ["# " + os.path.basename(f)[:-4].replace("_", " "), pem, ""]
here = os.path.dirname(os.path.abspath(__file__))
open(os.path.join(here, "..", "disk", "roots.pem"), "w").write("\n".join(out))
print("%d roots" % len(files))
