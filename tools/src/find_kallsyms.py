# -*- coding: utf-8 -*-
"""Recover kernel symbol addresses from the physical-memory dump.

The device has kptr_restrict=2, so /proc/kallsyms shows names but all-zero
addresses. However the running kernel image itself contains the full
kallsyms tables (kallsyms_addresses, kallsyms_names, token table...). This
script locates kallsyms_addresses in the dumped image by scanning for a long
monotonically-increasing run of 0xC0xxxxxx pointers, then maps the symbol
names (in /proc/kallsyms order) to real virtual addresses.

Mapping: virt = phys + 0xC0000000  (PAGE_OFFSET confirmed from kernel data
pointers like 0xc0c962c8 found at phys 0xc7a000).
"""
import struct, sys, os

DUMP = r"C:\TVROOT\tv-setup\firmware-backup-current\kernel_phys_0_16M.bin"
KALLSYMS = r"C:\TVROOT\tv-setup\recon\raw3\kallsyms.txt"
PAGE_OFFSET = 0xC0000000

def main():
    d = open(DUMP, "rb").read()
    names = []
    for line in open(KALLSYMS, encoding="utf-8", errors="replace"):
        parts = line.rstrip("\n").split(" ", 2)
        if len(parts) >= 3:
            names.append((parts[0], parts[1], parts[2]))
        elif len(parts) == 2:
            names.append((parts[0], parts[1], ""))
    print("[*] kallsyms lines:", len(names))

    # Find the largest run of increasing 0xC0000000..0xC1000000 u32 values.
    best = (0, 0)  # (start_offset, count)
    i = 0
    n = len(d) - 4
    while i < n:
        v = struct.unpack_from("<I", d, i)[0]
        if 0xC0000000 <= v <= 0xC1000000:
            j = i + 4
            prev = v
            cnt = 1
            while j < n:
                w = struct.unpack_from("<I", d, j)[0]
                if not (0xC0000000 <= w <= 0xC1000000) or w < prev:
                    break
                prev = w
                cnt += 1
                j += 4
            if cnt > best[1]:
                best = (i, cnt)
            i = j
        else:
            i += 4
    start, count = best
    print("[*] longest ascending 0xC0xxxxxx run: offset=0x%x count=%d" % (start, count))
    print("    => virtual addr of table: 0x%08x" % (start + PAGE_OFFSET))

    if count < 1000:
        print("[!] run too short, aborting")
        return 1

    # /proc/kallsyms order == kallsyms_addresses order. Align from the end:
    # usually count == number of symbols. Show the first few and search targets.
    addrs = [struct.unpack_from("<I", d, start + 4 * k)[0] for k in range(count)]
    targets = ["sys_call_table", "commit_creds", "prepare_kernel_cred",
               "init_task", "kptr_restrict", "vector_swi", "ptmx_fops",
               "sys_setresuid", "sys_read", "sys_write", "proc_kallsyms_operations"]
    print("\n[*] first 6 (addr, kallsyms line):")
    for k in range(min(6, count, len(names))):
        print("    0x%08x  %s" % (addrs[k], " ".join(names[k])))

    print("\n[*] assumed alignment: table[0] <-> kallsyms line[0]")
    idx = {nm: k for k, (a, t, nm) in enumerate(names)}
    for t in targets:
        if t in idx and idx[t] < count:
            print("    %-24s 0x%08x  (index %d)" % (t, addrs[idx[t]], idx[t]))
        else:
            print("    %-24s not resolvable" % t)

    # also verify by counting: are there more names than table entries?
    print("\n[*] names=%d table_entries=%d delta=%d" %
          (len(names), count, len(names) - count))

    out = r"C:\TVROOT\tv-setup\exploit-src\rtd299x-root\symbols_recovered.txt"
    with open(out, "w", encoding="utf-8") as f:
        for k in range(min(count, len(names))):
            a, t, nm = names[k]
            f.write("%08x %s %s\n" % (addrs[k], t, nm))
    print("[*] wrote recovered symbols ->", out)
    return 0

if __name__ == "__main__":
    sys.exit(main())
