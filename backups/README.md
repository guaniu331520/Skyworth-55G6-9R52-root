# 救砖备份分片（eMMC 全盘）

本目录包含一台 RTD299x 电视 eMMC 全部可读分区的**字节级备份**，已按 GitHub 单文件上限切成 90 MiB 分片。

> 这些是**特定设备的固件镜像**，仅供本项目演示「如何做完整可恢复的救砖备份」以及原设备灾难恢复。刷入不兼容的机型会变砖。

## 文件清单

| 原始镜像 | 分区内容 | 原始字节数 | SHA256（前16位） | 分片数 |
|---|---|---|---|---|
| `mmcblk0p1.img.lzo` | `/system` | 578,730,212 | `c4f717f19683f849` | 7 |
| `mmcblk0p2.img.lzo` | `/data` | 2,951,426,842 | `fad1dccc799005f4` | 32 |
| `mmcblk0p3.img.lzo` | `/cache` | 649,418 | `ca75dcbef2226a09` | 1 |
| `mmcblk0p5.img.lzo` | `/skyworth` (factory) | 54,511,993 | `6a977702014027ac` | 1 |
| `mmcblk0p6.img.lzo` | `/dtv` | 549,346 | `493c2c9064c7e495` | 1 |
| `mmcblk0p7.img.lzo` | boot（压缩） | 2,892,986 | `c47b52470ddb1a32` | 1 |
| `mmcblk0p7.img` | boot（原始，字节级验证过） | 16,769,024 | `16ff9c3000457996` | 1 |
| `mmcblk0p8.img.lzo` | `/skyota` | 573,319,367 | `2ba83cca5ee5368b` | 7 |

原始总量约 **3.89 GiB**，压缩后落盘；切为 **51 个分片**，每片 ≤ 90 MiB。

`.lzo` 是 `busybox lzop -1` 压缩流，不是 zip。解压：`busybox lzop -d < image.lzo > image.img`。

## 校验与重组

`manifest.json` 记录了每个原始镜像的完整 SHA256、每个分片的 SHA256 与字节数。`SHA256SUMS` 是兼容 `sha256sum -c` 的清单。

```bash
# 1) 校验所有分片（不会写文件）
python3 merge.py --verify-only

# 2) 重组为原始镜像（输出到 MERGE_OUT 环境变量指定目录，默认当前目录）
python3 merge.py
# 或
MERGE_OUT=/your/path python3 merge.py

# 3) 用标准工具复核
sha256sum -c SHA256SUMS
```

boot 分区（`mmcblk0p7.img`）做过**三重字节级验证**：设备端 `dd | sha256sum`、lzo 解压后的 `sha256sum`、本地原始镜像，三者完全一致（`16ff9c30…`），解压长度精确为 16,769,024 字节。

## 生成这些分片

`scripts/split_images.py` 用 90 MiB 块切分原始镜像并生成 `manifest.json` / `SHA256SUMS`。使用前修改脚本顶部的 `SRC` / `DST` 路径。

## 变砖恢复思路

1. 用 `merge.py` 重组出原始 `.img`。
2. `.lzo` 的先在设备上 `busybox lzop -d` 还原成 `.img`。
3. 从恢复环境（fastboot / 厂商线刷工具 / 第三方 recovery）按分区刷回，例如：
   ```
   dd if=mmcblk0p7.img of=/dev/block/mmcblk0p7 bs=4M
   ```
4. 刷回后**先别重启**，逐分区校验 SHA256 无误再重启。

⚠️ 分区号因机型而异。刷错分区（尤其 bootloader / boot）会导致设备无法启动。务必先在自己的设备上用 `ls -l /dev/block/platform/*/by-name/` 确认分区到名字的映射。
