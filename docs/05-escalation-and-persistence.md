# 05 · 提权与持久化

本阶段从「物理内存可读写」走到「每次开机自动 root」。核心思路不是 hook 系统调用，而是**直接改运行中的 `struct cred`**：

> `adbd` 派生 shell 子进程时沿用同一份 cred（`copy_cred` 只增加引用计数）。因此只要把这份 cred 的 uid/gid 清零、capability 填满，**之后每个新的 `adb shell` 都直接是 uid=0 且能力全开**。

全程不写任何 flash 分区、不替换 init、不 patch boot 镜像。

---

## 1. `struct cred` 的内存布局（Linux 3.10 / ARM32）

| 偏移 | 字段 | 特征 |
|---|---|---|
| `+0x00` | `atomic_t usage` | 引用计数，合理范围 `1..1000000` |
| `+0x04` | `uid` | 全部等于 `AID_SHELL = 2000` |
| `+0x08` | `gid` | 同上 |
| `+0x0c` | `suid` | 同上 |
| `+0x10` | `sgid` | 同上 |
| `+0x14` | `euid` | 同上 |
| `+0x18` | `egid` | 同上 |
| `+0x1c` | `fsuid` | 同上 |
| `+0x20` | `fsgid` | 同上 |
| `+0x24` | `securebits` | `0` |
| `+0x28..+0x47` | 4 组 `kernel_cap_t`（inh / prm / eff / bset），每组 2 个 `u32` | 共 8 个 32 位字 |
| `+0x48..` | `user_struct` / `group_info` / RCU 指针 | `0xc0xxxxxx` 形态的内核指针，用于结构校验 |

八个连续的 uid/gid 字段（偏移 `+0x04` 到 `+0x23`）是最强的特征：普通内核数据里几乎不可能出现 8 个连续、都等于 2000 的 `u32`。

---

## 2. 阶段 A：只读定位 cred（`rtfind2`）

```bash
adb push rtfind2 /data/local/tmp/ && adb shell 'chmod 755 /data/local/tmp/rtfind2'
adb shell id                         # 确认此时还是 uid=2000
adb shell /data/local/tmp/rtfind2
```

`rtfind2` 的行为：

- 以 `O_RDONLY` 打开 `/dev/rtkmem`，按 1 MiB 分块 `mmap(PROT_READ)` 扫描物理地址 `0..0x20000000`；
- 对每个 4 字节对齐位置读 10 个字，要求：
  - `w[0]`（usage）在 `1..1000000`；
  - `w[1..8]`（8 个 uid/gid）**全部**等于当前 shell 的 uid（2000）；
  - `w[9]`（securebits）为 0；
- 命中则打印 `phys` / `virt`（`virt = phys + 0xC0000000`）、8 个字段值和 8 个 capability 字；
- 最多打印 20 个候选；**只写 stdout，不写内存**。

输出示例：

```
[*] rtfind2 READ-ONLY: uid=2000 pid=...
[+] CRED CANDIDATE #1 phys=0x000xxxxx virt=0xc0xxxxxx
    usage=3  uid..fsgid: 2000 2000 2000 2000 2000 2000 2000 2000  securebits=0
    cap words: 00000000 00000000 00000000 00000000 ...
[*] total cred candidates: N
[*] nothing written
```

如果 0 个候选：先确认 `getenforce`、节点权限、以及 shell 确实是 2000；不要进入下一步。

---

## 3. 阶段 B：改写 cred 即时提权（`rtroot`）

```bash
adb push rtroot /data/local/tmp/ && adb shell 'chmod 755 /data/local/tmp/rtroot'
adb shell /data/local/tmp/rtroot

adb shell id
# uid=0(root) gid=0(root) groups=...

adb shell 'dd if=/dev/block/mmcblk0p7 of=/dev/null bs=4096 count=1'
# 能读裸 boot 分区，证明 root 生效
adb shell 'cat /proc/self/status | grep -E "Cap(Inh|Prm|Eff|Bnd)"'
```

`rtroot` 对每个与 `rtfind2` **完全相同特征**的 cred 执行：

