# 配置样例（脱敏）

本目录**故意不存放真实配置文件**。原因：

- 厂商 `general_config.xml` 里含具体机型目录名、升级服务器域名、广告下发域名。这些虽非个人隐私，但与特定设备/地区绑定，直接照抄到别的机器可能不适用，也可能触发厂商风控。
- `/system/etc/hosts` 的域名屏蔽清单同理，不同固件版本的广告域名不同。

因此这里只给出**改动模式**，你需要在自己的设备上按 `docs/07-customization.md` 实地确认后再改。

## pcfg 配置项改动模式

文件通常位于：`/system/pcfg/<机型目录>/config/general_config.xml`

格式（`value` 用双引号包裹）：

```xml
<config name="BOOT_AD" value="true"/>
<config name="APP_SELF_INSTALL" value="false"/>
<config name="CURRENT_UPGRADE_SERVER" value="some.host/path"/>
<config name="CURRENT_AD_SERVER" value="some.host"/>
```

本项目做过的改动（值改成）：

| 配置项 | 原值 | 改为 | 目的 |
|---|---|---|---|
| `BOOT_AD` | `true` | `false` | 关开机广告拉取 |
| `APP_SELF_INSTALL` | `false` | `true` | 开放自由安装 APK |
| `CURRENT_UPGRADE_SERVER` | `<某域名>` | `127.0.0.1` | 断掉自动升级 |
| `CURRENT_AD_SERVER` | `<某域名>` | `127.0.0.1` | 断掉广告下发 |

> 改前务必 pull 一份原件备份，并记录 SHA256。厂商升级/恢复出厂可能覆盖这些文件。

## hosts 屏蔽模式

`/system/etc/hosts` 追加格式：

```
127.0.0.1       <广告/升级域名>
```

注意：部分厂商固件在启动时会重写 hosts。改完后要**重启再复查**文件是否仍在；若被重置，需把屏蔽逻辑写进开机脚本（见 `scripts/sotv_bringup.sh` 的思路）。

## init 开机脚本注入模式

在厂商开机脚本（本项目是 `/system/etc/init.tvserver.sh`）**末尾追加**：

```sh
# <your label>
/system/xbin/sotv_root --daemon >/dev/null 2>&1 &
sh /system/xbin/sotv_bringup.sh >/dev/null 2>&1 &
```

改前同样备份原件（设备上留 `.bak`），并用 `sh -n` 检查语法。
