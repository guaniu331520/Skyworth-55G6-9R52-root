#!/usr/bin/env python3
"""Split the eMMC rescue images into GitHub-friendly chunks (<100 MiB).
Also emits SHA256SUMS and a JSON manifest used by merge.py.
"""
import hashlib
import json
import os
import shutil
from datetime import datetime, timezone

SRC = r"C:\path\to\firmware-backup-current\emmc"   # directory holding the *.img.lzo files
DST = r"C:\path\to\repo\backups"                    # output directory for .partNNN chunks
CHUNK = 90 * 1024 * 1024  # 90 MiB, safely below GitHub's 100 MiB hard limit

IMAGES = [
    "mmcblk0p1.img.lzo",   # /system
    "mmcblk0p2.img.lzo",   # /data
    "mmcblk0p3.img.lzo",   # /cache
    "mmcblk0p5.img.lzo",   # /skyworth (factory)
    "mmcblk0p6.img.lzo",   # /dtv
    "mmcblk0p7.img.lzo",   # boot (compressed)
    "mmcblk0p7.img",       # boot (raw, byte-verified)
    "mmcblk0p8.img.lzo",   # /skyota
]

DESC = {
    "mmcblk0p1.img.lzo": "/system (ext4, read-only)",
    "mmcblk0p2.img.lzo": "/data (ext4, userdata)",
    "mmcblk0p3.img.lzo": "/cache (ext4)",
    "mmcblk0p5.img.lzo": "/skyworth / factory (ext4)",
    "mmcblk0p6.img.lzo": "/dtv (ext4)",
    "mmcblk0p7.img.lzo": "boot partition, lzop-compressed",
    "mmcblk0p7.img": "boot partition, raw image (byte-verified)",
    "mmcblk0p8.img.lzo": "/skyota (ext4)",
}


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main():
    os.makedirs(DST, exist_ok=True)
    # clean old chunks
    for name in os.listdir(DST):
        p = os.path.join(DST, name)
        if os.path.isfile(p):
            os.remove(p)

    manifest = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "chunk_size_bytes": CHUNK,
        "chunk_format": "<original-name>.partNNN",
        "images": {},
    }
    sums = []

    for name in IMAGES:
        src = os.path.join(SRC, name)
        if not os.path.isfile(src):
            print("SKIP missing:", name)
            continue
        size = os.path.getsize(src)
        full_sha = sha256_file(src)
        parts = []
        n = 0
        with open(src, "rb") as f:
            while True:
                block = f.read(CHUNK)
                if not block:
                    break
                part_name = "%s.part%03d" % (name, n)
                part_path = os.path.join(DST, part_name)
                with open(part_path, "wb") as pf:
                    pf.write(block)
                psha = hashlib.sha256(block).hexdigest()
                parts.append({"name": part_name, "bytes": len(block), "sha256": psha})
                sums.append("%s  %s" % (psha, part_name))
                n += 1
        manifest["images"][name] = {
            "description": DESC.get(name, ""),
            "bytes": size,
            "sha256": full_sha,
            "part_count": n,
            "parts": parts,
        }
        print("%-22s %12d bytes  sha=%s  parts=%d" % (name, size, full_sha[:16], n))

    with open(os.path.join(DST, "SHA256SUMS"), "w", encoding="utf-8") as f:
        f.write("\n".join(sums) + "\n")
    with open(os.path.join(DST, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)

    total = sum(v["bytes"] for v in manifest["images"].values())
    print("\nTOTAL %d bytes (%.2f GiB), %d parts" %
          (total, total / 1024 ** 3, len(sums)))


if __name__ == "__main__":
    main()
