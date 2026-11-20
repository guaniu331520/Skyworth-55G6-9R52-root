# 04 · 编译指南与 PT_TLS 对齐坑

所有配套工具都是 **ARMv7 静态 ELF**，用 Android NDK **r25b** 的 clang 交叉编译。编译本身只有一行命令，但**编译后必须 patch `PT_TLS` 的 `p_align`**，否则二进制 push 到 Android 5.1 上根本跑不起来。

---

## 1. 编译命令

```bash
export NDK_ROOT=<NDK_ROOT>                       # android-ndk-r25b 解压目录
export HOST_TAG=<HOST_TAG>                       # linux-x86_64 / darwin-x86_64 / windows-x86_64
export CC="$NDK_ROOT/toolchains/llvm/prebuilt/$HOST_TAG/bin/clang"

# Windows 主机用 clang.cmd；路径用正斜杠，例如：
# export CC="<NDK_ROOT>/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.cmd"

$CC --target=armv7a-linux-androideabi19 --static -O2 -o rtprobe rtprobe.c
$CC --target=armv7a-linux-androideabi19 --static -O2 -o rtdump  rtdump.c
$CC --target=armv7a-linux-androideabi19 --static -O2 -o rtwrite rtwrite.c
$CC --target=armv7a-linux-androideabi19 --static -O2 -o rtfind2 rtfind2.c
$CC --target=armv7a-linux-androideabi19 --static -O2 -o rtroot  rtroot.c
$CC --target=armv7a-linux-androideabi19 --static -O2 -o rtcaps  rtcaps.c
$CC --target=armv7a-linux-androideabi19 --static -O2 -o sotv_root sotv_root.c
$CC --target=armv7a-linux-androideabi19 --static -O2 -o su      su.c
$CC --target=armv7a-linux-androideabi19 --static -O2 -o nclisten nclisten.c
$CC --target=armv7a-linux-androideabi19 --static -O2 -o ncsend   ncsend.c
```

参数说明：

| 参数 | 原因 |
|---|---|
| `--target=armv7a-linux-androideabi19` | 目标机是 32 位 ARM EABI（Cortex-A53，**无 arm64**）。API 19 覆盖 Android 5.1 的 API 22，且避免依赖 21+ 的 Bionic 符号。 |
| `--static` | 目标机不保证有匹配的 C++ / C 运行库；静态链接让工具自包含，push 即可运行。 |
| `-O2` | cred 扫描是逐页热循环，需要优化；同时控制体积。 |

批量 patch 与安装：

```bash
for f in rtprobe rtdump rtwrite rtfind2 rtroot rtcaps sotv_root su nclisten ncsend; do
  python3 patch_pt_tls.py "$f"        # 见第 2 节
done

adb push rtprobe rtdump rtwrite rtfind2 rtroot rtcaps sotv_root su nclisten ncsend \
        /data/local/tmp/
adb shell 'chmod 755 /data/local/tmp/rtprobe /data/local/tmp/rtdump \
            /data/local/tmp/rtwrite /data/local/tmp/rtfind2 /data/local/tmp/rtroot \
            /data/local/tmp/rtcaps /data/local/tmp/sotv_root /data/local/tmp/su \
            /data/local/tmp/nclisten /data/local/tmp/ncsend'
```

---

## 2. PT_TLS `p_align` 坑（关键）

### 2.1 现象

编译成功、文件也是 ARM ELF，但一 push 到设备上执行就：

```
ERROR: TLS segment underaligned
```

或者更隐蔽：命令**没有任何输出**就直接结束（linker 在 `execve` 阶段拒绝了二进制）。`file` 看着正常，`adb shell` 也不报「cannot execute binary file」，很容易误判成工具本身有 bug。

### 2.2 原因

老 Bionic 的动态加载器对 `PT_TLS` 程序头的对齐有硬性要求：`p_align` 过小时（NDK r25b 的 lld 默认写 **8**），加载器判定 TLS 段「underaligned」，拒绝装载。注意即使链接方式是 `--static`，Android 的静态可执行文件初始化路径仍会走这段 TLS 检查。

社区常把这条要求概括为「`p_align` 至少要达到页面大小（4096）」；但对本项目用 NDK r25b 生成的这批二进制，**实测把 `p_align` 改成 64 即可通过校验并正常运行**（64 是大于 8 的 2 的幂，满足该 Bionic 版本的对齐约束）。若你的设备用 64 仍报同样错误，按文末提示把参数改成 4096 再试。

### 2.3 诊断

用 NDK 自带的 `llvm-readelf`（或任意 `readelf`）：

```bash
readelf -l rtprobe | grep -A1 TLS
# 或
<NDK_ROOT>/toolchains/llvm/prebuilt/<HOST_TAG>/bin/llvm-readelf -l rtprobe | grep -A1 TLS

# 期望修复前：
#   TLS     0x000000 0x...... 0x...... 0x..... 0x.....  R   0x8
#                                                       ^^^^ 问题在这里
```

### 2.4 修复脚本 `patch_pt_tls.py`

