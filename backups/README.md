# 救砖备份分片（eMMC 全盘）

本项目 eMMC 全盘字节级备份，已切成小分片上传。

> 这些是**特定设备的固件镜像**，仅供本项目演示「如何做完整可恢复的救砖备份」以及原设备灾难恢复。刷入不兼容机型会变砖。

## 实际存放位置

分片位于 **[`backups20/`](../backups20/)** 目录（本目录 `backups/` 仅保留早期 90MiB 方案的说明与清单，分片已迁到 `backups20/`）。

采用 20 MiB 分片（GitHub Contents API 上传稳定），共 **204 个分片**：

| 原始镜像 | 分区内容 | 原始字节数 | SHA256（前16位） | 分片数 |
|---|---|---|---|---|
| `mmcblk0p1.img.lzo` | `/system` | 578,730,212 | `c4f717f19683f849` | 28 |
| `mmcblk0p2.img.lzo` | `/data` | 2,951,426,842 | `fad1dccc799005f4` | 141 |
| `mmcblk0p3.img.lzo` | `/cache` | 649,418 | `ca75dcbef2226a09` | 1 |
| `mmcblk0p5.img.lzo` | `/skyworth` (factory) | 54,511,993 | `6a977702014027ac` | 3 |
| `mmcblk0p6.img.lzo` | `/dtv` | 549,346 | `493c2c9064c7e495` | 1 |
| `mmcblk0p7.img.lzo` | boot（压缩） | 2,892,986 | `c47b52470ddb1a32` | 1 |
| `mmcblk0p7.img` | boot（原始，字节级验证过） | 16,769,024 | `16ff9c3000457996` | 1 |
| `mmcblk0p8.img.lzo` | `/skyota` | 573,319,367 | `2ba83cca5ee5368b` | 28 |

原始总量约 **3.89 GiB**；压缩后分片落盘。

## 关于 `part094.b64`

`mmcblk0p2.img.lzo.part094` 的二进制被 GitHub 的密钥扫描误报拦截（分片内含 SDK 的字段名字符串，被启发式规则当成密钥，实际无真实凭据）。为合规上传，该分片以 **base64 文本**形式存为 `part094.b64`。重组前先解码：

```bash
base64 -d mmcblk0p2.img.lzo.part094.b64 > mmcblk0p2.img.lzo.part094
```

## 校验与重组

`backups20/manifest20.json` 记录每个原始镜像的完整 SHA256 与每个分片的 SHA256；`backups20/SHA256SUMS20` 是校验清单。

用 `scripts/merge.py`（把它的 HERE 指向 `backups20/`，并把 part094.b64 先解码）校验并重组。核心流程：

```bash
# 1) 若存在 .b64 分片，先解码
base64 -d backups20/mmcblk0p2.img.lzo.part094.b64 > backups20/mmcblk0p2.img.lzo.part094

# 2) 校验每个分片 SHA256（对照 manifest20.json）
python3 scripts/merge.py --verify-only

# 3) 按 part 序号拼接回原始镜像，校验整体 SHA256
python3 scripts/merge.py
```

boot 分区（`mmcblk0p7.img`）做过**三重字节级验证**：设备端 `dd | sha256sum`、lzo 解压后的 `sha256sum`、本地原始镜像，三者完全一致（`16ff9c30…`），解压长度精确 16,769,024 字节。

## 变砖恢复思路

1. 重组出原始 `.img`；`.lzo` 的先在设备上 `busybox lzop -d` 还原。
2. 从恢复环境按分区刷回，例如 `dd if=mmcblk0p7.img of=/dev/block/mmcblk0p7 bs=4M`。
3. 刷回后先逐分区校验 SHA256，再重启。

⚠️ 分区号因机型而异，刷错分区（尤其 bootloader / boot）会导致无法启动。务必先用 `ls -l /dev/block/platform/*/by-name/` 确认映射。