1. `+0x04..+0x23` 的 8 个 uid/gid 字段**清零** → uid/gid 全部变成 0；
2. 对 capability 字采取**保守策略**：只把原本为 `0` 的字置为 `0xffffffff`，不覆盖非零字段（避免破坏内核已设定的值）；
3. `__sync_synchronize()` + `msync(MS_SYNC)` 确保写入对内核可见。

写之前**二次校验** cred 特征，写之后再校验一次；任何一项不符就跳过该候选。这是防止误伤 look-alike 数据的关键。

> **此时 root 只在内存里**。电视一重启，cred 随进程重建，root 立即失效。这是一次性即时 root 会话。

---

## 4. 阶段 C：补满 capability（解决 `CAP_SYS_ADMIN` 被 drop）

### 4.1 问题

`rtroot` 之后 `id` 显示 root 了，但：

```bash
adb shell 'cat /proc/self/status | grep Cap'
# CapInh: 0000000000000000
# CapPrm: 0000000000000000
# CapEff: 0000000000000000
# CapBnd: 00000000000000c0      <-- 只剩 CAP_SETGID(6) 和 CAP_SETUID(7)
```

Android 5.1 的 `adbd` 在初始化时把 bounding set 里大部分 capability 丢掉了，其中就包括 **`CAP_SYS_ADMIN (21)`**。后果是：

```bash
adb shell 'mount -o rw,remount /system'
# mount: permission denied (EPERM)
```

`rtroot` 保守地「只填原本为 0 的 cap 字」在这里不够——bounding set 的限制需要显式补满 `cap_bset`。

### 4.2 解决：`rtcaps`

`rtcaps` 扫描**已经提权**的 cred，特征为：

- 8 个 uid/gid 字段全为 0；
- `securebits == 0`；
- 至少 4 个 capability 字等于 `0xffffffff`（`rtroot` 留下的指纹，用来区分真 cred 和内核里其它数组）；
- 尾部 `+0x60..+0x68` 至少有一个是 `0xc0000000..0xf0000000` 形态的指针。

然后对命中的 cred 把 `+0x28..+0x47` 的**四组 capability（inh / prm / eff / bset）全部写成 `0xffffffff`**：

```bash
adb push rtcaps /data/local/tmp/ && adb shell 'chmod 755 /data/local/tmp/rtcaps'

# 先只看不写
adb shell '/data/local/tmp/rtcaps --dry'

# 实际补满，并在同一进程内立刻尝试 remount
adb shell /data/local/tmp/rtcaps
# [*] patched N creds (all 4 cap sets -> full)
# [*] mount(MS_REMOUNT without RDONLY) -> 0 SUCCESS /system is now RW
# [*] restored ro -> 0 OK
```

**关键点**：`rtcaps` 调用 `mount(NULL, "/system", NULL, MS_REMOUNT, NULL)` 时**不 `execve`**。一旦 exec 新程序，内核会按 setuid/capability 规则重算进程的能力，刚补满的 `CapEff/CapBnd` 又会按要求收缩。成功 remount 后脚本立刻用 `MS_REMOUNT|MS_RDONLY` 恢复只读（持久化阶段会再打开一次）。

---

## 5. 阶段 D：持久化到 `/system`

目标：不碰 boot 分区，只利用厂商已有的开机脚本。

### 5.1 前置条件

- `/system` 的 fstab 是 `ro`、`wait`，**没有 `nosuid`** → setuid 二进制能生效；
- 已用 `rtcaps` 拿到可 remount 的能力。

### 5.2 安装步骤

```bash
adb shell <<'SH'
set -e
mount -o rw,remount /system

# 1) 备份厂商原始开机脚本（务必保留，回退要用）
cp -p /system/etc/init.tvserver.sh /system/etc/init.tvserver.sh.sotv_bak

# 2) 安装开机守护（0755）与 setuid su（06755）
cp /data/local/tmp/sotv_root /system/xbin/sotv_root
chmod 0755 /system/xbin/sotv_root
cp /data/local/tmp/su /system/xbin/su
chmod 06755 /system/xbin/su

# 3) 在厂商脚本末尾追加一行；grep -q 防止重复追加
if ! grep -q '/system/xbin/sotv_root --daemon' /system/etc/init.tvserver.sh; then
  echo '/system/xbin/sotv_root --daemon >/dev/null 2>&1 &' \
    >> /system/etc/init.tvserver.sh
fi

sync
mount -o ro,remount /system
SH
```

