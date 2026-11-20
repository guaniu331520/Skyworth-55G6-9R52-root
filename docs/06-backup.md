# 06 · 完整备份与恢复

**在任何写操作之前完成本文的备份。** 这是唯一能让变砖可逆的手段。备份全程只读分区，不在 TV 上做任何修改。

---

## 1. 分区映射与精确字节数

| 设备节点 | 挂载点 / 用途 | 精确字节数（`sysfs size × 512`） |
|---|---|---|
| `/dev/block/mmcblk0p1` | `/system`（ext4，ro） | `1142169088` |
| `/dev/block/mmcblk0p2` | `/data`（ext4，rw，用户数据） | `4607434752` |
| `/dev/block/mmcblk0p3` | `/cache` | `134209536` |
| `/dev/block/mmcblk0p5` | `/skyworth`（出厂分区） | `231449088` |
| `/dev/block/mmcblk0p6` | `/dtv` | `134209536` |
| `/dev/block/mmcblk0p7` | **boot**（未挂载） | `16769024` |
| `/dev/block/mmcblk0p8` | `/skyota` | `1342169088` |

`p7` 是 boot，优先级最高，额外保留了一份 **raw 镜像**（`mmcblk0p7.img`）并做过设备端逐字节校验。`p4` 在这台设备上不存在；eMMC 的 `boot0/boot1` 区域实际全零，无需备份。

> `/data` 是活分区，它的镜像只是**时间点快照**，不是文件系统一致性归档。这点要写进备份记录。

---

## 2. AP 隔离下的传输方向

局域网开了 **AP 隔离**：电视 → PC 的连接被阻断，PC → 电视可以连。因此不能用 `adb exec-out` 直接拉流（慢且受 ADB 通道限制），而是：

- **设备端**跑 `nclisten`：把 `stdin` 收到的数据发到监听端口；
- **PC 端**主动 `connect("<TV_IP>", <PORT>)` 接收并写文件。

每个分区的管道：

```
TV :  dd if=/dev/block/<PART> bs=1M 2>/dev/null | <BB> lzop -1 | /data/local/tmp/nclisten <PORT>
PC :  socket.create_connection(("<TV_IP>", <PORT>)) -> recv -> <PART>.img.lzo
```

这套方式比 `adb exec-out` 快 **5~7 倍**。`<BB>` 是设备上的 busybox 路径（例如 `/system/rtk_rootfs/bin/busybox`），按实际修改。

准备工具：

```bash
adb push nclisten <BB_BUSYBOX> /data/local/tmp/
adb shell 'chmod 755 /data/local/tmp/nclisten'

# 在设备上确认 busybox 带 lzop
adb shell '<BB> lzop --version'
```

---

## 3. 为什么用 `busybox lzop -1`

- 设备端 CPU 弱，压缩速度是瓶颈：`lzop -1` 比 `gzip` 快约 **10 倍**；
- eMMC 里大量全零区域，lzop 压缩率极高，最终镜像远小于分区原始大小；
- `-1` 是最快档，适合「先完整备份下来」这个目标。

> `lzop` 是**流式**格式，多个块拼成一条流；这正好让我们能在 PC 上做逐帧校验（第 4 节），而不必把几 GB 数据回灌设备做 hash。

---

## 4. lzop 逐帧校验原理

### 4.1 格式结构

一个 `.lzo` 文件由以下部分顺序组成：

1. **文件头**：9 字节 magic `\x89LZO\x00\x0d\x0a\x1a\x0a`，随后是版本、压缩级别、`flags`、可选字段、原文件名、头校验；
2. **若干数据块**：每块 = 4 字节**未压缩长度** + 4 字节**压缩长度** + 若干校验字段（由 `flags` 决定是 adler32 / crc32 的压缩和/或未压缩值）+ 压缩数据；
3. **终止块**：未压缩长度为 `0` 的 4 字节，表示流结束。

`flags` 中决定每块校验字段长度的位：

| 位 | 含义 | 字节数 |
|---|---|---|
| `0x00000001` | 未压缩数据的 adler32 | 4 |
| `0x00000002` | 压缩数据的 adler32 | 4 |
| `0x00000100` | 未压缩数据的 crc32 | 4 |
| `0x00000200` | 压缩数据的 crc32 | 4 |

### 4.2 校验步骤

1. 校验 magic，解析头部的 `flags` 与可选字段，算出每块的校验字节数；
2. 逐块读取长度对，检查文件没有在块中间截断；
3. 若记录了压缩数据的 adler32/crc32，就地对压缩数据验算；
4. 累加每块的**未压缩长度**；
5. 读到长度为 0 的终止块；
6. **累加值必须恰好等于该分区的精确字节数**。

