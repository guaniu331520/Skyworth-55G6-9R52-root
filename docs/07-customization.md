# 07 · 系统定制

前提：已按 [`05-escalation-and-persistence.md`](05-escalation-and-persistence.md) 拿到**持久 root**。

**通用纪律**：

1. 每一项改动前都**先备份原文件**；
2. `/system` 默认 `ro`，改之前 `mount -o rw,remount /system`，改完 `sync` 再恢复 `ro`；
3. `/data` 上的改动（`pm disable`、`settings put`）不需要 remount；
4. 每项改完都**真重启**复查，确认厂商没有在开机时把配置重写回去。

---

## 0. 先定位厂商配置文件

厂商配置通常位于：

```
/system/pcfg/<机型>/config/general_config.xml
```

格式是 XML 式的键值对：

```xml
<config name="APP_SELF_INSTALL" value="false"/>
<config name="UPGRADE_SERVER" value="http://<OTA_HOST>/..."/>
```

**先确认设备实际读取哪一份机型目录**，不要盲改：

```bash
adb shell 'getprop ro.product.model; getprop ro.product.device'
adb shell 'find /system/pcfg -name general_config.xml'
```

下文所有 `<机型>` 请替换为实际目录名。所有写配置的操作都先备份：

```bash
adb shell 'CFG=/system/pcfg/<机型>/config/general_config.xml; \
           cp -p "$CFG" "$CFG.sotv_bak"'
```

---

## 1. 去广告

三管齐下：清缓存视频、停用广告服务、屏蔽域名。

### 1.1 删除开机广告缓存

```bash
adb shell 'ls -l /data/local/bootvideo.ts 2>/dev/null && rm -f /data/local/bootvideo.ts'
```

有些固件缓存路径不同，扩大搜索：

```bash
adb shell 'find /data /system -iname "*bootvideo*" -o -iname "*boot_ad*" 2>/dev/null'
```

> 只删缓存视频，不要删系统应用本身，以免破坏 launcher 依赖。

### 1.2 `pm disable` 广告服务

先找出实际包名（按 `ad` / `ads` / `promo` 等关键字过滤）：

```bash
adb shell 'pm list packages | grep -iE "ad|promo|advert"'
```

停用与恢复：

```bash
adb shell 'pm disable <AD_SERVICE_PKG>'
# 恢复：adb shell 'pm enable <AD_SERVICE_PKG>'
```

`pm disable` 的状态存在 `/data/system/package-restrictions.xml`，重启后保留。

### 1.3 hosts 域名屏蔽

在 `/system/etc/hosts` 末尾追加广告域名族：

```bash
adb shell <<'SH'
mount -o rw,remount /system
cp -p /system/etc/hosts /system/etc/hosts.sotv_bak
for d in <AD_DOMAIN_1> <AD_DOMAIN_2> <AD_DOMAIN_3>; do
  grep -q "[[:space:]]$d\$" /system/etc/hosts || \
    echo "0.0.0.0 $d" >> /system/etc/hosts
done
sync
mount -o ro,remount /system
SH
```

> ⚠️ **注意**：部分厂商固件（尤其带 Play 服务或 OTA 的）会在开机或升级时**重写 `/system/etc/hosts`**。所以每次升级或重启后都要复查：
>
> ```bash
> adb shell 'tail -20 /system/etc/hosts | grep 0.0.0.0'
> ```
>
> 若被重置，重新追加即可；更彻底的做法是把屏蔽规则写进厂商配置里的 DNS 选项（如果存在）。

---

## 2. 关闭自动升级（保留手动入口）

目标：让自动检查升级永远连不上服务器，但**不禁用升级程序本身**，从而保留手动升级入口。

### 2.1 升级服务器指向本地

```bash
adb shell <<'SH'
CFG=/system/pcfg/<机型>/config/general_config.xml
mount -o rw,remount /system
cp -p "$CFG" "$CFG.sotv_bak"

# 把升级服务器域名/地址改指向本地回路
# 具体 config name 以设备上实际读到的为准，下面只是示例
sed -i 's|\(name="UPGRADE_SERVER" value="\)[^"]*|\1http://127.0.0.1/|' "$CFG"

sync
mount -o ro,remount /system
SH
```

`127.0.0.1` 是本机回路地址，电视不会真的连到外部升级服务器；也可以指向局域网内一个不存在的地址 `<LOCAL_HOST>`。

### 2.2 域名屏蔽

按第 1.3 节的方式把升级域名 `<OTA_DOMAIN_1>`、`<OTA_DOMAIN_2>` 追加进 `/system/etc/hosts`：

```bash
echo '0.0.0.0 <OTA_DOMAIN_1>' >> /system/etc/hosts
echo '0.0.0.0 <OTA_DOMAIN_2>' >> /system/etc/hosts
```

### 2.3 复查

```bash
adb shell 'grep -i "UPGRADE\|OTA" /system/pcfg/<机型>/config/general_config.xml'
adb shell 'pm list packages | grep -i upgrade'    # 升级服务仍在，只是连不上服务器
```

> 不要在重启后立刻下结论：有些固件延迟几十秒才触发检查。观察一段时间确认没有自动下载/安装。

---

## 3. 开放 APK 自由安装

两个开关缺一不可：

- `APP_SELF_INSTALL`：允许侧载 / 自我安装；
- `package_verifier_enable=0`：**`INSTALL_FAILED_VERIFICATION_FAILURE` 的根源**——系统安装前的包校验服务会拒绝未签名/未知来源的 APK。

### 3.1 修改厂商配置

