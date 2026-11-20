# RTD299x TV Root & Rescue Kit

一份针对 **Realtek RTD299x / Android 5.1 智能电视**的免拆机 root、持久化与全盘救砖备份的完整实录与工具集。

本项目记录了一次真实操作：从「只有一个无线 ADB shell」开始，利用厂商自身暴露的 `/dev/rtkmem` 设备节点，**不写任何分区、不刷机、不拆机**拿到运行中内核的读写能力，定位内核符号、改写进程 cred 提权，再把 root 持久化到 `/system`，最后做字节级全盘备份与各类系统定制。

> ⚠️ **法律与安全声明**：本资料仅用于你**自己拥有合法所有权**的设备的研究与维护。修改系统可能违反厂商保修条款、使设备失去安全更新、或因误操作导致设备不可用。请先在备用机上验证，并**务必先做完整备份**。作者不对任何设备损坏、数据丢失或滥用行为负责。

---

## 目录

- [1. 适用设备与环境](#1-适用设备与环境)
- [2. 它做了什么 / 原理概览](#2-它做了什么--原理概览)
- [3. 阶段一：只读侦察](#3-阶段一只读侦察)
- [4. 阶段二：内核 dump 与符号还原](#4-阶段二内核-dump-与符号还原)
- [5. 阶段三：改写 cred 提权](#5-阶段三改写-cred-提权)
- [6. 阶段四：持久化 root](#6-阶段四持久化-root)
- [7. 阶段五：系统定制](#7-阶段五系统定制)
- [8. 全盘救砖备份与恢复](#8-全盘救砖备份与恢复)
- [9. 回退](#9-回退)
- [10. 工具与脚本清单](#10-工具与脚本清单)

详细分步见 [`docs/`](docs/) 目录。

---

## 1. 适用设备与环境

本项目针对的具体设备（实测）：

| 项目 | 值 |
|---|---|
| 厂商 / 型号 | Skyworth 酷开 TV（9R5x / rtd299o 平台） |
| SoC | Realtek RTD299x（ARMv7 四核 Cortex-A53，约 1.0 GHz，无 arm64） |
| Android | 5.1.1（API 22），`LMY47V`，`test-keys` 签名 |
| 内核 | `3.10.19-mag2+`，`#SMP PREEMPT`，Realtek ASDK 4.9.2 编译 |
| 内存 | 约 1.27 GB RAM + 128 MB swap |
| 存储 | eMMC 约 8 GB（`mmcblk0`，512 B/扇区） |
| 关键前提 | `/dev/rtkmem` 可读写且以 0777 暴露；SELinux 为 disabled |
| ADB | 无线 ADB（TCP 5555），已获取授权 |

**核心前提**：`getenforce` 返回 `Disabled`（或 `Permissive`），且 `/dev/rtkmem` 存在可对普通 shell 打开。这在这类广电定制机上很常见（厂商自己的 root 服务用它）。**如果你的设备没有这个节点或 SELinux 强制开启，本方法不适用。**

示例 App（本项目配套的学习软件）版本：`v0.5.1 (versionCode 48)`。

> 注：文中所有 IP、管理密码、主机名、序列号均已脱敏为占位符（如 `<TV_IP>`、`ADB_HOST`）。硬件型号与软件版本为说明必要性而保留。

---

## 2. 它做了什么 / 原理概览

整条链路的核心是一个事实：**厂商固件把整条物理内存通过 `/dev/rtkmem` 暴露给了普通用户**。`mmap()` 这个节点的 offset 就是**物理字节地址**，于是我们能在用户态任意读写物理内存。

四步走：

1. **只读侦察**：映射内核代码区，确认读到的是活内核；dump 前 16 MB 物理内存；从镜像里还原出内核自带的 `kallsyms` 符号表，拿到 `commit_creds` / `prepare_kernel_cred` / `sys_call_table` 等真实地址。
2. **提权**：在物理内存里扫描 `struct cred`（特征：8 个连续的 uid/gid 字段都等于 shell 的 2000），把这些字段清零、把 capability 位图填满。由于 adbd 派生 shell 子进程时沿用同一份 cred，于是**每个新的 adb shell 都直接是 uid=0 且能力全开**。
3. **持久化**：利用 root 临时把 `/system` 重新挂载为可写，安装开机守护 + setuid `su`，在厂商开机脚本末尾追加一行拉起守护。重启后守护每 60 秒重扫一次 cred，`adbd` 即便重启也无法丢掉 root。
4. **备份与定制**：root 状态下流式 dd 全部分区到 PC（lzop 压缩 + 逐帧校验），并做去广告、关自动升级、开放 APK 安装、开机自动进入指定 App 等系统定制。

**安全性设计**：所有内存写操作都严格匹配 cred 结构特征才执行，不写任何 flash 分区、不替换 init、不 patch boot 镜像。最坏情况是内核 oops 自动重启，重启后状态干净（开机脚本会重新拉起守护）。

---

## 3. 阶段一：只读侦察

先确认前提，全程**只读**：

```bash
adb connect <TV_IP>:5555
adb shell id                       # 仍是 uid=2000(shell)
adb shell getenforce               # 期望 Disabled / Permissive
adb shell 'ls -l /dev/rtkmem'      # 期望 crw-rw-rw- root root
adb shell 'cat /proc/iomem | grep -i "kernel\|System RAM"'
```

`rtprobe.c` 用来验证 mmap 语义：

- offset = **物理字节地址**（不是页帧号 pfn；用 pfn 会 EINVAL）
- 映射 `0x00108000`（内核代码起点）读到的第一条指令是 ARM 的 `safe_svcmode_maskall` 入口 → 证明读到的是正在运行的内核
- 内核数据区出现 `0xc0c9xxxx` 一类指针 → 推断 `PAGE_OFFSET = 0xC0000000`

编译（用 NDK 的 clang）：

```bash
clang --target=armv7a-linux-androideabi19 --static -O2 -o rtprobe rtprobe.c
# 老 Bionic 要求 PT_TLS 的 p_align >= 页面大小，否则报 "TLS segment underaligned"
# 见 docs/04-building.md 的 patch_pt_tls.py
adb push rtprobe /data/local/tmp/ && adb shell /data/local/tmp/rtprobe
```

> 关键坑：静态链接的 ARM ELF 在 Android 5.1 上若 PT_TLS 的 `p_align` 为 8，动态加载器会拒绝执行。必须 patch 成 64。

---

## 4. 阶段二：内核 dump 与符号还原

`rtdump.c` 按页对齐、分块只读地把物理内存写到文件：

```bash
adb shell '/data/local/tmp/rtdump /data/local/tmp/kernel.bin 0 1000000'   # 前 16 MB
adb pull /data/local/tmp/kernel.bin .
```

在 dump 里搜索 `Linux version` 字符串，应与 `getprop`/`build.prop` 里的内核版本一致，确认抓对了当前固件。

`find_kallsyms.py` 从内核镜像里还原 kallsyms 表：内核把符号地址表、名字表、token 表都编进了镜像。脚本定位这些表并遍历，输出 `symbols_recovered.txt`（本例 6 万多个符号的真实虚拟地址）。对本项目，关键符号为：

| 符号 | 虚拟地址（本 firmware） |
|---|---|
| `sys_call_table` | `0xc0113a88` |
| `commit_creds` | `0xc018d40c` |
| `prepare_kernel_cred` | `0xc018d998` |
| `init_task` | `0xc0c962c8` |
| `kptr_restrict` | `0xc0c8f6cc` |

交叉验证方法：`sys_call_table[3]` / `[4]` 应分别是 `sys_read` / `sys_write`（ARM EABI 的调用号 3、4）；`kptr_restrict` 处读出的值应等于 `cat /proc/sys/kernel/kptr_restrict`。

再用 `rtwrite.c` 做一次**无害写验证**：仅把 `kptr_restrict` 从 2 改成 0，然后从**独立路径**（sysctl 读取 + `/proc/kallsyms`）确认写真的落到了物理内存，再立刻改回 2。这一步能在动 cred 之前证明写通道有效。

---

## 5. 阶段三：改写 cred 提权

`rtfind2.c` 在物理内存里扫描 `struct cred` 特征：

```
[+0x00] usage        （引用计数，合理范围 1..1e6）
[+0x04..+0x20] 8 个 uid/gid 全等于 AID_SHELL(2000)
[+0x24] securebits == 0
尾部有 user_struct / group_info 等内核指针（结构校验）
```

`rtroot.c` 对命中的每个 cred：

- 把 `+0x04..+0x20` 的 8 个 uid/gid 字段清零 → uid/gid 全变 0
- 把原本为 0 的 capability 字置为全 1（保守：不覆盖非零字段）

执行后验证：

```bash
adb shell /data/local/tmp/rtroot
adb shell id                  # uid=0(root) gid=0(root)
adb shell 'dd if=/dev/block/mmcblk0p7 of=/dev/null bs=4096 count=1'  # 能读裸分区
```

**注意**：此时 root **只在内存里**，电视一重启就失效。它是一次性的即时 root 会话。

---

## 6. 阶段四：持久化 root

目标是不碰 boot 分区（最危险），只利用厂商已有的开机脚本。

1. 补满 capability：Android 默认 drop 掉了 bounding set 里的 `CAP_SYS_ADMIN`，所以要先把 cred 的 `cap_bset` 也填满（`rtcaps.c`）。否则 `mount -o remount` 会被拒。
2. remount `/system` 为可写（fstab 只有 `ro`、没有 `nosuid`，所以 setuid `su` 能生效）。
3. 安装两个组件：
   - `/system/xbin/sotv_root`（0755）：开机守护，扫 cred 提权并补满 caps，每 60 秒重跑，`adbd` 重启也不会丢 root
   - `/system/xbin/su`（06755，setuid）：任何 App 调用即可拿 root
4. 在厂商 `/system/etc/init.tvserver.sh` **末尾追加一行**：
   ```sh
   /system/xbin/sotv_root --daemon >/dev/null 2>&1 &
   ```
5. `sync`，把 `/system` 恢复为只读。

**踩过的坑**：
- `su` 用 `setuid(0)` 只改 effective uid，exec 之后 real uid 会被内核重置回 2000 → 必须用 `setresuid(0,0,0)` 把三个 uid 都钉成 0。
- 守护的 cred 签名必须和提权工具**完全一致**；加严尾部校验反而会导致开机漏检。

真重启验证：`id` → `uid=0`，`CapEff/CapBnd` 全开，守护进程由 init 托管。

---

## 7. 阶段五：系统定制

拿到持久 root 后可以做的事（每项都**改前备份、可回退**）：

- **去开机广告**：删除缓存视频 `/data/local/bootvideo.ts`、`pm disable` 广告服务、在 `/system/etc/hosts` 屏蔽广告域名族。注意 Play 厂商会重置 hosts，需确认重启后仍在。
- **关自动升级**：把厂商配置里的升级服务器指向 `127.0.0.1` + hosts 屏蔽升级域名，保留手动升级入口。
- **开放自由安装 APK**：把 `APP_SELF_INSTALL` 设为 true、`package_verifier_enable` 设为 0（后者正是 `INSTALL_FAILED_VERIFICATION_FAILURE` 的根源）。
- **开机自动进入指定 App**：新增一个 bring-up 脚本，等 `sys.boot_completed` 后的一段时间内持续检查前台，不是目标 App 就 `am start`。只按包名判断，不强指定 Activity，以免破坏孩子端的锁屏管控。

厂商配置通常位于 `/system/pcfg/<机型>/config/general_config.xml`（XML 式的 `<config name="..." value="..."/>`）。改之前确认设备实际读取哪一份机型目录。

---

## 8. 全盘救砖备份与恢复

见 [`backups/README.md`](backups/README.md) 与 [`docs/06-backup.md`](docs/06-backup.md)。

要点：

- 由于 LAN 有 AP 隔离（TV→PC 连不通、PC→TV 可连），用设备端 `nclisten` + PC 端主动连的方式传流，比 `adb exec-out` 快 5~7 倍。
- 用 `busybox lzop -1` 在设备端压缩（比 gzip 快约 10 倍，对全零区压缩率极高）。
- 完整性用**逐 lzop 帧校验**（每块声明的压缩长度、终止块、解压总长 == 分区精确字节数），避免把几 GB 回灌设备做 hash。
- `/proc/partitions` 以 1 KB 为单位且对奇数扇区分区**向下取整**，精确大小要从 `/sys/class/block/<part>/size × 512` 取。

本仓库 `backups/` 里的分片由 `scripts/split_images.py` 切成 90 MiB，用 `scripts/merge.py` 校验并重组。

---

## 9. 回退

```bash
# 移除 root 持久化（保留 /system 原样）
adb root   # 或用你的临时提权工具先拿到 root
adb shell 'mount -o rw,remount /system
           cp /system/etc/init.tvserver.sh.sotv_bak /system/etc/init.tvserver.sh
           rm -f /system/xbin/su /system/xbin/sotv_root /system/xbin/sotv_bringup.sh
           sync; mount -o ro,remount /system'
adb reboot
```

完整救砖（变砖时）用备份镜像从 fastboot/恢复环境按分区刷回，参见 `docs/06-backup.md`。**动手前务必确认每个镜像的 SHA256**。

---

## 10. 工具与脚本清单

`tools/src/`（C 源码，用 NDK clang 交叉编译为 ARMv7 静态 ELF；PT_TLS 需 patch）：

| 文件 | 作用 |
|---|---|
| `rtprobe.c` | 探测 `/dev/rtkmem` 的 mmap 语义，只读读内核头 |
| `rtdump.c` | 按页对齐分块 dump 物理内存 |
| `find_kallsyms.py` | 从内核镜像还原 kallsyms 符号表 |
| `rtperm.c` / `rtwrite.c` | 验证 RW mmap / 无害写测试（改 `kptr_restrict`） |
| `rtfind.c` / `rtfind2.c` | 通过 comm / cred 特征定位 cred 结构 |
| `rtroot.c` | 改写 cred 即时提权 |
| `rtcaps.c` | 补满 capability bounding set |
| `sotv_root.c` | 持久化开机守护 |
| `su.c` | setuid root 替代品（`setresuid`） |
| `drop2000.c` | 测试辅助：派生一个 uid=2000 进程 |
| `nclisten.c` / `ncsend.c` | TCP 流式传输（绕过 ADB 带宽瓶颈） |
| `vecdump.c` | 早期向量表探测实验 |

`scripts/`：备份/切割/合并/收尾校验脚本与开机 bring-up 脚本（IP 与路径均已脱敏为占位符，使用前按注释修改）。

`docs/`：分步详解与编译注意事项。

`configs/`：脱敏后的配置样例（pcfg / hosts / init 片段），仅作格式参考。

---

## 致谢与免责

这些技术来自公开的内核提权原理与社区实践。再次强调：仅用于自有设备的合法研究与维护。