这就同时证明了「流没有被截断」和「解压后总长与分区一致」，而不需要把镜像传回 TV 做一次几 GB 的 loopback hash。

### 4.3 可直接用的校验函数

```python
import os, struct, zlib

MAGIC = b"\x89LZO\x00\x0d\x0a\x1a\x0a"

def lzo_check(path, expected):
    """返回 (ok, uncompressed_total, blocks, flags_hex, detail)。"""
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        if f.read(9) != MAGIC:
            return False, 0, 0, "", "bad lzop magic"
        f.read(8)                                   # version..level
        flags = struct.unpack(">I", f.read(4))[0]
        if flags & 0x00000800:                      # 可选 filter 字段
            f.read(4)
        f.read(4 + 4 + 4)                           # mode, mtime_low, mtime_high
        nlen = f.read(1)[0]
        f.read(nlen)                                # 原文件名
        f.read(4)                                   # 头校验

        def cksz(fl):
            n = 0
            for bit in (0x1, 0x2, 0x100, 0x200):
                if fl & bit:
                    n += 4
            return n
        csz = cksz(flags)

        total = blocks = 0
        while True:
            hdr = f.read(4)
            if len(hdr) < 4:
                return False, total, blocks, hex(flags), "truncated block size"
            usize = struct.unpack(">I", hdr)[0]
            if usize == 0:                          # 终止块
                break
            raw = f.read(4)
            if len(raw) < 4:
                return False, total, blocks, hex(flags), "truncated comp size"
            csize = struct.unpack(">I", raw)[0]
            sums = f.read(csz)
            if len(sums) < csz:
                return False, total, blocks, hex(flags), "truncated checksums"
            data = f.read(csize)
            if len(data) < csize:
                return False, total, blocks, hex(flags), "truncated block data"
            if flags & 0x00000002:
                if zlib.adler32(data) & 0xffffffff != struct.unpack(">I", sums[:4])[0]:
                    return False, total, blocks, hex(flags), "adler32 mismatch in block %d" % blocks
            elif flags & 0x00000200:
                if zlib.crc32(data) & 0xffffffff != struct.unpack(">I", sums[-4:])[0]:
                    return False, total, blocks, hex(flags), "crc32 mismatch in block %d" % blocks
            total += usize
            blocks += 1
        consumed = f.tell()

    ok = (total == expected)
    detail = "OK" if ok else "length %d != expected %d" % (total, expected)
    return ok, total, blocks, hex(flags), "%s (file=%d, consumed=%d)" % (
        detail, size, consumed)
```

用法：

```bash
python3 -c 'from lzo_check import lzo_check; print(lzo_check("mmcblk0p7.img.lzo", 16769024))'
# (True, 16769024, N, '0x...', 'OK (file=..., consumed=...)')
```

仓库里的 `scripts/backup_tcp.py` 在流结束时自动调用 `lzo_check`；`scripts/finalize_backup.py` 会对**小的 boot 分区**做额外的设备端逐字节校验：

```bash
# 设备端：原始分区 sha256
adb shell 'dd if=/dev/block/mmcblk0p7 bs=1M 2>/dev/null | <BB> sha256sum'

# 注意：adb shell 的 stdin 是 PTY，会破坏二进制回传！
# 必须把 .lzo push 回设备，再用 push 后的文件做解压校验：
adb push mmcblk0p7.img.lzo /data/local/tmp/verify_boot.lzo
adb shell '<BB> lzop -d < /data/local/tmp/verify_boot.lzo | <BB> sha256sum'
```

两个 sha256 相等才把 boot 镜像标记为 `verified: byte-exact`。

---

## 5. `/proc/partitions` 的 1 KB 向下取整坑

```bash
adb shell cat /proc/partitions
# major minor  #blocks  name
#  ...      1142169 mmcblk0p1      <-- 单位是 1 KiB，且对奇数扇区向下取整
```

`/proc/partitions` 以 **1 KiB（1024 字节）**为单位，并且对扇区数为奇数的分区（本机的 `p1` / `p5`）**向下取整**。于是：

```
真实字节数 = 1142169088
/proc/partitions 推算 = 1142169 * 1024 = 1142167296
                                   差值 = 512 字节
```

若用 `/proc/partitions` 的值做 `lzo_check` 的 `expected`，校验会永远失败（少 512 字节）。

**精确字节数从 sysfs 取**（`size` 单位是 512 字节扇区）：

```bash
adb shell 'for p in /sys/class/block/mmcblk0p*; do \
             echo "$(basename $p) $(( $(cat $p/size) * 512 ))"; \
           done'
```