```bash
adb shell <<'SH'
CFG=/system/pcfg/<机型>/config/general_config.xml
mount -o rw,remount /system
cp -p "$CFG" "$CFG.sotv_bak"

# 允许自行安装（若该键已存在则改值；不存在则追加）
if grep -q 'name="APP_SELF_INSTALL"' "$CFG"; then
  sed -i 's|\(name="APP_SELF_INSTALL" value="\)[^"]*|\1true|' "$CFG"
else
  sed -i 's|</config>|  <config name="APP_SELF_INSTALL" value="true"/>\n</config>|' "$CFG"
fi

sync
mount -o ro,remount /system
SH
```

### 3.2 关闭包校验器

```bash
adb shell 'settings put global package_verifier_enable 0'
adb shell 'settings get global package_verifier_enable'    # 0
```

### 3.3 验证

```bash
adb install -r <TEST_APK>       # 应能成功安装，不再报 VERIFICATION_FAILURE
```

---

## 4. 开机自动进入指定 App

### 4.1 设计原则

- **按包名判断前台**：`dumpsys window | grep mCurrentFocus`，只看输出里是否包含 `<TARGET_PKG>`；
- **不强指定 Activity**：不要用 `am start -n <PKG>/.SomeActivity` 写死组件。厂商/儿童端可能有自己的锁屏与管控 Activity，强指定会破坏它；
- **限时退出**：等 `sys.boot_completed=1` 后再持续断言一段时间（例如 90 秒），随后退出，不把用户永久锁在某个 App 里。

### 4.2 `sotv_bringup.sh`

保存为 `sotv_bringup.sh`（`<TARGET_PKG>` 换成实际包名）：

```sh
#!/system/bin/sh
# 开机后把目标 App 带到前台，按包名判断，不写死 Activity。
# 由 /system/etc/init.tvserver.sh 以 root 后台拉起。

PKG=<TARGET_PKG>

# 1) 最多等 120 秒，直到系统启动完成
i=0
while [ $i -lt 120 ]; do
    [ "$(getprop sys.boot_completed)" = "1" ] && break
    sleep 1
    i=$((i + 1))
done

# 2) 让 launcher / 其它 BOOT_COMPLETED 接收者先稳定下来
sleep 10

# 3) 30 轮 × 3 秒 = 90 秒内，前台不是目标包名就拉起
j=0
while [ $j -lt 30 ]; do
    f=$(dumpsys window | grep mCurrentFocus)
    case "$f" in
        *"$PKG"*) ;;
        *) monkey -p "$PKG" 1 >/dev/null 2>&1 ;;
    esac
    sleep 3
    j=$((j + 1))
done

exit 0
```

`monkey -p <PKG> 1` 只按包名启动该 App 的默认入口，**不指定具体 Activity**，因此不会覆盖厂商/儿童端的锁屏管控。若设备上没有 `monkey`，也可退而求其次用 `am start`（让系统自己解析默认 Activity）：

```sh
am start -a android.intent.action.MAIN \
         -c android.intent.category.LAUNCHER \
         -p "$PKG" >/dev/null 2>&1
```

### 4.3 安装与拉起

```bash
adb push sotv_bringup.sh /data/local/tmp/
adb shell <<'SH'
mount -o rw,remount /system
cp /data/local/tmp/sotv_bringup.sh /system/xbin/sotv_bringup.sh
chmod 0755 /system/xbin/sotv_bringup.sh

# 与 root 守护一起，在厂商脚本末尾追加（grep -q 防重复）
if ! grep -q 'sotv_bringup.sh' /system/etc/init.tvserver.sh; then
  echo '/system/xbin/sotv_bringup.sh >/dev/null 2>&1 &' \
    >> /system/etc/init.tvserver.sh
fi

sync
mount -o ro,remount /system
SH
```

### 4.4 验证

```bash
adb reboot
# 等待开机完成后观察前台
adb shell 'dumpsys window | grep mCurrentFocus'
# 应包含 <TARGET_PKG>

adb shell 'ps | grep sotv_bringup | grep -v grep'
# 90 秒后进程应自行退出（设计如此，不永久锁定）
```

---

## 5. 改动汇总与回退

| 项目 | 改动位置 | 回退方法 |
|---|---|---|
| 去广告缓存 | `/data/local/bootvideo.ts` | 无法恢复（本来就是缓存） |
| 广告服务 | `pm disable <AD_SERVICE_PKG>` | `pm enable <AD_SERVICE_PKG>` |
| hosts 屏蔽 | `/system/etc/hosts` | `cp hosts.sotv_bak hosts` |
| 自动升级 | `general_config.xml` 的 `UPGRADE_SERVER` | `cp ...general_config.xml.sotv_bak ...` |
| APK 安装 | `APP_SELF_INSTALL` + `settings` | 恢复配置备份 + `settings put global package_verifier_enable 1` |
| 开机自启 | `/system/xbin/sotv_bringup.sh` + 开机脚本 | 删脚本 + 从 `init.tvserver.sh.sotv_bak` 恢复 |

一键回退（按需挑选）：

```bash
adb shell <<'SH'
mount -o rw,remount /system
[ -f /system/etc/hosts.sotv_bak ] && \
  cp /system/etc/hosts.sotv_bak /system/etc/hosts
CFG=/system/pcfg/<机型>/config/general_config.xml
[ -f "$CFG.sotv_bak" ] && cp "$CFG.sotv_bak" "$CFG"
rm -f /system/xbin/sotv_bringup.sh
cp /system/etc/init.tvserver.sh.sotv_bak /system/etc/init.tvserver.sh
sync
mount -o ro,remount /system
SH
adb shell 'settings put global package_verifier_enable 1'
adb reboot
```

> 如需连 root 一起移除，再执行 [`05-escalation-and-persistence.md`](05-escalation-and-persistence.md) 第 7 节的回退步骤；如需完整救砖，用 [`06-backup.md`](06-backup.md) 的镜像按分区刷回。
