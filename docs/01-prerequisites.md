# 01 · 前置条件与风险

> 本文是整条 root / 救砖链路的**入口检查清单**。任何一项不满足，都应停止，不要强行尝试后续步骤。
> 所有 IP、主机名、密码、序列号均已脱敏为占位符；硬件型号与软件版本按说明需要保留。

---

## 1. 适用基线

| 项目 | 值 |
|---|---|
| SoC 平台 | Realtek **RTD299x**（9R5x 平台，ARMv7 四核 Cortex-A53，**没有 arm64**） |
| Android | **5.1.1**（API 22，`LMY47V`，`test-keys` 签名） |
| 内核 | **3.10.19-mag2+**（`#SMP PREEMPT`，Realtek ASDK 编译） |
| 内存 / 存储 | 约 1.27 GB RAM；eMMC 约 8 GB（`mmcblk0`，512 B/扇区） |
| 访问方式 | 无线 ADB（TCP 5555），已授权 |
| 核心前提 | `/dev/rtkmem` 可读写且以 `0777` 暴露；SELinux 为 disabled |

本方法**不依赖** Bootloader 解锁、fastboot 或刷机。它利用的是厂商固件自身把整条物理内存通过 `/dev/rtkmem` 暴露给普通用户这一事实。

> 换一台设备（即使同型号、不同固件版本）都必须**从阶段一重新侦察**；本文与后续文档中的符号地址只对「本 firmware」有效。

---

## 2. 三项硬性前提

### 2.1 `/dev/rtkmem` 节点存在且普通 shell 可打开

```bash
adb connect <TV_IP>:5555
adb shell id
# uid=2000(shell) gid=2000(shell) ...

adb shell 'ls -l /dev/rtkmem'
# 期望：crw-rw-rw- root root ... /dev/rtkmem   （0777）

adb shell 'ls -l /dev/rtkmem /dev/rtk*'
```

必须是 `0777`（至少 `0666`）。如果只是 `0600` 或属于某个仅限厂商进程的组，普通 shell 的 `open()` 会返回 `EACCES`，整条链路到此为止。

进一步验证可打开性（后续 `rtprobe` 会做完整探测）：

```bash
adb shell 'exec 3<>/dev/rtkmem && echo OPEN_RW_OK || echo OPEN_FAIL'
```

### 2.2 SELinux 为 Disabled（或 Permissive）

```bash
adb shell getenforce
# 期望：Disabled   （Permissive 可继续，但要额外审查 avc 日志）
```

**为什么 Enforcing 不行**：

- `mmap()` 一个 `dev_type` 设备节点做 `PROT_READ|PROT_WRITE` 会被 SELinux 拒绝，即使 DAC 权限是 `0777`；
- 持久化阶段需要 `mount -o remount,rw /system`、写 `/system/etc` 和 `/system/xbin`，在 Enforcing 下都会被 policy 拦截；
- `setuid` 二进制也受 `nosuid` 之外的 SEAndroid 规则约束。

若为 Enforcing，不要尝试「先提权再 setenforce 0」——那正是本方法要避免的循环依赖。

### 2.3 无线 ADB 已获得授权

```bash
adb connect <TV_IP>:5555
adb devices
# <TV_IP>:5555    device
adb shell id        # 必须能进 shell，且是 uid=2000(shell)
```

电视端必须在开发者选项里开启「网络调试 / ADB over network」，并且已经在本机 `adb key` 上点过授权（或设备的 `adb_keys` 已包含你的公钥）。如果 `adb devices` 显示 `unauthorized`，先在电视上确认授权弹窗；这一步无法绕过。

> 本文所有命令中的 `<TV_IP>` 请替换为你电视在局域网内的实际地址；`<ADB>` 请替换为本机 `platform-tools/adb` 的路径。

---

## 3. 编译工具链：Android NDK r25b

所有配套 C 工具（`rtprobe` / `rtdump` / `rtwrite` / `rtfind2` / `rtroot` / `rtcaps` / `sotv_root` / `su` / `nclisten` 等）都用 **Android NDK r25b** 的 clang 交叉编译为 **ARMv7 静态 ELF**：

```bash
export NDK_ROOT=<NDK_ROOT>                       # 例如 android-ndk-r25b 的解压目录
export HOST_TAG=<HOST_TAG>                       # linux-x86_64 / darwin-x86_64 / windows-x86_64
export CC="$NDK_ROOT/toolchains/llvm/prebuilt/$HOST_TAG/bin/clang"

$CC --target=armv7a-linux-androideabi19 --static -O2 -o rtprobe rtprobe.c
```

三个参数的含义：