保存为 `patch_pt_tls.py`。它直接改 ELF32 文件：读 program header 表，找到 `PT_TLS`（类型 7）的表项，把 `p_align`（`Elf32_Phdr` 内偏移 **+28**）改为 64。

```python
#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""patch_pt_tls.py — 修复 NDK/lld 生成的 ARM ELF32 静态二进制的 PT_TLS p_align。

用法:
    python3 patch_pt_tls.py <elf32-file> [align]

默认 align = 64（本项目实测可用）。若设备仍报 TLS segment underaligned，
试 4096：python3 patch_pt_tls.py <elf32-file> 4096
"""
import struct
import sys

PT_TLS = 7                 # ELF program header 类型
PHDR_MIN_SIZE = 32         # sizeof(Elf32_Phdr)
P_ALIGN_OFF = 28           # p_align 在 Elf32_Phdr 内的字节偏移

ELFCLASS32, ELFDATA2LSB = 1, 1


def main():
    if len(sys.argv) < 2:
        print("usage: %s <elf32-file> [align=64]" % sys.argv[0])
        return 2

    path = sys.argv[1]
    align = int(sys.argv[2], 0) if len(sys.argv) > 2 else 64
    if align <= 0 or (align & (align - 1)):
        print("align must be a positive power of two")
        return 2

    with open(path, "r+b") as f:
        ident = f.read(16)
        if (ident[:4] != b"\x7fELF"
                or ident[4] != ELFCLASS32
                or ident[5] != ELFDATA2LSB):
            print("not a little-endian ELF32 file")
            return 2

        # Elf32_Ehdr: e_type,e_machine,e_version,e_entry,e_phoff
        f.seek(16)
        _type, _machine, _version, _entry, e_phoff = struct.unpack(
            "<HHIII", f.read(16))
        # e_shoff,e_flags,e_ehsize,e_phentsize,e_phnum
        f.seek(32)
        _shoff, _flags = struct.unpack("<II", f.read(8))
        _ehsize, e_phentsize, e_phnum = struct.unpack("<HHH", f.read(6))

        if e_phoff == 0 or e_phentsize < PHDR_MIN_SIZE:
            print("no usable program header table")
            return 2

        patched = 0
        for i in range(e_phnum):
            off = e_phoff + i * e_phentsize
            f.seek(off)
            ph = bytearray(f.read(e_phentsize))
            if len(ph) < PHDR_MIN_SIZE:
                print("truncated program header #%d" % i)
                return 2
            p_type, = struct.unpack_from("<I", ph, 0)
            if p_type != PT_TLS:
                continue
            old, = struct.unpack_from("<I", ph, P_ALIGN_OFF)
            struct.pack_into("<I", ph, P_ALIGN_OFF, align)
            f.seek(off)
            f.write(ph)
            print("PT_TLS phdr #%d: p_align 0x%x -> 0x%x" % (i, old, align))
            patched += 1
        f.flush()

    if patched == 0:
        print("[!] no PT_TLS segment found; nothing changed")
        return 1
    print("[+] patched %d PT_TLS segment(s) in %s" % (patched, path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

### 2.5 验证修复

```bash
python3 patch_pt_tls.py rtprobe
# PT_TLS phdr #N: p_align 0x8 -> 0x40

readelf -l rtprobe | grep -A1 TLS
#   TLS  ...  R   0x40

adb push rtprobe /data/local/tmp/
adb shell 'chmod 755 /data/local/tmp/rtprobe && /data/local/tmp/rtprobe'
# 正常打印探测结果，不再出现 TLS segment underaligned
```

> 备选：如果 64 在你的设备上不生效，运行 `python3 patch_pt_tls.py rtprobe 4096`。该脚本对同一文件重复运行是安全的：第二次会把 `p_align` 从 64/4096 改成同一个值。

---

## 3. 常见报错速查

| 报错 | 原因 | 处理 |
|---|---|---|
| `TLS segment underaligned` / 静默退出 | `PT_TLS p_align` 太小（默认 8） | 跑 `patch_pt_tls.py`，先 64，不行再 4096 |
| `cannot execute: required file not found` | 编译成了动态链接，或架构不对 | 确认加了 `--static` 与 `--target=armv7a-...` |
| `mmap FAILED errno=22 (EINVAL)` | offset 语义搞错（用了 pfn） | 见 [`02-recon.md`](02-recon.md)：offset 是物理字节地址 |
| `open rtkmem: Permission denied` | 节点权限或 SELinux 不满足 | 见 [`01-prerequisites.md`](01-prerequisites.md) |
| 执行到一半 `SIGSEGV` | 映射了不可读物理页 | 工具内置信号保护会跳过；若频繁发生检查 dump 范围 |

---

## 4. 下一步

工具编译并 patch 完成后：

- 侦察 / 符号还原流程见 [`02-recon.md`](02-recon.md)、[`03-kernel-symbols.md`](03-kernel-symbols.md)；
- 提权与持久化见 [`05-escalation-and-persistence.md`](05-escalation-and-persistence.md)；
- 全盘备份见 [`06-backup.md`](06-backup.md)。
