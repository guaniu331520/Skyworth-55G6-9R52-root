# 02 · 只读侦察详解

本阶段的目标是回答三个问题，且**全程只读**：

1. `mmap("/dev/rtkmem", offset)` 里的 `offset` 到底是**物理字节地址**还是页帧号（pfn）？
2. 映射到的内存真的属于**当前正在运行的内核**吗？
3. 内核的虚拟地址与物理地址如何换算（`PAGE_OFFSET`）？

只读工具即便判断错了，最坏也只是进程 `SIGSEGV`/`SIGBUS` 崩溃——源码里对每个可能 fault 的映射都装了信号保护并用 `sigsetjmp`/`siglongjmp` 跳过。**不会写任何一个字节。**

---

## 1. 前提复查

```bash
adb connect <TV_IP>:5555
adb shell id                                # uid=2000(shell)
adb shell getenforce                        # Disabled / Permissive
adb shell 'ls -l /dev/rtkmem'               # crw-rw-rw-
adb shell 'cat /proc/version'               # Linux version 3.10.19-mag2+ ...
```

---

## 2. 从 `/proc/iomem` 找内核代码起点

```bash
adb shell 'cat /proc/iomem | grep -iE "System RAM|Kernel code|Kernel data"'
```

典型形态（数值为示意，实际以你的设备为准）：

```
00000000-3fffffff : System RAM
  00108000-00xxxxxx : Kernel code
  00xxxxxx-00cxxxxx : Kernel data
```

我们要的是 `Kernel code` 的起始物理地址，本 firmware 是 **`0x00108000`**。这就是后面 `rtprobe` 的 `KCODE_PHYS` 常量。

> 注意：`/proc/iomem` 在 `kptr_restrict` 较严的设备上可能隐藏部分区间；如果看不到 `Kernel code`，用 `Linux version` 字符串在 dump 里定位（见 [`03-kernel-symbols.md`](03-kernel-symbols.md)）。

---

## 3. `rtprobe`：验证 mmap offset = 物理字节地址

### 3.1 两种竞争假设

`remap_pfn_range()` 这类驱动通常把 `vma->vm_pgoff`（即 `offset >> PAGE_SHIFT`）当作物理页帧号。但 Realtek 的这个节点可能自己实现了 `mmap`，把 `offset` 直接当物理地址。源码同时试了几种解释：

| 假设 | offset 取值 | 预期 |
|---|---|---|
| A：物理字节地址 | `0x00108000` | `MAP_SHARED` 成功，读到 ARM 指令 |
| B：页帧号 pfn | `0x00108000 >> 12 = 0x108` | 大概率 `EINVAL` 或读到无关页 |
| C：offset 0 从物理 0 开始 | `0` | 可读但不对应内核代码 |
| D：交叉验证 | `0x00c7a000`（内核数据区） | 读到 `0xc0xxxxxx` 一类内核指针 |

编译与运行（编译参数的 PT_TLS 坑见 [`04-building.md`](04-building.md)）：

```bash
clang --target=armv7a-linux-androideabi19 --static -O2 -o rtprobe rtprobe.c
python3 patch_pt_tls.py rtprobe           # 必须！把 PT_TLS p_align 改成 64
adb push rtprobe /data/local/tmp/
adb shell 'chmod 755 /data/local/tmp/rtprobe'
adb shell /data/local/tmp/rtprobe
```

### 3.2 如何判定「读到的是活内核」

成功的输出形态大致是：

```
[*] rtprobe: READ-ONLY /dev/rtkmem mmap semantics probe
[*] uid=2000 gid=2000  KCODE_PHYS=0x00108000
  A phys-addr MAP_SHARED  offset=0x00108000 -> MAP OK @0x..., read=OK
      words: e10f9000 e12cf000 ...
      ARM-opcode-like words: 7/8   <-- looks like kernel text
  B pfn MAP_SHARED        offset=0x00000108 -> mmap FAILED errno=22 (EINVAL)
  D kdata phys MAP_SHARED offset=0x00c7a000 -> MAP OK @0x..., read=OK
      words: c0c962c8 c0c8f6cc ...
[*] done (no writes performed)
```

判据有两条，互相印证：

1. **A 成功、B 失败** → `offset` 是**物理字节地址**。用 pfn 会 `EINVAL`，证实假设 A。
2. **在 `0x00108000` 读到密集的 ARM 指令**（条件码字段为 `0xA`–`0xE` 的字，例如 `0xE1..`/`0xE3..`/`0xE5..`/`0xEA..`），而不是全零或随机数据 → 这是正在执行的内核代码区，不是未使用的物理页。

