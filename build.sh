#!/bin/bash
set -euo pipefail
umask 022
export LC_ALL=C PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
[ "$(id -u)" = 0 ] || { echo "Run as root: sudo bash build.sh"; exit 1; }
command -v apt-get >/dev/null 2>&1 || { echo "This script needs a Debian/Ubuntu host (apt-get not found)."; exit 1; }
OUT="${OUT:-$PWD/out}"; SIZE="${SIZE:-3G}"; SUITE="${SUITE:-bookworm}"; TARGET="${TARGET:-pc}"; DEBUG_SHELL="${DEBUG_SHELL:-0}"
[ "$TARGET" = pc ] || [ "$TARGET" = pi ] || { echo "TARGET must be pc or pi"; exit 1; }
[ "$DEBUG_SHELL" = 0 ] || [ "$DEBUG_SHELL" = 1 ] || { echo "DEBUG_SHELL must be 0 or 1"; exit 1; }
# Full-disk encryption (LUKS2, argon2id): the root file system, the Aegis vault and its key are encrypted at rest.
# ENCRYPT=1 (default) asks for a passphrase now and at every boot; give it non-interactively with LUKS_PASSPHRASE=...; ENCRYPT=0 turns it off.
ENCRYPT="${ENCRYPT:-1}"
[ "$ENCRYPT" = 0 ] || [ "$ENCRYPT" = 1 ] || { echo "ENCRYPT must be 0 or 1"; exit 1; }
if [ "$ENCRYPT" = 1 ]; then
  if [ -z "${LUKS_PASSPHRASE:-}" ]; then
    read -r -s -p "Disk passphrase (asked at every boot, min 8 characters): " P1; echo
    read -r -s -p "Repeat it: " P2; echo
    [ "$P1" = "$P2" ] || { echo "Passphrases differ."; exit 1; }
    LUKS_PASSPHRASE="$P1"; unset P1 P2
  fi
  [ "${#LUKS_PASSPHRASE}" -ge 8 ] || { echo "The passphrase must be at least 8 characters."; exit 1; }
fi
[[ "$SIZE" =~ ^[1-9][0-9]*[MG]$ ]] || { echo "SIZE must look like 3G or 3072M"; exit 1; }
[[ "$SUITE" =~ ^[a-z]+$ ]] || { echo "SUITE must be a Debian suite name, e.g. bookworm"; exit 1; }
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
for f in os/RealOneOS.c kernel/kernel.conf Aegis/lockdown.sh Aegis/sysctl.conf Aegis/modprobe-blacklist.conf Aegis/nftables.conf Aegis/suid-strip.list Aegis/limits.conf Aegis/aegis kernel/cmdline.pc kernel/cmdline.pi kernel/pi-config.txt assets/wallpapers/Aurora.png assets/sounds/boot.mp3 rak/include/rak.h rak/tools/pack.sh apps; do [ -e "$SRC/$f" ] || { echo "missing $SRC/$f (run build.sh from a full checkout of Root/)"; exit 1; }; done
. "$SRC/kernel/kernel.conf"
mkdir -p "$OUT"; WORK="$(mktemp -d)"; MNT="$WORK/mnt"; mkdir -p "$MNT" "$WORK/src"
IMG="$OUT/realoneos.img"; [ "$TARGET" = pi ] && IMG="$OUT/realoneos-pi.img"; LOOP=""

echo "== Host tools =="
export DEBIAN_FRONTEND=noninteractive
APT="-o Acquire::Retries=5"
apt-get $APT update
apt-get $APT install -y debootstrap debian-archive-keyring parted e2fsprogs grub-pc-bin grub2-common qemu-utils xz-utils dosfstools cryptsetup
[ "$TARGET" != pi ] || apt-get $APT install -y qemu-user-static binfmt-support

