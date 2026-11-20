#!/usr/bin/env python3
"""Reassemble the eMMC rescue images from .partNNN chunks.

Usage:
    python3 merge.py                 # verify chunks + reassemble all images
    python3 merge.py --verify-only   # only verify SHA256, do not write images

This script reads manifest.json next to it, checks every chunk's SHA256,
concatenates them in order, and verifies the reassembled image against the
original image SHA256 recorded in the manifest.
"""
import argparse
import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.environ.get("MERGE_OUT", HERE)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verify-only", action="store_true")
    args = ap.parse_args()

    with open(os.path.join(HERE, "manifest.json"), encoding="utf-8") as f:
        manifest = json.load(f)

    ok = True
    for name, meta in manifest["images"].items():
        parts = meta["parts"]
        # verify each chunk
        for p in parts:
            pp = os.path.join(HERE, p["name"])
            if not os.path.isfile(pp):
                print("MISSING", p["name"])
                ok = False
                continue
            if sha256_file(pp) != p["sha256"]:
                print("CORRUPT", p["name"])
                ok = False
        if not ok:
            continue
        if args.verify_only:
            print("OK(verify) %-22s %d parts" % (name, len(parts)))
            continue
        # reassemble
        out = os.path.join(OUT_DIR, name)
        h = hashlib.sha256()
        with open(out, "wb") as of:
            for p in parts:
                with open(os.path.join(HERE, p["name"]), "rb") as pf:
                    for block in iter(lambda: pf.read(1 << 20), b""):
                        of.write(block)
                        h.update(block)
        match = (h.hexdigest() == meta["sha256"])
        print("%-22s reassembled %d bytes  sha256=%s  %s" %
              (name, meta["bytes"], h.hexdigest()[:16],
               "OK" if match else "MISMATCH!"))
        if not match:
            ok = False

    print("\nRESULT:", "ALL GOOD" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
