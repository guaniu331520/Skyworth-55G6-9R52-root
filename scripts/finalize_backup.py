# -*- coding: utf-8 -*-
"""Finalize the eMMC backup: register any partitions whose stream finished
but whose manifest entry was skipped, byte-verify the critical boot image,
and emit a human-readable report.

Safe to run repeatedly. Read-only with respect to all partitions.
"""
import hashlib, json, os, subprocess, sys, time

sys.path.insert(0, r"C:\path\to\backup\emmc")  # directory containing backup_tcp.py
from backup_tcp import PARTS, lzo_check, ADB, DEV, BB, OUT, MANIFEST

EXPECTED = {n: b for n, b, _ in PARTS}


def shell(cmd, stdin=None, timeout=1800):
    r = subprocess.run([ADB, "-s", DEV, "shell", cmd], input=stdin,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=timeout)
    return r.stdout.decode("utf-8", "replace").strip(), r.returncode


def main():
    subprocess.run([ADB, "connect", DEV], capture_output=True, timeout=30)
    ident, _ = shell("id")
    print("[*] device:", ident[:70])
    if "uid=0" not in ident:
        print("[!] root lost - cannot byte-verify, but local images are unaffected")

    manifest = json.load(open(MANIFEST, encoding="utf-8")) if os.path.exists(MANIFEST) else {}

    # 1) register complete-but-unregistered .lzo files
    for name, nbytes, desc in PARTS:
        key = name + ".img.lzo"
        path = os.path.join(OUT, key)
        if key in manifest or not os.path.exists(path):
            continue
        ok, total, blocks, flags, detail = lzo_check(path, nbytes)
        print("[%s] %-12s frames=%s blocks=%s %s" %
              ("+" if ok else "!", name, ok, blocks, detail))
        if ok:
            manifest[key] = {
                "bytes": nbytes,
                "lzo_bytes": os.path.getsize(path),
                "blocks": blocks,
                "flags": flags,
                "desc": desc,
                "verified": "lzop frame walk: uncompressed length == exact sysfs partition size",
            }

    # 2) byte-exact loopback verification of the small, critical boot partition
    boot = os.path.join(OUT, "mmcblk0p7.img.lzo")
    if "uid=0" in ident and os.path.exists(boot):
        href, rc1 = shell("dd if=/dev/block/mmcblk0p7 bs=1048576 2>/dev/null | %s sha256sum" % BB)
        href = href.split()[0] if href else None
        # adb shell stdin is a PTY and mangles binary; push is byte-safe.
        subprocess.run([ADB, "-s", DEV, "push", boot,
                        "/data/local/tmp/verify_boot.lzo"],
                       capture_output=True, timeout=300)
        hdec, rc2 = shell("%s lzop -d < /data/local/tmp/verify_boot.lzo | %s sha256sum" % (BB, BB))
        hdec = hdec.split()[0] if hdec and not hdec.startswith("\x89") else None
        shell("rm -f /data/local/tmp/verify_boot.lzo")
        ok = bool(href and hdec and href == hdec)
        print("[%s] boot p7 byte-exact: raw=%s decompressed=%s" %
              ("+" if ok else "!", href, hdec))
        if ok:
            for k in ("mmcblk0p7.img.lzo", "mmcblk0p7.img"):
                if k in manifest:
                    manifest[k]["sha256"] = href
                    manifest[k]["verified"] = "device-side loopback: lzop -d | sha256sum == dd | sha256sum"

    manifest["_meta"] = dict(manifest.get("_meta", {}), **{
        "device": "Skyworth Coocaa 55G6 / 9R52_G6 / rtd299o (Android 5.1.1)",
        "fingerprint": "Skyworth/rtd299o_tv001/rtd299o_tv001:5.1.1/LMY47V/www-data05181212:user/test-keys",
        "kernel": "3.10.19-mag2+ #26 SMP PREEMPT Fri May 18 12:30:20 2018",
        "method": "root (rtkmem cred patch) -> dd | busybox lzop -1 | nclisten -> PC TCP socket",
        "partition_map": {"p1": "/system", "p2": "/data", "p3": "/cache",
                          "p5": "/skyworth", "p6": "/dtv", "p7": "boot", "p8": "/skyota"},
        "restore": "lzop -d <part>.img.lzo | dd of=/dev/block/<part> bs=1048576  (ONLY via root)",
        "restore_warning": "Writing partitions is a brick-risk operation. Verify hashes before any write.",
        "notes": [
            "Exact sizes come from /sys/class/block/<part>/size * 512; /proc/partitions floors odd-sector p1/p5 by 512 bytes.",
            "boot0/boot1 are genuinely all-zero on this device (unused eMMC boot areas).",
            "/data is a live rw partition: its image is a point-in-time snapshot, not a filesystem-consistent archive.",
            "Root is memory-only (cred patch); a TV reboot loses it, but these images survive on the PC.",
        ],
        "finalized": time.strftime("%Y-%m-%d %H:%M:%S"),
    })
    json.dump(manifest, open(MANIFEST, "w", encoding="utf-8"),
              ensure_ascii=False, indent=2)

    # 3) report
    print("\n=== FINAL BACKUP INVENTORY ===")
    total_raw = total_lzo = 0
    for k, v in sorted(manifest.items()):
        if k.startswith("_"):
            continue
        b = v.get("bytes", 0)
        z = v.get("lzo_bytes", v.get("size", 0)) or 0
        total_raw += b
        total_lzo += z
        print("  %-24s raw=%-12d compressed=%-12d %s" %
              (k, b, z, "sha256=" + v["sha256"][:16] if v.get("sha256") else v.get("verified", "")[:48]))
    print("  %-24s %d bytes raw -> %d bytes on disk (%.1f%%)" %
          ("TOTAL", total_raw, total_lzo, 100.0 * total_lzo / max(total_raw, 1)))
    missing = [n for n in EXPECTED if n + ".img.lzo" not in manifest]
    print("\n  missing partitions:", missing or "NONE")
    print("  manifest:", MANIFEST)
    return 0 if not missing else 1


if __name__ == "__main__":
    sys.exit(main())