把结果写进备份清单（manifest）。后续所有校验、切割、恢复都用这里的精确值。

---

## 6. 备份执行与 SHA256SUMS

### 6.1 完整备份流程（概念）

对每个分区依次：

1. TV 端启动 `dd | lzop -1 | nclisten <PORT>`；
2. PC 端 `connect("<TV_IP>", <PORT>)` 收流，保存为 `<PART>.img.lzo`；
3. 本地运行 `lzo_check(path, exact_size)`，失败就重传；
4. 通过则写入 `backup_manifest.json`（记录字节数、压缩后大小、块数、flags、描述）。

端口按分区递增（例如 `21000 + i`），每个分区用一个新端口，避免串流。

### 6.2 切割成可入库的分片

`scripts/split_images.py` 把镜像切成 **90 MiB** 分片（低于 GitHub 单文件 100 MiB 硬限制），并生成校验文件：

```bash
python3 scripts/split_images.py
# mmcblk0p1.img.lzo   1142169088 bytes  sha=...  parts=N
# ...
# TOTAL ... bytes (... GiB), N parts
```

产出：

- `<name>.part000`, `<name>.part001`, …（命名格式 `<原文件名>.partNNN`）
- `SHA256SUMS`：每行 `<sha256>  <分片名>`
- `manifest.json`：每个镜像的完整 sha256、字节数、每个分片的 sha256

校验分片完整性：

```bash
sha256sum -c SHA256SUMS
# <part>: OK
# ... 全部 OK 才能继续
```

### 6.3 重组：`merge.py`

```bash
# 只校验分片 sha256，不写出镜像
python3 scripts/merge.py --verify-only

# 校验 + 按序拼接 + 与原镜像 sha256 比对
python3 scripts/merge.py
# mmcblk0p7.img.lzo       reassembled ... bytes  sha256=...  OK
# ...
# RESULT: ALL GOOD
```

`merge.py` 的逻辑：

1. 读 `manifest.json`；
2. 对每个镜像，逐个校验分片文件的 SHA256（缺失 / 损坏直接报错）；
3. 按顺序拼接，同时计算拼接后的 SHA256；
4. 与 manifest 里记录的**原镜像完整 SHA256** 比对，不符就是 `MISMATCH!`；
5. 输出目录可用环境变量 `MERGE_OUT` 指定。

> 不要在 `sha256sum -c` 还没全绿时就运行 `merge.py` 写盘。

---

## 7. 变砖后按分区刷回

> ⚠️ **写分区是变砖高风险操作。** 刷入前必须再次确认每个镜像的 SHA256 与分区对应关系。刷错分区（例如把 `/data` 刷到 `p7`）会直接硬变砖。

前提：进入一个能写块设备的环境（恢复环境 / fastboot 下的临时系统 / 另一个 Android），并 root。

```bash
# 1) 重组并校验（见第 6 节）
python3 scripts/merge.py
sha256sum -c SHA256SUMS

# 2) 确认目标分区精确字节数与镜像一致
adb shell 'cat /sys/class/block/mmcblk0p7/size'   # 扇区数 × 512 == 16769024

# 3) 解压并写回（以 boot p7 为例）
adb push mmcblk0p7.img.lzo /data/local/tmp/
adb shell 'lzop -d < /data/local/tmp/mmcblk0p7.img.lzo | \
           dd of=/dev/block/mmcblk0p7 bs=1M'
```

通用形式：

```bash
lzop -d < <PART>.img.lzo | dd of=/dev/block/<PART> bs=1M
```

**建议顺序**：先 `p7`（boot，有 raw byte-verified 镜像）和 `p1`（`/system`），确认能开机后再恢复 `p2`（`/data`）、`p3`（`/cache`）、`p5`（`/skyworth`）、`p6`（`/dtv`）、`p8`（`/skyota`）。

`/data` 的镜像是快照，刷回会覆盖刷机后的所有用户数据——仅在确实需要时使用。

---

## 8. 备份完成核对清单

- [ ] 7 个分区的 `.img.lzo` 全部存在
- [ ] 每个 `.img.lzo` 的 `lzo_check` 返回 `ok=True` 且 `total == 精确字节数`
- [ ] `p7` 额外做过设备端 loopback sha256 逐字节校验
- [ ] `SHA256SUMS` 全绿，`merge.py` 返回 `ALL GOOD`
- [ ] `backup_manifest.json` 里记录了精确字节数、分区映射与恢复命令
- [ ] 镜像有**至少两份物理副本**（本机 + 离线介质）

下一步：拿到持久 root 后做系统定制，见 [`07-customization.md`](07-customization.md)。