- `--target=armv7a-linux-androideabi19`：生成 32 位 ARM EABI（ARMv7）代码，API level 19。Android 5.1 是 API 22，用 19 可覆盖老 Bionic，避免依赖 21+ 才有的符号。
- `--static`：目标机上不保证有匹配的动态库；静态链接让工具成为自包含二进制，push 即可运行。
- `-O2`：扫描 / 解密热循环需要一定速度，同时保持体积可控。

**静态 ELF 的 PT_TLS 对齐坑**：NDK r25b / lld 生成的静态 ARM ELF 里 `PT_TLS` 段的 `p_align` 默认为 8，老 Bionic 加载器会报 `TLS segment underaligned` 并拒绝执行。编译后必须按 [`04-building.md`](04-building.md) 用 `patch_pt_tls.py` 把 `p_align` 改成 64。未 patch 的二进制 push 上去会「一运行就没反应」。

设备上放工具的位置用 `/data/local/tmp`：

```bash
adb push rtprobe rtdump rtwrite /data/local/tmp/
adb shell 'chmod 755 /data/local/tmp/rtprobe /data/local/tmp/rtdump /data/local/tmp/rtwrite'
```

---

## 4. 为什么必须先做完整备份

这是整份文档里**唯一不可跳过**的建议。

1. **写物理内存不是事务操作**。提权工具严格匹配 `struct cred` 特征才写，但内核里可能存在「看起来像 cred」的 look-alike 数据；一旦误写，轻则内核 oops 自动重启，重则破坏正在使用的内核对象。
2. **持久化会改 `/system`**：`remount,rw`、追加开机脚本、安装 setuid 二进制。任一步出错都可能导致开机失败或系统不稳定。
3. **`/data` 是活分区**：备份镜像只是时间点快照，不是文件系统一致性归档；但变砖后它往往是唯一能拿回的资料。
4. **变砖后的恢复只有一条可靠路径**：字节级分区镜像。没有镜像时，即使有 OTA 包也未必能救回被写坏的 boot / 分区表。

请在**任何写操作之前**完成 [`06-backup.md`](06-backup.md) 的全盘备份，并核对每个镜像的 SHA256。先在备用机上完整演练一遍，再动主力设备。

---

## 5. 风险与最坏情况

| 操作 | 最坏结果 | 可逆性 |
|---|---|---|
| 只读侦察 / dump | 进程崩溃（有 `SIGSEGV`/`SIGBUS` 保护） | 完全可逆 |
| 无害写测试（`kptr_restrict` 2→0→2） | 无权限变化；重启后复位 | 可逆 |
| 改 cred 提权 | 内核 oops → 自动重启；重启后干净 | 重启即恢复 |
| remount `/system` + 装 su/守护 | 开机脚本写坏 → 进不了系统 | 需进恢复环境按分区刷回 |
| 按分区刷回镜像 | 刷错分区 → 硬变砖 | 取决于是否有正确镜像 |

**安全边界**：本方法不写任何 flash 分区、不替换 init、不 patch boot 镜像。所有内存写都严格匹配结构特征；最坏情况是内核 oops 自动重启，而开机脚本会在重启后重新拉起守护。

**不要在承载重要账号、支付或工作数据的设备上操作。** 守护 `su` 是「单用户家用环境」的简化实现，没有授权 UI，任何本机 App 都能拿到 root。

---

## 6. 法律与免责声明

- 本资料**仅用于你自己拥有合法所有权的设备**的研究与维护。
- 修改系统可能违反厂商保修条款、使设备失去安全更新，或因误操作导致设备永久不可用。
- 请遵守所在国家/地区关于设备修改、信号处理与软件安装的法律法规。
- 作者不对任何设备损坏、数据丢失、保修失效或滥用行为承担责任。
- 继续即表示你已理解并自行承担全部风险。

---

## 7. 开工前核对清单

```bash
adb connect <TV_IP>:5555
adb shell id                                   # uid=2000(shell)
adb shell getenforce                           # Disabled / Permissive
adb shell 'ls -l /dev/rtkmem'                  # crw-rw-rw-
adb shell 'cat /proc/version'                  # 3.10.19-mag2+
adb shell 'grep -i "System RAM\|Kernel code" /proc/iomem'
adb shell 'cat /proc/sys/kernel/kptr_restrict'  # 通常是 2
```

- [ ] 备份已完成，SHA256 已核对（[`06-backup.md`](06-backup.md)）
- [ ] 备用机已演练
- [ ] 工具已用 NDK r25b 编译并 **patch 过 PT_TLS**
- [ ] 已记录原始 `/system/etc/init.tvserver.sh` 的备份路径

下一步：[`02-recon.md`](02-recon.md) —— 只读侦察。
