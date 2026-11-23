#!/usr/bin/env python3
"""Builds the NBTApps package of ID6Term: a signed .nbtpkg for the community catalogue
(https://github.com/tom-pryor/nbtapps-packages) plus the record files its pull request and release bot want.

    dist/nbtpkg/src/                        the package source: manifest.json (from nbtapps/manifest.json.in),
                                            payload/ID6Term/<the five plugin files>, licenses/LICENSE
    dist/nbtpkg/<id>-<version>.nbtpkg       the archive, signed with --key
    dist/nbtpkg/nbtpkg-record.json          for the GitHub release: lets the catalogue's bot propose later versions
    dist/nbtpkg/versions/<version>.json     the record to add as packages/<id>/versions/<version>.json in the PR
    dist/nbtpkg/listing.json                copy of nbtapps/listing.json, for the PR's packages/<id>/listing.json

Needs a checkout of https://github.com/tom-pryor/nbtapps (its headunit/tools/nbtpkg.py and the manifest schema)
and the python module `cryptography` for signing. The plugin is built first (make / Momentics); this script runs
tools/package.py to collect it.

Usage:
    python tools/nbtpkg_build.py --key ~/.nbtpkg/josephworks.key [--note "What is new"]...
    python tools/nbtpkg_build.py --unsigned          # to try the package format without a key
    python tools/nbtpkg_build.py --keygen NAME       # once: NAME.key / NAME.pub in ~/.nbtpkg

`--key` and `--keydir` take `~`, `$VAR` and `%VAR%`, whatever shell you are in.

The version is ID6TERM_RELEASE in src/id6term_app.cpp and the package id is the template's. The record's url is the
GitHub release asset of tag v<version>; `source.commit` is HEAD, so commit (and tag) before building the archive you
publish.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import package  # noqa: E402  (tools/package.py: collects dist/ID6Term/)

PLUGIN_NAME = "ID6Term"
REPO_URL = "https://github.com/josephworks/ID6Term"
PAYLOAD_FILES = ("libID6Term.so", "ID6Term.manifest", "ID6Term.xml", "ID6Term_ALL_Texts.zip", "ID6Term_ALL_Images.zip")

NBTAPPS_DIR = os.path.join(ROOT, "nbtapps")
TEMPLATE = os.path.join(NBTAPPS_DIR, "manifest.json.in")
LISTING = os.path.join(NBTAPPS_DIR, "listing.json")
OUT = os.path.join(ROOT, "dist", "nbtpkg")
SRC = os.path.join(OUT, "src")
DEFAULT_NBTAPPS = os.path.join(os.path.dirname(ROOT), "nbtapps", "headunit")
DEFAULT_KEYDIR = os.path.join(os.path.expanduser("~"), ".nbtpkg")


def release_version():
    with open(os.path.join(ROOT, "src", "id6term_app.cpp"), encoding="utf-8") as f:
        m = re.search(r'^#define ID6TERM_RELEASE\s+"([^"]+)"', f.read(), re.M)
    if not m:
        sys.exit("nbtpkg_build.py: ID6TERM_RELEASE not found in src/id6term_app.cpp")
    return m.group(1)


def git(*args):
    try:
        return subprocess.check_output(["git", "-C", ROOT] + list(args), text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def user_path(path):
    """Expands ~, $VAR and %VAR% (os.path.expandvars knows %VAR% on Windows only) so a key path works in any shell."""
    path = os.path.expandvars(os.path.expanduser(path))
    return re.sub(r"%([^%]+)%", lambda m: os.environ.get(m.group(1), m.group(0)), path)


def nbtpkg_tool(nbtapps):
    tool = os.path.join(nbtapps, "tools", "nbtpkg.py")
    schema = os.path.join(nbtapps, "spec", "manifest.schema.json")
    if not (os.path.isfile(tool) and os.path.isfile(schema)):
        sys.exit("nbtpkg_build.py: %s is not a checkout of nbtapps/headunit (no tools/nbtpkg.py + spec/manifest.schema.json)\n"
                 "    git clone https://github.com/tom-pryor/nbtapps %s" % (nbtapps, os.path.dirname(nbtapps)))
    return tool


def run(tool, *args):
    cmd = [sys.executable, tool] + list(args)
    print("+ " + " ".join(cmd))
    return subprocess.check_output(cmd, text=True)


def collect_source(version):
    """dist/ID6Term/ (via package.py) -> dist/nbtpkg/src/ with the filled-in manifest. Returns the manifest."""
    package.main()
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    payload = os.path.join(SRC, "payload", PLUGIN_NAME)
    os.makedirs(payload)
    for name in PAYLOAD_FILES:
        shutil.copy2(os.path.join(package.DIST, name), payload)
    os.makedirs(os.path.join(SRC, "licenses"))
    shutil.copy2(os.path.join(ROOT, "LICENSE"), os.path.join(SRC, "licenses", "LICENSE"))
    with open(TEMPLATE, encoding="utf-8") as f:
        manifest = json.loads(f.read().replace("@VERSION@", version))
    if manifest["version"] != version:
        sys.exit("nbtpkg_build.py: %s must take its version from @VERSION@" % TEMPLATE)
    with open(os.path.join(SRC, "manifest.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)
        f.write("\n")
    return manifest


def write_json(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(value, f, indent=2, ensure_ascii=False)
        f.write("\n")


def plugin_ids():
    """The ids the catalogue hands out, read from the plugin files so the PR text cannot drift from them."""
    with open(os.path.join(ROOT, "res", "ID6Term.manifest"), encoding="utf-8") as f:
        connector = re.search(r"<id>(\d+)</id>", f.read()).group(1)
    with open(os.path.join(ROOT, "res", "ID6Term.xml"), encoding="utf-8") as f:
        focus = re.search(r'entryButtonFocusID="(\d+)"', f.read()).group(1)
    return int(connector), int(focus)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Build the NBTApps package of ID6Term.")
    ap.add_argument("--nbtapps", default=DEFAULT_NBTAPPS, help="checkout of nbtapps/headunit (default: %(default)s)")
    ap.add_argument("--key", help="private Ed25519 key file (nbtpkg.py keygen); its .pub is the PR's author.pub")
    ap.add_argument("--unsigned", action="store_true", help="build without a signature (not publishable)")
    ap.add_argument("--keygen", metavar="NAME", help="make NAME.key / NAME.pub in --keydir and exit")
    ap.add_argument("--keydir", default=DEFAULT_KEYDIR, help="where --keygen writes (default: %(default)s)")
    ap.add_argument("--released", default=datetime.date.today().isoformat(), help="release date YYYY-MM-DD (default: today)")
    ap.add_argument("--note", action="append", default=[], help="a release note line (repeatable)")
    ap.add_argument("--url", help="where the archive will be published (default: the GitHub release asset of v<version>)")
    args = ap.parse_args(argv)
    args.nbtapps, args.keydir = user_path(args.nbtapps), user_path(args.keydir)
    if args.key:
        args.key = user_path(args.key)
        if not os.path.isfile(args.key):
            sys.exit("nbtpkg_build.py: key file %s not found (make one with --keygen NAME)" % args.key)

    tool = nbtpkg_tool(args.nbtapps)
    if args.keygen:
        os.makedirs(args.keydir, exist_ok=True)
        print(run(tool, "keygen", args.keygen, "-o", args.keydir), end="")
        print("Keep %s.key private and out of every repo; %s.pub goes into the pull request as author.pub."
              % (os.path.join(args.keydir, args.keygen), os.path.join(args.keydir, args.keygen)))
        return
    if bool(args.key) == args.unsigned:
        ap.error("give --key FILE, or --unsigned")

    version = release_version()
    package_id = collect_source(version)["id"]
    archive = os.path.join(OUT, "%s-%s.nbtpkg" % (package_id, version))

    build = ["build", SRC, "-o", OUT]
    if args.key:
        build += ["--key", args.key]
    print(run(tool, *build), end="")
    verify = ["verify", archive, "--json"]
    verify += ["--key", re.sub(r"\.key$", "", args.key) + ".pub"] if args.key else ["--unsigned"]
    info = json.loads(run(tool, *verify))
    with open(archive, "rb") as f:
        sha256 = hashlib.sha256(f.read()).hexdigest()
    assert sha256 == info["sha256"]

    commit = git("rev-parse", "HEAD") or "0" * 40
    dirty = bool(git("status", "--porcelain"))
    url = args.url or "%s/releases/download/v%s/%s" % (REPO_URL, version, os.path.basename(archive))
    record = {
        "url": url,
        "sha256": sha256,
        "released": args.released,
        "notes": args.note or ["ID6Term %s" % version],
        "source": {"repo": REPO_URL, "commit": commit},
    }
    write_json(os.path.join(OUT, "nbtpkg-record.json"), {"id": package_id, "version": version, "record": record})
    write_json(os.path.join(OUT, "versions", "%s.json" % version), record)
    shutil.copy2(LISTING, os.path.join(OUT, "listing.json"))

    connector, focus = plugin_ids()
    print()
    print("%s  %d bytes" % (archive, info["size"]))
    print("sha256     %s" % sha256)
    print("flags      %s" % (", ".join(info["flags"]) or "none"))
    print("signature  %s" % ("key " + info["key"][:16] + "..." if "key" in info else "NONE - not publishable"))
    print("record     %s" % os.path.join(OUT, "nbtpkg-record.json"))
    print("PR files   %s, %s" % (os.path.join(OUT, "listing.json"), os.path.join(OUT, "versions", version + ".json")))
    print("plugin ids connector %d, entry focus %d (name them in the pull request)" % (connector, focus))
    if dirty or commit == "0" * 40:
        print("WARNING: the working tree is not a clean commit; the record's source.commit (%s) must be the commit"
              " the archive was built from. Commit, tag v%s and build again before publishing." % (commit[:12], version))


if __name__ == "__main__":
    main()
