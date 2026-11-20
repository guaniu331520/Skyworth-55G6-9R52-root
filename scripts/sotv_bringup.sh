#!/system/bin/sh
# SOTV boot bring-up: after a cold boot, make sure the quiz UI is in front.
#
# Why: StudyOnTV's BootReceiver only starts its BACKGROUND service; the Coocaa
# launcher (com.tianci.movieplatform) and 3rd-party apps that also listen to
# BOOT_COMPLETED steal the foreground, so the TV lands on the home page instead
# of the quiz screen that was showing before power-off.
#
# Strategy: wait for boot_completed, let the launcher settle, then for 90s
# re-launch MainActivity whenever something else is on top. It then EXITS so
# the user is not permanently locked into the quiz app.
#
# Launched from /system/etc/init.tvserver.sh (runs as root, oneshot, background).

PKG=com.studyontv.tv
ACT=com.studyontv.tv/.MainActivity

# 1) wait up to 120s for the system to finish booting
i=0
while [ $i -lt 120 ]; do
    b=$(getprop sys.boot_completed)
    [ "$b" = "1" ] && break
    sleep 1
    i=$((i+1))
done

# 2) let the launcher / ActivityManager / other BOOT_COMPLETED apps settle
sleep 10

# 3) 30 rounds x 3s = 90s of foreground assertion
j=0
while [ $j -lt 30 ]; do
    f=$(dumpsys window | grep mCurrentFocus)
    case "$f" in
        *$PKG*) ;;
        *) am start -n "$ACT" >/dev/null 2>&1 ;;
    esac
    sleep 3
    j=$((j+1))
done

exit 0
