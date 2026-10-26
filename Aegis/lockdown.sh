#!/bin/bash
# Aegis: runs once inside the image during build (root only).
# 1) installs the hardening files, 2) packs the source files into an AES-256 encrypted vault
# (/etc/realone/aegis/vault.enc, HMAC-SHA256 protected), 3) deletes the plaintext and seals everything root-only.
[ "$(id -u)" = 0 ] || { echo "Aegis: ACCESS DENIED - root access is required." >&2; exit 13; }
set -e
S=/etc/realone/aegis-src
V=/etc/realone/aegis
install -D -m 600 "$S/sysctl.conf"             /etc/sysctl.d/99-realone-hardening.conf
install -D -m 600 "$S/modprobe-blacklist.conf" /etc/modprobe.d/realone-blacklist.conf
install -D -m 600 "$S/nftables.conf"           /etc/nftables.conf
install -D -m 600 "$S/limits.conf"             /etc/security/limits.d/99-realone.conf
while read -r f; do
  case "$f" in ''|\#*) continue;; esac
  [ -e "$f" ] && chmod u-s,g-s "$f" || true
done < "$S/suid-strip.list"
systemctl enable nftables.service || echo "WARNING: could not enable the firewall"

# --- OS files: root-owned, never writable by the user, immutable, and recorded for the integrity audit ---
install -m 700 "$S/aegis" /usr/sbin/aegis
chown -R root:root /usr/local/share/realone /usr/local/bin/RealOneOS
chmod -R go-w,a+rX /usr/local/share/realone; chmod 755 /usr/local/bin/RealOneOS
sha256sum /usr/local/share/realone/apps/*.RAK > /usr/local/share/realone/apps.sha256   # the only apps the OS will ever load
chmod 644 /usr/local/share/realone/apps.sha256
{ find /usr/local/share/realone -type f; ls /usr/local/bin/RealOneOS /usr/local/sbin/realone-* /etc/systemd/system/realone-* /usr/sbin/aegis; } | sort -u | xargs sha256sum > "$S/integrity.sha256"
find /usr/local/share/realone -type f -exec chattr +i {} + 2>/dev/null || true
chattr -i /usr/local/share/realone/apps 2>/dev/null || true
chattr +i /usr/local/bin/RealOneOS /usr/local/sbin/realone-* /etc/systemd/system/realone-* /usr/sbin/aegis 2>/dev/null || echo "note: immutable flag not supported here, relying on root ownership"
# --- vault ---
umask 077
install -d -m 700 "$V"
head -c 64 /dev/urandom > "$V/key"                      # 32 bytes encryption key + 32 bytes MAC key, unique per image
tar -C "$S" -c --exclude=aegis . | openssl enc -aes-256-cbc -pbkdf2 -iter 600000 -salt \
    -pass "pass:$(head -c 32 "$V/key" | base64 -w0)" -out "$V/vault.enc"
openssl dgst -sha256 -hmac "$(tail -c +33 "$V/key" | base64 -w0)" -r < "$V/vault.enc" | cut -d' ' -f1 > "$V/vault.mac"
chown -R root:root "$V"
chmod 400 "$V/key" "$V/vault.enc" "$V/vault.mac"
chattr +i "$V"/key "$V"/vault.enc "$V"/vault.mac 2>/dev/null || echo "note: immutable flag not supported here, relying on 700/400 permissions"
# remove the plaintext copy
find "$S" -type f ! -name lockdown.sh -exec shred -u {} + 2>/dev/null || true
rm -rf "$S"
