#!/usr/bin/env bash
#
# make-woa-usb.sh — build a Windows-on-ARM install stick that boots on firmware
#                   with no persistent UEFI variables.
#
# Windows ISOs already ship \efi\boot\bootaa64.efi, which is the removable-media
# fallback path, so Setup itself needs no NVRAM entry. (Making the *installed*
# system boot without NVRAM is a separate step — see docs/INSTALL.md.)
#
# install.wim is often larger than FAT32's 4 GiB per-file limit. It is copied
# as-is when it fits and split into install.swm parts when it does not; Windows
# Setup picks the split form up automatically. A single install.wim is the more
# well-trodden path, so it is preferred whenever the size allows.
#
# Usage:
#   sudo bash tools/make-woa-usb.sh /dev/sdX /path/to/mounted/iso
#
# Refuses to touch anything that is not a removable USB disk.
#
set -euo pipefail

DEV="${1:-}"
SRC="${2:-}"
SPLIT_MIB=3800          # comfortably under FAT32's 4 GiB per-file limit

die()  { echo "ERROR: $*" >&2; exit 1; }
info() { echo "[*] $*"; }

[ -n "$DEV" ] && [ -n "$SRC" ] || die "usage: sudo bash $0 /dev/sdX /path/to/mounted/iso"
[ "$(id -u)" -eq 0 ] || die "must run as root (partitioning + mkfs)"
[ -b "$DEV" ] || die "$DEV is not a block device"
[ -d "$SRC" ] || die "$SRC is not a directory (mount the ISO first)"
[ -f "$SRC/sources/install.wim" ] || die "$SRC/sources/install.wim not found — is that a Windows ISO?"
command -v wimsplit  >/dev/null || die "wimsplit missing (apt install wimtools)"
command -v sgdisk    >/dev/null || die "sgdisk missing (apt install gdisk)"
command -v mkfs.vfat >/dev/null || die "mkfs.vfat missing (apt install dosfstools)"

# ---------------------------------------------------------------- safety gates
BASE="$(basename "$DEV")"
case "$BASE" in
  *[0-9]) die "$DEV looks like a partition; pass the whole disk (e.g. /dev/sdd)" ;;
esac
[ -e "/sys/block/$BASE" ] || die "no /sys/block/$BASE"
[ "$(cat "/sys/block/$BASE/removable")" = "1" ] || die "$DEV is not removable — refusing"
[ "$(lsblk -ndo TRAN "$DEV")" = "usb" ] || die "$DEV is not on the USB bus — refusing"

ROOTDISK="$(lsblk -ndo PKNAME "$(findmnt -no SOURCE /)" 2>/dev/null || true)"
[ "$BASE" = "$ROOTDISK" ] && die "$DEV is the root disk — refusing"

SIZE_GB=$(( $(blockdev --getsize64 "$DEV") / 1000000000 ))
[ "$SIZE_GB" -ge 8 ]  || die "$DEV is only ${SIZE_GB} GB; need >= 8 GB"
[ "$SIZE_GB" -le 256 ] || die "$DEV is ${SIZE_GB} GB — suspiciously large for a stick, refusing"

echo
echo "  About to ERASE EVERYTHING on:"
lsblk -o NAME,SIZE,TYPE,TRAN,MODEL,LABEL,MOUNTPOINT "$DEV"
echo
echo "  Source: $SRC"
echo "  install.wim: $(du -h "$SRC/sources/install.wim" | cut -f1)"
echo
read -r -p "  Type ERASE to continue: " CONFIRM
[ "$CONFIRM" = "ERASE" ] || die "aborted"

# ---------------------------------------------------------------- partitioning
info "unmounting any partitions on $DEV"
while read -r mp; do
  [ -n "$mp" ] && { info "  umount $mp"; umount "$mp" || umount -l "$mp"; }
done < <(lsblk -nro MOUNTPOINT "$DEV")

info "wiping partition table"
wipefs -a "$DEV" >/dev/null
sgdisk --zap-all "$DEV" >/dev/null

info "creating a single ESP spanning the disk"
sgdisk --new=1:1MiB:0 --typecode=1:EF00 --change-name=1:WOASETUP "$DEV" >/dev/null
partprobe "$DEV"; udevadm settle; sleep 1

# p1 / 1 depending on device naming (sdd1 vs nvme0n1p1 vs mmcblk0p1)
PART="${DEV}1"; [ -b "$PART" ] || PART="${DEV}p1"
[ -b "$PART" ] || die "partition node did not appear for $DEV"

info "formatting $PART as FAT32"
mkfs.vfat -F 32 -n WOASETUP "$PART" >/dev/null

MNT="$(mktemp -d)"
cleanup() { umount "$MNT" 2>/dev/null || true; rmdir "$MNT" 2>/dev/null || true; }
trap cleanup EXIT
mount "$PART" "$MNT"

# ---------------------------------------------------------------- copy payload
WIM_BYTES=$(stat -c%s "$SRC/sources/install.wim")
FAT32_MAX=4294967295

info "copying ISO contents (install.wim handled separately)"
rsync -rlt --info=progress2 --exclude='sources/install.wim' "$SRC"/ "$MNT"/

if [ "$WIM_BYTES" -lt "$FAT32_MAX" ]; then
  info "install.wim is $WIM_BYTES B, under FAT32's $FAT32_MAX limit — copying whole"
  rsync -lt --info=progress2 "$SRC/sources/install.wim" "$MNT/sources/install.wim"
else
  info "install.wim is $WIM_BYTES B, over FAT32's limit — splitting into ${SPLIT_MIB} MiB parts"
  wimsplit "$SRC/sources/install.wim" "$MNT/sources/install.swm" "$SPLIT_MIB"
fi

sync
echo
info "done — install image on the stick:"
ls -la "$MNT/sources"/install.wim "$MNT/sources"/install.swm* 2>/dev/null || true
echo
echo "  Boot file present: $([ -f "$MNT/efi/boot/bootaa64.efi" ] && echo yes || echo 'NO — Setup will not boot!')"
df -h "$MNT" | tail -1
echo
echo "  Now: plug into the board, power on, and pick the USB from the EDK2 boot menu."
echo "  See docs/INSTALL.md for the post-install NVRAM-less boot fixup."