cleanup() {
  set +e
  for d in dev/pts dev proc sys boot/firmware boot; do umount -l "$MNT/$d" 2>/dev/null; done
  umount -l "$MNT" 2>/dev/null
  [ -e /dev/mapper/realone_root ] && cryptsetup close realone_root 2>/dev/null
  [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
  [ -n "${WORK:-}" ] && [ -d "$WORK" ] && ! grep -qF " $WORK" /proc/mounts && rm -rf --one-file-system "$WORK" 2>/dev/null
}
trap cleanup EXIT

echo "== Creating disk image ($SIZE) =="
rm -f "$IMG"; truncate -s "$SIZE" "$IMG"
LOOP="$(losetup --find --show --partscan "$IMG")"
apt-get install -y --no-install-recommends libout123-modules || true
if [ "$TARGET" = pi ]; then
  parted -s "$LOOP" mklabel msdos mkpart primary fat32 4MiB 260MiB mkpart primary ext4 260MiB 100%
  RAWROOT="${LOOP}p2"
else
  parted -s "$LOOP" mklabel msdos mkpart primary ext4 1MiB 513MiB set 1 boot on mkpart primary ext4 513MiB 100%
  BOOTP="${LOOP}p1"; RAWROOT="${LOOP}p2"        # /boot stays unencrypted (kernel + initramfs); everything else is encrypted
fi
partprobe "$LOOP" 2>/dev/null || true
for i in $(seq 1 30); do [ -b "$RAWROOT" ] && break; sleep 0.5; done
[ -b "$RAWROOT" ] || { echo "Partition device $RAWROOT did not appear."; exit 1; }
if [ "$ENCRYPT" = 1 ]; then
  echo "== Encrypting the root partition (LUKS2) =="
  printf %s "$LUKS_PASSPHRASE" | cryptsetup luksFormat --type luks2 --cipher aes-xts-plain64 --key-size 512 --hash sha256 --pbkdf argon2id -q -d - "$RAWROOT"
  printf %s "$LUKS_PASSPHRASE" | cryptsetup open -d - "$RAWROOT" realone_root
  unset LUKS_PASSPHRASE
  LUKSUUID="$(blkid -s UUID -o value "$RAWROOT")"
  ROOTP=/dev/mapper/realone_root; ROOTSPEC=/dev/mapper/realone_root
else
  ROOTP="$RAWROOT"
fi
mkfs.ext4 -F -L realoneos "$ROOTP"
mount "$ROOTP" "$MNT"
UUID="$(blkid -s UUID -o value "$ROOTP")"
[ "$ENCRYPT" = 1 ] || ROOTSPEC="UUID=$UUID"
apt-get install -y --no-install-recommends libout123-modules || true
if [ "$TARGET" = pi ]; then
  mkfs.vfat -F 32 -n RPIFW "${LOOP}p1"
  BUUID="$(blkid -s UUID -o value "${LOOP}p1")"
else
  mkfs.ext4 -F -L realoneboot "$BOOTP"
  mkdir -p "$MNT/boot"; mount "$BOOTP" "$MNT/boot"
  BUUID="$(blkid -s UUID -o value "$BOOTP")"
fi

echo "== Installing base system (debootstrap $SUITE) =="
apt-get install -y --no-install-recommends libout123-modules || true
if [ "$TARGET" = pi ]; then
  update-binfmts --enable qemu-aarch64 2>/dev/null || true
  debootstrap --arch=arm64 --foreign --variant=minbase --components=main,non-free-firmware "$SUITE" "$MNT" http://deb.debian.org/debian
  cp /usr/bin/qemu-aarch64-static "$MNT/usr/bin/"
  chroot "$MNT" /debootstrap/debootstrap --second-stage
  mkdir -p "$MNT/boot/firmware"; mount "${LOOP}p1" "$MNT/boot/firmware"
else
  debootstrap --arch=amd64 --variant=minbase "$SUITE" "$MNT" http://deb.debian.org/debian
fi
mount --bind /dev "$MNT/dev"; mount --bind /dev/pts "$MNT/dev/pts"
mount -t proc proc "$MNT/proc"; mount -t sysfs sys "$MNT/sys"
cp /etc/resolv.conf "$MNT/etc/resolv.conf"

echo "$ROOTSPEC / ext4 defaults,noatime 0 1" > "$MNT/etc/fstab"
[ "$TARGET" != pc ] || echo "UUID=$BUUID /boot ext4 defaults,nosuid,nodev,noexec 0 2" >> "$MNT/etc/fstab"
[ "$ENCRYPT" != 1 ] || echo "realone_root UUID=$LUKSUUID none luks,discard" > "$MNT/etc/crypttab"
[ "$TARGET" != pi ] || echo "UUID=$BUUID /boot/firmware vfat defaults,nosuid,nodev,noexec 0 2" >> "$MNT/etc/fstab"
echo "tmpfs /tmp tmpfs defaults,nosuid,nodev,noexec,mode=1777,size=64M 0 0" >> "$MNT/etc/fstab"
echo "tmpfs /Root/Storage/Temps tmpfs defaults,nosuid,nodev,noexec,mode=0700,uid=1000,gid=1000,size=64M 0 0" >> "$MNT/etc/fstab"   # Storage/Temps is wiped at every boot
echo realoneos > "$MNT/etc/hostname"
echo "127.0.0.1 localhost realoneos" > "$MNT/etc/hosts"
mkdir -p "$MNT/Root/Storage/User/.realone/wallpapers" "$MNT/usr/local/share/realone/wallpapers" "$MNT/Root/Storage/Data/Apps" "$MNT/Root/Storage/Downloads" "$MNT/Root/Storage/Temps"
cp "$SRC/os/RealOneOS.c" "$MNT/root/RealOneOS.c"
mkdir -p "$MNT/root/src"; cp -r "$SRC/rak" "$SRC/apps" "$MNT/root/src/"
cp "$SRC"/assets/wallpapers/*.png "$MNT/usr/local/share/realone/wallpapers/"   # built-in wallpapers live in the OS, not in Storage
mkdir -p "$MNT/usr/local/share/realone" "$MNT/etc/realone"
cp "$SRC"/assets/sounds/*.mp3 "$MNT/usr/local/share/realone/"
cp -r "$SRC/Aegis" "$MNT/etc/realone/aegis-src"      # encrypted and sealed (root-only) by Aegis/lockdown.sh

echo "== Installing kernel, X and building RealOneOS =="
printf '#!/bin/sh\nexit 101\n' > "$MNT/usr/sbin/policy-rc.d"; chmod 755 "$MNT/usr/sbin/policy-rc.d"
chroot "$MNT" env TARGET="$TARGET" SUITE="$SUITE" KERNEL_PKG_PC="$KERNEL_PKG_PC" KERNEL_PKG_PI="$KERNEL_PKG_PI" KERNEL_FIRMWARE_PI="$KERNEL_FIRMWARE_PI" DEBUG_SHELL="$DEBUG_SHELL" /bin/bash -e <<'CH'
export DEBIAN_FRONTEND=noninteractive LANG=C
mkdir -p /etc/apt/apt.conf.d
echo 'Acquire::Retries "5"; APT::Install-Recommends "0";' > /etc/apt/apt.conf.d/80realone
[ -f /etc/apt/sources.list ] && { grep -q non-free-firmware /etc/apt/sources.list || sed -i 's/ main[[:space:]]*$/ main non-free-firmware/' /etc/apt/sources.list; }
[ "$TARGET" = pi ] && echo "deb http://deb.debian.org/debian $SUITE-backports main non-free-firmware" >> /etc/apt/sources.list
apt-get update
apt-get install -y --no-install-recommends cryptsetup cryptsetup-initramfs initramfs-tools systemd-sysv udev kmod procps \
  xserver-xorg xserver-xorg-video-fbdev xserver-xorg-video-vesa xinit fonts-dejavu-core \
  gcc libc6-dev pkg-config libcairo2-dev libx11-dev libjpeg-dev libasound2-dev libcairo2 libx11-6 libjpeg62-turbo libasound2 mpg123 alsa-utils
apt-get install -y --no-install-recommends libout123-modules || true
if [ "$TARGET" = pi ]; then
  apt-get install -y --no-install-recommends -t "$SUITE-backports" "$KERNEL_PKG_PI" "$KERNEL_FIRMWARE_PI"
else
  apt-get install -y --no-install-recommends "$KERNEL_PKG_PC"
fi
apt-get install -y --no-install-recommends wpasupplicant iw rfkill wireless-regdb ca-certificates systemd-resolved zip \
  firmware-iwlwifi firmware-realtek firmware-atheros firmware-brcm80211 firmware-misc-nonfree nftables openssl xauth
mkdir -p /etc/wpa_supplicant /etc/systemd/network /usr/local/sbin
cat > /etc/wpa_supplicant/realone.conf <<'WC'
ctrl_interface=DIR=/run/wpa_supplicant
update_config=1
WC
chmod 600 /etc/wpa_supplicant/realone.conf
cat > /usr/local/sbin/realone-wifid <<'WD'
#!/bin/sh
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
IF=""
for i in $(seq 1 40); do
  for d in /sys/class/net/*/wireless /sys/class/net/*/phy80211; do
    [ -e "$d" ] || continue
    IF="$(basename "$(dirname "$d")")"; break
  done
  [ -n "$IF" ] && break
  sleep 1
done
[ -n "$IF" ] || exit 0
exec wpa_supplicant -i "$IF" -c /etc/wpa_supplicant/realone.conf
WD
chmod 755 /usr/local/sbin/realone-wifid
cat > /etc/systemd/system/realone-wifi.service <<'WS'
[Unit]
Description=RealOne Wi-Fi (wpa_supplicant)
After=systemd-udevd.service
[Service]
ExecStart=/usr/local/sbin/realone-wifid
Restart=on-failure
RestartSec=3
NoNewPrivileges=yes
PrivateTmp=yes
ProtectHome=yes
ProtectKernelModules=yes
ProtectControlGroups=yes
LockPersonality=yes
RestrictRealtime=yes
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6 AF_NETLINK AF_PACKET
[Install]
WantedBy=multi-user.target
WS
cat > /etc/systemd/network/20-realone-wifi.network <<'WN'
[Match]
Type=wlan
[Network]
DHCP=yes
IPv6AcceptRA=yes
WN
systemctl enable realone-wifi.service systemd-networkd.service systemd-resolved.service || echo "WARNING: could not enable the Wi-Fi services"
systemctl mask systemd-networkd-wait-online.service || true
gcc -O2 -w -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE -pie -Wl,-z,relro,-z,now -I/root/src/rak/include -o /usr/local/bin/RealOneOS /root/RealOneOS.c $(pkg-config --cflags --libs cairo x11) -ljpeg -lasound -ldl -lm -lpthread
chmod 755 /usr/local/bin/RealOneOS
test -x /usr/local/bin/RealOneOS
# ---- build the RAK apps (Settings.RAK, Calculator.RAK) ----
mkdir -p /usr/local/share/realone/apps
CAIRO_CFLAGS="$(pkg-config --cflags cairo)" CAIRO_LIBS="$(pkg-config --libs cairo)" bash /root/src/rak/tools/pack.sh /usr/local/share/realone/apps
ls /usr/local/share/realone/apps/Settings.RAK /usr/local/share/realone/apps/Calculator.RAK
ls /usr/local/share/realone/wallpapers/Aurora.png
cat > /usr/local/bin/RealOne <<'LAUNCH'
if [ -n "$DISPLAY" ]; then exec RealOneOS "$@"; fi
VT="${XDG_VTNR:-$(tty 2>/dev/null | sed -n 's|.*/tty\([0-9][0-9]*\)$|\1|p')}"
exec xinit /usr/local/bin/RealOneOS "$@" -- :0 "vt${VT:-1}" -nolisten tcp
LAUNCH
chmod 755 /usr/local/bin/RealOne
# ---- the Storage jail: RealOneOS runs as the unprivileged user "realuser", confined to /Root/Storage ----
# Layout: /Root/Storage/User (home) | Data/Apps | Downloads | Temps (tmpfs)
groupadd -f netdev
useradd -u 1000 -U -M -c "RealUser" -d /Root/Storage/User -s /usr/sbin/nologin -G video,audio,netdev realuser   # login name realuser, display name RealUser
passwd -l realuser
chown -R realuser:realuser /Root/Storage
chown root:root /Root; chmod 755 /Root
chmod 700 /Root/Storage /Root/Storage/User /Root/Storage/Data /Root/Storage/Data/Apps /Root/Storage/Downloads /Root/Storage/Temps
# (brightness, LEDs and the Wi-Fi switch are done by the root helper realone-hw below, so the user needs no write access to /sys or /dev)
sed -i 's#^ctrl_interface=.*#ctrl_interface=DIR=/run/wpa_supplicant GROUP=netdev#' /etc/wpa_supplicant/realone.conf
# X server stays a root service; only the display connection (cookie) is shared with the jailed user
cat > /usr/local/sbin/realone-xstart <<'XS'
#!/bin/sh
A=/run/realone-x/Xauthority
rm -f "$A"; touch "$A"
xauth -f "$A" add :0 . "$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')"
chgrp realuser "$A"; chmod 640 "$A"
exec Xorg :0 vt1 -nolisten tcp -novtswitch -auth "$A"
XS
chmod 755 /usr/local/sbin/realone-xstart
cat > /etc/systemd/system/realone-x.service <<'XU'
[Unit]
Description=RealOne display server
After=systemd-udevd.service
[Service]
RuntimeDirectory=realone-x
RuntimeDirectoryMode=0750
ExecStart=/usr/local/sbin/realone-xstart
Restart=always
NoNewPrivileges=yes
ProtectHome=yes
ProtectControlGroups=yes
LockPersonality=yes
RestrictAddressFamilies=AF_UNIX AF_NETLINK
[Install]
WantedBy=multi-user.target
XU
# power requests from the jailed user: only the exact words "poweroff" or "reboot" are accepted
cat > /usr/local/sbin/realone-power <<'PW'
#!/bin/sh
R=/run/realone-power/request
[ -f "$R" ] && [ ! -L "$R" ] || { rm -f "$R"; exit 0; }
W="$(timeout 2 head -c 16 "$R" 2>/dev/null | tr -d '\n')"; rm -f "$R"
case "$W" in poweroff) exec systemctl poweroff;; reboot) exec systemctl reboot;; esac
PW
chmod 755 /usr/local/sbin/realone-power
cat > /etc/systemd/system/realone-power.path <<'PP'
[Path]
PathChanged=/run/realone-power/request
PathExists=/run/realone-power/request
[Install]
WantedBy=multi-user.target
PP
cat > /etc/systemd/system/realone-power.service <<'PS'
[Service]
Type=oneshot
ExecStart=/usr/local/sbin/realone-power
PS
echo 'd /run/realone-power 0700 realuser realuser -' > /etc/tmpfiles.d/realone-power.conf
# hardware requests from the jailed user (brightness / LED / Wi-Fi switch): only these exact words are accepted, nothing else is ever written
cat > /usr/local/sbin/realone-hw <<'HW'
#!/bin/sh
D=/run/realone-hw
for R in "$D"/req.*; do
  [ -f "$R" ] && [ ! -L "$R" ] || { rm -f "$R"; continue; }
  L="$(timeout 2 head -c 40 "$R" 2>/dev/null | head -n 1)"; rm -f "$R"
  set -- $L
  case "$1:$#" in
  brightness:2)
    case "$2" in ''|*[!0-9]*) continue;; esac; [ "$2" -le 255 ] || continue
    for B in /sys/class/backlight/*; do
      [ -r "$B/max_brightness" ] || continue
      M=$(cat "$B/max_brightness"); V=$(( $2 * M / 255 )); [ "$2" -gt 0 ] && [ "$V" -lt 1 ] && V=1
      echo "$V" > "$B/brightness" 2>/dev/null
    done;;
  led:2)
    case "$2" in 0|1) ;; *) continue;; esac
    for B in /sys/class/leds/ACT /sys/class/leds/led0 /sys/class/leds/led1; do
      [ -w "$B/brightness" ] || continue
      echo none > "$B/trigger" 2>/dev/null
      M=$(cat "$B/max_brightness" 2>/dev/null || echo 1); [ "$2" = 1 ] && echo "$M" > "$B/brightness" || echo 0 > "$B/brightness"
      break
    done;;
  rfkill:3)
    case "$2$3" in *[!0-9]*) continue;; esac; [ "$2" -le 15 ] && { [ "$3" = 0 ] || [ "$3" = 1 ]; } || continue
    printf "\\000\\000\\000\\000\\$(printf %03o "$2")\\003\\$(printf %03o "$3")\\000" > /dev/rfkill 2>/dev/null;;
  esac
done
HW
chmod 755 /usr/local/sbin/realone-hw
cat > /etc/systemd/system/realone-hw.path <<'HP'
[Path]
DirectoryNotEmpty=/run/realone-hw
[Install]
WantedBy=multi-user.target
HP
cat > /etc/systemd/system/realone-hw.service <<'HS'
[Service]
Type=oneshot
ExecStart=/usr/local/sbin/realone-hw
NoNewPrivileges=yes
ProtectHome=yes
PrivateNetwork=yes
RestrictAddressFamilies=AF_UNIX
HS
echo 'd /run/realone-hw 0700 realuser realuser -' >> /etc/tmpfiles.d/realone-power.conf
# after the OS exits: quick exit (<20 s, crash loop) -> power off, otherwise reboot
cat > /usr/local/sbin/realone-after <<'AF'
#!/bin/sh
systemctl list-jobs 2>/dev/null | grep -qE 'poweroff|reboot|halt' && exit 0
T0="$(cat /run/realone-start 2>/dev/null || echo 0)"
if [ $(( $(date +%s) - T0 )) -lt 20 ]; then sleep 15; systemctl poweroff; else systemctl reboot; fi
AF
chmod 755 /usr/local/sbin/realone-after
# integrity audit: before every start the vault is checked and every OS file is compared with its recorded SHA-256; on a mismatch the session does not start
cat > /etc/systemd/system/realone-audit.service <<'AU'
[Unit]
Description=Aegis integrity audit
DefaultDependencies=no
After=local-fs.target
[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/sbin/aegis audit
[Install]
WantedBy=multi-user.target
AU
systemctl enable realone-audit.service || true
# the session itself: no login, no shell, no password; the whole file system except Storage is read-only to it
cat > /etc/systemd/system/realone-session.service <<'SU'
[Unit]
Description=RealOneOS (jailed to /Root/Storage)
After=realone-x.service systemd-udevd.service realone-wifi.service realone-audit.service
Requires=realone-x.service realone-audit.service
[Service]
User=realuser
Group=realuser
Environment=HOME=/Root/Storage/User REALONE_STORAGE=/Root/Storage TMPDIR=/Root/Storage/Temps DISPLAY=:0 XAUTHORITY=/run/realone-x/Xauthority LANG=C.UTF-8
WorkingDirectory=/Root/Storage/User
ExecStartPre=+/bin/sh -c 'date +%%s > /run/realone-start'
ExecStartPre=/bin/sh -c 'for i in $$(seq 1 100); do [ -S /tmp/.X11-unix/X0 ] && exit 0; sleep 0.2; done; exit 1'
ExecStart=/usr/local/bin/RealOneOS
ExecStopPost=+/usr/local/sbin/realone-after
Restart=no
# --- the jail ---
NoNewPrivileges=yes
CapabilityBoundingSet=
AmbientCapabilities=
ProtectSystem=strict
ReadWritePaths=/Root/Storage /run/realone-power /run/realone-hw
NoExecPaths=/Root/Storage
InaccessiblePaths=-/root -/etc/realone -/etc/wpa_supplicant -/boot -/home -/srv -/mnt -/media -/var/log
ProtectHome=yes
ProtectProc=invisible
ProtectKernelModules=yes
ProtectKernelLogs=yes
ProtectControlGroups=yes
ProtectClock=yes
ProtectHostname=yes
LockPersonality=yes
RestrictRealtime=yes
RestrictSUIDSGID=yes
RestrictNamespaces=yes
SystemCallArchitectures=native
UMask=0077
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6 AF_NETLINK
SystemCallFilter=@system-service memfd_create
SystemCallFilter=~@mount @privileged @debug @module @reboot @swap @raw-io @cpu-emulation @obsolete @clock
SystemCallErrorNumber=EPERM
DevicePolicy=closed
DeviceAllow=char-alsa rw
ProtectKernelTunables=yes
MemoryDenyWriteExecute=yes
LimitMEMLOCK=0
PrivateMounts=yes
TasksMax=512
LimitCORE=0
LimitNOFILE=4096
StandardInput=null
StandardOutput=null
StandardError=null
[Install]
WantedBy=multi-user.target
SU
systemctl enable realone-x.service realone-session.service realone-power.path realone-hw.path || echo "WARNING: could not enable the RealOne session"
# no login prompt anywhere: tty1 belongs to the display server
systemctl mask getty@tty1.service getty.target >/dev/null 2>&1 || true
if [ "$DEBUG_SHELL" = 1 ]; then
mkdir -p /etc/systemd/system/getty@tty2.service.d
printf '[Service]\nExecStart=\nExecStart=-/sbin/agetty --autologin root --noclear %%I $TERM\n' > /etc/systemd/system/getty@tty2.service.d/debug.conf
systemctl unmask getty.target >/dev/null 2>&1 || true
systemctl enable getty@tty2.service || true
echo "WARNING: DEBUG_SHELL=1 - root shell on tty2 (do not ship this image)"
fi
passwd -l root
mkdir -p /etc/X11/xorg.conf.d /etc/systemd/journald.conf.d
cat > /etc/X11/xorg.conf.d/10-realone.conf <<'XC'
Section "ServerFlags"
  Option "BlankTime" "0"
  Option "StandbyTime" "0"
  Option "SuspendTime" "0"
  Option "OffTime" "0"
EndSection
Section "OutputClass"
  Identifier "realone-lite"
  Option "AccelMethod" "none"
  Option "ShadowFB" "true"
EndSection
XC
cat > /etc/systemd/journald.conf.d/realone.conf <<'JC'
[Journal]
Storage=volatile
RuntimeMaxUse=8M
ForwardToSyslog=no
JC
bash /etc/realone/aegis-src/lockdown.sh
mkdir -p /etc/systemd/logind.conf.d
printf '[Login]\nNAutoVTs=0\n' > /etc/systemd/logind.conf.d/realone.conf
systemctl mask autovt@.service ctrl-alt-del.target >/dev/null 2>&1 || true
chmod 700 /root
rm -f /root/RealOneOS.c
rm -rf /root/src
apt-get purge -y zip gcc libc6-dev pkg-config libcairo2-dev libx11-dev libjpeg-dev libasound2-dev
apt-get autoremove --purge -y
rm -f /usr/sbin/policy-rc.d
systemctl mask apt-daily.timer apt-daily-upgrade.timer apt-daily.service apt-daily-upgrade.service dpkg-db-backup.timer e2scrub_all.timer fstrim.timer >/dev/null 2>&1 || true
apt-get clean
rm -rf /var/lib/apt/lists/*
ln -sf ../run/systemd/resolve/stub-resolv.conf /etc/resolv.conf
CH

apt-get install -y --no-install-recommends libout123-modules || true
if [ "$TARGET" = pi ]; then
echo "== Raspberry Pi boot files =="
ls "$MNT"/boot/firmware/vmlinuz-* >/dev/null 2>&1 || { echo "kernel was not copied to /boot/firmware"; exit 1; }
[ -f "$MNT/boot/firmware/config.txt" ] || { echo "config.txt missing in /boot/firmware"; exit 1; }
echo "console=tty1 root=$ROOTSPEC $(tr '\n' ' ' < "$SRC/kernel/cmdline.pi")" > "$MNT/boot/firmware/cmdline.txt"
cat "$SRC/kernel/pi-config.txt" >> "$MNT/boot/firmware/config.txt"
ls "$MNT/boot/firmware"

echo "== Finishing =="
rm -f "$MNT/usr/bin/qemu-aarch64-static"
sync; cleanup; LOOP=""; trap - EXIT
xz -T0 -f "$IMG"
else
echo "== Bootloader =="
KERN="$(basename "$(ls "$MNT"/boot/vmlinuz-* | sort -V | tail -1)")"
INITRD="initrd.img-${KERN#vmlinuz-}"
[ -f "$MNT/boot/$INITRD" ] || { echo "initrd $INITRD missing"; exit 1; }
mkdir -p "$MNT/boot/grub"
cat > "$MNT/boot/grub/grub.cfg" <<GR
set timeout=0
set default=0
insmod all_video
set gfxpayload=1024x768x32
menuentry "RealOneOS" {
  search --no-floppy --fs-uuid --set=root $BUUID
  linux /$KERN root=$ROOTSPEC $(tr '\n' ' ' < "$SRC/kernel/cmdline.pc")
  initrd /$INITRD
}
GR
grub-install --target=i386-pc --recheck --boot-directory="$MNT/boot" --modules="part_msdos ext2" "$LOOP"

echo "== Finishing =="
sync; cleanup; LOOP=""; trap - EXIT
qemu-img convert -c -O qcow2 "$IMG" "$OUT/realoneos.qcow2"
xz -T0 -f "$IMG"
fi

rm -rf --one-file-system "$WORK"
( cd "$OUT" && sha256sum realoneos* > SHA256SUMS )
ls -lh "$OUT"
[ "$ENCRYPT" != 1 ] || echo "== The disk is encrypted: the passphrase is asked at every boot and cannot be recovered if lost =="
if [ "$TARGET" = pi ]; then echo "== Done. Flash out/realoneos-pi.img.xz to an SD card (Raspberry Pi Imager > Use custom, or balenaEtcher) =="
else echo "== Done. Boot out/realoneos.qcow2 in QEMU / Limbo / VirtualBox (convert) =="; fi