追加的那一行就是全部开机注入：

```sh
/system/xbin/sotv_root --daemon >/dev/null 2>&1 &
```

### 5.3 两个必须注意的坑

**坑 1：`su` 必须用 `setresuid(0,0,0)`，不能用 `setuid(0)`**

setuid-root 程序被 `execve` 时，`euid` 变成 0，但 **`real uid` 仍是调用者的 2000**。随后 `execve` 一个非 setuid 程序（比如 `/system/bin/sh`）时，内核会把 `euid` 重置回 `real uid`，root 悄悄丢失。

```c
/* 正确：三个 uid / gid 全部钉成 0，再 exec */
setresgid(0, 0, 0);
setresuid(0, 0, 0);
execv("/system/bin/sh", av);
```

`su.c` 已按此实现，并支持 `su`、`su -c "cmd"`、`su <cmd> args` 三种用法。

**坑 2：守护的 cred 签名必须和提权工具完全一致**

`sotv_root` 扫描时用的特征必须与 `rtroot` / `rtcaps` **逐字段一致**。如果「加强」尾部指针校验（比如要求所有尾部指针都合法），反而会漏掉真实 cred，导致开机后漏检、root 丢失。不要自行收紧匹配条件。

### 5.4 守护行为

`/system/xbin/sotv_root --daemon` 由厂商开机脚本以 root 身份拉起：

- 扫描 cred → 清零 uid/gid → 补满四组 capability；
- 每 **60 秒**重扫一次；
- 即使 `adbd` 被重启、shell 子进程重建，下一轮扫描会重新提权，root 不会丢。

---

## 6. 阶段 E：真重启验证

**不要只靠内存态的 `id` 判断成功**，必须真重启：

```bash
adb reboot
# 等待电视完全起来、无线 ADB 重新可用
adb connect <TV_IP>:5555

adb shell id
# uid=0(root) gid=0(root)

adb shell 'cat /proc/self/status | grep -E "Cap(Inh|Prm|Eff|Bnd)"'
# CapInh/CapPrm/CapEff/CapBnd 全为 0xffffffff

adb shell 'ps | grep sotv_root | grep -v grep'
# 守护进程存在，且父进程是 init

adb shell 'su -c id'
# uid=0(root)

adb shell 'dd if=/dev/block/mmcblk0p7 of=/dev/null bs=4096 count=1'
# 裸分区可读
```

只有 `id` 为 uid=0、CapEff/CapBnd 全开、守护由 init 托管三项同时满足，才算持久化成功。

> 重启后无线 ADB 授权一般需要重新确认（取决于设备是否保留 `adb_keys`）。若 `unauthorized`，先在电视端确认授权弹窗。

---

## 7. 回退（移除持久化）

```bash
adb root    # 或用临时提权工具先拿到 root
adb shell 'mount -o rw,remount /system
           cp /system/etc/init.tvserver.sh.sotv_bak /system/etc/init.tvserver.sh
           rm -f /system/xbin/su /system/xbin/sotv_root /system/xbin/sotv_bringup.sh
           sync
           mount -o ro,remount /system'
adb reboot
```

这一步只恢复 `/system`，不动其它分区。若要完整救砖，用 [`06-backup.md`](06-backup.md) 的镜像按分区刷回。

---

## 8. 安全边界小结

- 所有内存写都严格匹配 cred 特征，不匹配就跳过；
- 不写任何 flash 分区、不替换 init、不 patch boot 镜像；
- 最坏情况是内核 oops 自动重启，重启后状态干净，开机脚本会重新拉起守护；
- `su` 是无授权 UI 的简化实现，适用于单用户家用环境——任何本机 App 都能拿到 root，请勿在不可信 App 环境下使用。
