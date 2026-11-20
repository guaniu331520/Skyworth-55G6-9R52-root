# -*- coding: utf-8 -*-
"""Full byte-level eMMC backup over a direct PC->TV TCP socket.

The LAN blocks TV->PC connects (AP isolation) but allows PC->TV, so the TV
runs `nclisten` (stdin -> listening socket) and the PC connects to it.

Pipeline per partition:
  TV:  dd if=/dev/block/X bs=1M | busybox lzop -1 | nclisten <port>
  PC:  connect(<TV_IP>, <port>) -> recv -> <part>.img.lzo

Integrity is checked locally by walking the lzop frame structure:
every block's declared compressed size must fit, the stream must end with
the zero-length terminator, and the sum of uncompressed block sizes must
equal the partition's exact byte size. That proves a complete, untruncated
stream without shipping gigabytes back to the TV for a loopback hash.
"""
import json, os, socket, struct, subprocess, sys, time, zlib

# --- EDIT THESE FOR YOUR OWN SETUP ---
ADB = r"C:\path\to\platform-tools\adb.exe"
DEV = "<TV_IP>:5555"          # e.g. 192.168.x.x:5555
DEV_IP = "<TV_IP>"
BB = "/system/rtk_rootfs/bin/busybox"
OUT = r"C:\path\to\backup\emmc"
MANIFEST = os.path.join(OUT, "backup_manifest.json")
BASE_PORT = 21000
MAGIC = b"\x89LZO\x00\x0d\x0a\x1a\x0a"

# Exact byte sizes from /sys/class/block/<part>/size * 512.
# NOTE: /proc/partitions reports 1KB blocks and FLOORS odd-sector partitions,
# so p1/p5 are 512 bytes larger than kb*1024 would suggest.
PARTS = [
    ("mmcblk0p7", 16769024,    "boot (unmounted)"),
    ("mmcblk0p3", 134209536,   "/cache"),
    ("mmcblk0p6", 134209536,   "/dtv"),
    ("mmcblk0p5", 231449088,   "/skyworth"),
    ("mmcblk0p1", 1142169088,  "/system (ext4 ro)"),
    ("mmcblk0p8", 1342169088,  "/skyota"),
    ("mmcblk0p2", 4607434752,  "/data (ext4 rw)"),
]


def adb(args, **kw):
    return subprocess.run([ADB, "-s", DEV] + args, **kw)


def lzo_check(path, expected):
    """Return (ok, uncompressed_total, blocks, flags_hex, detail)."""
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        if f.read(9) != MAGIC:
            return False, 0, 0, "", "bad lzop magic"
        h = f.read(2 + 2 + 2 + 1 + 1)          # version..level
        if len(h) < 8:
            return False, 0, 0, "", "truncated header"
        flags = struct.unpack(">I", f.read(4))[0]
        # optional filter field
        if flags & 0x00000800:
            f.read(4)
        f.read(4 + 4 + 4)                       # mode, mtime_low, mtime_high
        nlen = f.read(1)[0]
        f.read(nlen)
        f.read(4)                               # header checksum
        # checksum field sizes from lzop flags
        def cksz(fl):
            n = 0
            if fl & 0x00000001: n += 4          # adler32 of uncompressed
            if fl & 0x00000002: n += 4          # adler32 of compressed
            if fl & 0x00000100: n += 4          # crc32 of uncompressed
            if fl & 0x00000200: n += 4          # crc32 of compressed
            return n
        csz = cksz(flags)
        total = 0
        blocks = 0
        while True:
            hdr = f.read(4)
            if len(hdr) < 4:
                return False, total, blocks, hex(flags), "truncated block size"
            usize = struct.unpack(">I", hdr)[0]
            if usize == 0:
                break
            csize_raw = f.read(4)
            if len(csize_raw) < 4:
                return False, total, blocks, hex(flags), "truncated comp size"
            csize = struct.unpack(">I", csize_raw)[0]
            sums = f.read(csz)
            if len(sums) < csz:
                return False, total, blocks, hex(flags), "truncated checksums"
            data = f.read(csize)
            if len(data) < csize:
                return False, total, blocks, hex(flags), "truncated block data"
            # verify compressed-data checksum when lzop recorded one
            if flags & 0x00000002:
                want = struct.unpack(">I", sums[:4])[0]
                if zlib.adler32(data) & 0xffffffff != want:
                    return False, total, blocks, hex(flags), "adler32 mismatch in block %d" % blocks
            elif flags & 0x00000200:
                want = struct.unpack(">I", sums[-4:])[0]
                if zlib.crc32(data) & 0xffffffff != want:
                    return False, total, blocks, hex(flags), "crc32 mismatch in block %d" % blocks
            total += usize
            blocks += 1
        tail = f.tell()
    ok = (total == expected)
    return ok, total, blocks, hex(flags), (
        "OK" if ok else "length %d != expected %d" % (total, expected)
    ) + " (file=%d, consumed=%d)" % (size, tail)