### 3.3 内核代码入口特征：`safe_svcmode_maskall`

ARM Linux 3.10 的内核入口 `stext` 一开始就执行安全模式切换宏 `safe_svcmode_maskall`（同一时期也叫 `setmode`），展开后是典型的：

```asm
mrs     r9, cpsr            @ 机器码形如 0xE10F9000
bic     r9, r9, #0x1f
orr     r9, r9, #0xd3       @ SVC 模式 + 关 IRQ/FIQ
msr     cpsr_c, r9          @ 机器码形如 0xE129F000
```

所以在内核代码起点看到以 `0xE10F9000`（`mrs r9, cpsr`）/ `0xE12x...`（`msr`）开头、后续连续多个 `0xE` 条件码 ARM 指令的序列，就是 `safe_svcmode_maskall` 入口特征，证明：

- 映射偏移语义正确；
- 读到的是**当前运行内核**的代码，而不是某份残留镜像。

> 不同 3.10 小版本宏展开的指令条数会有差异，不要把「第一条必须是某个固定值」当成唯一判据。更稳妥的是结合 `/proc/iomem` 的 `Kernel code` 区间与后面的 `Linux version` 字符串校验。

---

## 4. 推断 `PAGE_OFFSET = 0xC0000000`

在 `0x00c7a000` 一带（内核数据 / `init_task` 附近）读到的字里出现大量 `0xc0c9xxxx`、`0xc0c8xxxx` 形式的值：

```
words: c0c962c8 c0c8f6cc c0c...... ...
```

这些是内核持有的**虚拟地址指针**。它们与所在物理地址的差值是固定的：

```
0xc0c962c8 (virtual) - 0x00c962c8 (physical) = 0xC0000000
```

因此：

```
virt = phys + PAGE_OFFSET
PAGE_OFFSET = 0xC0000000
```

这也符合 ARM32 经典的 3G/1G 地址空间划分：用户空间 `0x00000000`–`0xBFFFFFFF`，内核空间从 `0xC0000000` 开始。后续所有符号换算都按此公式，例如：

- `kptr_restrict` 虚拟地址 `0xc0c8f6cc` → 物理地址 `0x00c8f6cc`
- `init_task` 虚拟地址 `0xc0c962c8` → 物理地址 `0x00c962c8`

> 如果某台设备的内核数据指针不是 `0xc0......` 形式，说明 `PAGE_OFFSET` 不同（例如 `0x80000000` 的 2G/2G 划分），必须重新推断，不能套用本项目的常量。

---

## 5. `kptr_restrict`：为什么 `/proc/kallsyms` 地址全 0

```bash
adb shell 'cat /proc/sys/kernel/kptr_restrict'
# 2

adb shell 'grep " sys_call_table\| commit_creds\| kptr_restrict" /proc/kallsyms'
# 00000000 A sys_call_table
# 00000000 T kptr_restrict
# ... 名字可见，地址全是 0
```

`/proc/kallsyms` 的**名字**可读，但 `kptr_restrict=2` 会把非特权进程看到的**地址**全部打成 0。这正是下一步要从物理内存 dump 里还原 kallsyms 表的原因：

- 内核镜像里编译进了完整的 `kallsyms_addresses`（真实地址）、`kallsyms_names`（token 压缩的名字表）、`kallsyms_token_table` 等常量表；
- `kptr_restrict` 只影响 `/proc/kallsyms` 的**打印**，不影响这些表在内存里的存在；
- 只读 dump 就能拿到真实地址，无需提权。

读取 `sysctl` 本身是常规只读操作，可作为后面「写通道验证」的独立观测路径。

---

## 6. 本阶段产出与边界

| 产出 | 值 / 结论 |
|---|---|
| 内核代码物理起点 | `0x00108000` |
| mmap offset 语义 | 物理字节地址（非 pfn） |
| 内核入口特征 | `safe_svcmode_maskall` 展开的 ARM 指令流 |
| `PAGE_OFFSET` | `0xC0000000` |
| 符号可见性 | `/proc/kallsyms` 名字可读、地址为 0（`kptr_restrict=2`） |

本阶段**没有写任何内存或分区**。下一步：[`03-kernel-symbols.md`](03-kernel-symbols.md) —— 把物理内存 dump 下来，还原真实符号地址。