def backup_one(name, nbytes, desc, port, force=False):
    expected = nbytes
    lzo = os.path.join(OUT, name + ".img.lzo")
    if not force and os.path.exists(lzo):
        ok, total, blocks, flags, detail = lzo_check(lzo, expected)
        if ok:
            print("[=] %-12s already complete, %d blocks, %s" % (name, blocks, detail), flush=True)
            return {"bytes": expected, "lzo_bytes": os.path.getsize(lzo),
                    "blocks": blocks, "flags": flags, "desc": desc,
                    "verified": "lzop frame walk: uncompressed length == partition size"}
        print("[~] %-12s existing file incomplete (%s), re-streaming" % (name, detail), flush=True)

    remote = ("dd if=/dev/block/%s bs=1048576 2>/dev/null | %s lzop -1 | "
              "/data/local/tmp/nclisten %d" % (name, BB, port))
    print("\n=== %-12s %-20s raw=%d bytes  port=%d ===" % (name, desc, expected, port), flush=True)
    proc = subprocess.Popen([ADB, "-s", DEV, "shell", remote],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    sock = None
    for _ in range(60):
        try:
            sock = socket.create_connection((DEV_IP, port), timeout=3)
            break
        except OSError:
            if proc.poll() is not None:
                break
            time.sleep(0.25)
    if sock is None:
        e = proc.stderr.read().decode(errors="replace")[:300] if proc.poll() is not None else "connect timeout"
        print("  CONNECT FAILED: %s" % e, flush=True)
        proc.kill()
        return None
    sock.settimeout(180)
    t0 = time.time()
    n = 0
    with open(lzo, "wb") as f:
        while True:
            try:
                b = sock.recv(262144)
            except socket.timeout:
                print("  recv timeout", flush=True)
                break
            if not b:
                break
            f.write(b)
            n += len(b)
    sock.close()
    try:
        proc.wait(timeout=120)
        rc = proc.returncode
    except subprocess.TimeoutExpired:
        proc.kill()
        rc = -1
    dt = time.time() - t0
    speed = n / 1048576 / max(dt, 0.01)
    print("  streamed %d bytes in %.1fs (%.2f MiB/s), device rc=%d" % (n, dt, speed, rc), flush=True)
    ok, total, blocks, flags, detail = lzo_check(lzo, expected)
    print("  lzop frames: %s blocks=%s flags=%s -> %s" %
          ("OK" if ok else "FAIL", blocks, flags, detail), flush=True)
    if not ok:
        return None
    return {"bytes": expected, "lzo_bytes": n, "blocks": blocks, "flags": flags,
            "seconds": round(dt, 1), "speed_MiB_s": round(speed, 2), "desc": desc,
            "verified": "lzop frame walk: uncompressed length == partition size; "
                        "per-block compressed checksum verified where present"}


def main():
    os.makedirs(OUT, exist_ok=True)
    subprocess.run([ADB, "connect", DEV], capture_output=True, timeout=30)
    r = adb(["shell", "id"], capture_output=True, timeout=30)
    ident = r.stdout.decode(errors="replace").strip()
    print("[*] device identity:", ident[:80], flush=True)
    if "uid=0" not in ident:
        print("[!] ROOT LOST - abort (re-run step 3 first)", flush=True)
        return 2

    manifest = {}
    if os.path.exists(MANIFEST):
        try:
            manifest = json.load(open(MANIFEST, encoding="utf-8"))
        except Exception:
            pass

    force = "--force" in sys.argv
    only = None
    for a in sys.argv[1:]:
        if a.startswith("--only="):
            only = a.split("=", 1)[1].split(",")
    failures = []
    for i, (name, nbytes, desc) in enumerate(PARTS):
        if only and name not in only:
            continue
        key = name + ".img.lzo"
        if not force and key in manifest and os.path.exists(os.path.join(OUT, key)):
            print("[=] %-12s already in manifest, skipping" % name, flush=True)
            continue
        res = backup_one(name, kb, desc, BASE_PORT + i, force=force)
        if res:
            manifest[key] = res
            manifest["_meta"] = {
                "device": "Skyworth Coocaa 55G6 / 9R52_G6 / rtd299o",
                "fingerprint": "Skyworth/rtd299o_tv001/rtd299o_tv001:5.1.1/LMY47V/www-data05181212:user/test-keys",
                "kernel": "3.10.19-mag2+ #26 SMP PREEMPT Fri May 18 12:30:20 2018",
                "method": "root (rtkmem cred patch) -> dd | busybox lzop -1 | nclisten -> PC TCP socket",
                "partition_map": {"p1": "/system", "p2": "/data", "p3": "/cache",
                                  "p5": "/skyworth", "p6": "/dtv", "p7": "boot", "p8": "/skyota"},
                "restore": "decompress .img.lzo with lzop -d, then dd of=/dev/block/<part> bs=1048576",
                "note": "/data is a live rw partition; its image is a point-in-time snapshot. "
                        "boot0/boot1 are genuinely all-zero on this device.",
                "updated": time.strftime("%Y-%m-%d %H:%M:%S"),
            }
            json.dump(manifest, open(MANIFEST, "w", encoding="utf-8"),
                      ensure_ascii=False, indent=2)
        else:
            failures.append(name)

    print("\n[*] DONE. failures=%s" % (failures or "none"), flush=True)
    for k, v in manifest.items():
        if not k.startswith("_"):
            print("    %-24s raw=%-12s lzo=%s" %
                  (k, v.get("bytes"), v.get("lzo_bytes", v.get("size"))), flush=True)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
