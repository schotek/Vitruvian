#!/usr/bin/env bash
#
# Author: Vláďa Janeček <vlada@janecek.cloud>
#
# Builds a bootable Vitruvian RAW disk image (UEFI, GPT: ESP + ext4 root)
# from a non-Debian host, the same way build-iso.sh builds the live ISO.
#
# The image file is SPARSE: it is created at 4G, populated, and then grown
# with `qemu-img resize` to RAW_SIZE (default 64G) — the file *looks* 64G
# but only occupies as much disk as is actually written (~4-5G after the
# build; check with `du -h` vs `ls -lh`). On the FIRST boot the image's
# vos-resize-root unit grows the root partition and filesystem to fill the
# whole virtual disk, then disables itself. Boot the image only after this
# script finishes — a boot before the resize would burn that one-shot unit
# at the small size.
#
# Run as a NORMAL user (NOT under sudo!):
#     bash build/host/build-raw.sh
#
# The script requests sudo itself (apt, loop devices, mounts, chroot).
#
# Optional environment variables:
#     ARCH=amd64      target architecture (default amd64 -> generated.amd64)
#     RAW_SIZE=64G    virtual disk size to grow to; set RAW_SIZE=keep to
#                     leave the image at its built size (4G)
#
# Copying sparse files: plain cp/mv on the same filesystem keep the holes;
# for transfers use `rsync -S` or compress first — scp inflates the file
# to its full apparent size.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
ARCH="${ARCH:-amd64}"
RAW_SIZE="${RAW_SIZE:-64G}"
GEN="$REPO/generated.$ARCH"

if [ "$(id -u)" -eq 0 ]; then
	echo "!! Do not run as root/sudo. Run as a normal user:" >&2
	echo "     bash $0" >&2
	echo "   (the script requests sudo itself where needed.)" >&2
	exit 1
fi

if [ ! -d "$GEN" ]; then
	echo "!! Could not find $GEN - has the tree been configured yet?" >&2
	echo "   Configure the build tree first (see wiki.v-os.dev)." >&2
	exit 1
fi

echo "==> (1/4) Installing host dependencies for the RAW image build..."
# build-iso.sh needs only libbfd-dev; the raw path additionally partitions
# and formats a loop device on the host and builds a standalone EFI grub.
sudo apt install -y libbfd-dev qemu-utils parted rsync dosfstools \
	grub-common grub-efi-amd64-bin ovmf

cd "$GEN"

echo "==> (2/4) Checking the chroot (debootstrap)..."
# Same logic as build-iso.sh: distro packages land in the image rootfs ONLY
# when the chroot is (re)created, and the check must run BEFORE ../configure
# (cmake resolves libraries from the chroot sysroot, so a stale chroot makes
# configure hard-fail before any later regeneration step could run).
_pkg_stamp="$GEN/.packages.stamp"
_pkg_hash="$(sha256sum "$REPO/build/scripts/lib/packages.sh" | cut -d' ' -f1)"
if [ -d "$GEN/image_tree/chroot" ] \
		&& [ "$(cat "$_pkg_stamp" 2>/dev/null)" != "$_pkg_hash" ]; then
	echo "==> Package lists changed since the chroot was created:"
	echo "    regenerating it (re-downloads packages, takes a few minutes)."
	(
		set +u  # the sourced build libs are not `set -u`-clean
		. "$REPO/build/scripts/lib/common.sh"
		. "$REPO/build/scripts/lib/packages.sh"
		. "$REPO/build/scripts/lib/boards.sh"
		. "$REPO/build/scripts/lib/chroot.sh"
		. "$REPO/build/scripts/lib/qemu.sh"
		chroot_regenerate "$GEN" "$ARCH"
	)
	# Stamp immediately: a compile failure later in this run must not
	# re-trigger a full debootstrap on the next attempt.
	printf '%s\n' "$_pkg_hash" > "$_pkg_stamp"
fi

# --enable-wayland: build the Vitrine nested Wayland compositor (amd64 only
# so far), keeping the RAW image feature-equal to the ISO.
_configure_extra=""
if [ "$ARCH" = "amd64" ]; then
	_configure_extra="--enable-wayland"
fi
../configure --chroot-build --buildtools=../buildtools $_configure_extra
printf '%s\n' "$_pkg_hash" > "$_pkg_stamp"

echo "==> (3/4) Baking the RAW image (bake build --image-type=raw)..."
../bake build --image-type=raw

raw="$GEN/output/vitruvian.raw"
if [ ! -f "$raw" ]; then
	echo "!! RAW image not found at $raw - check the bake output above." >&2
	exit 1
fi

echo "==> (4/4) Growing the virtual disk..."
if [ "$RAW_SIZE" != "keep" ]; then
	qemu-img resize -f raw "$raw" "$RAW_SIZE"
	echo "    Grown to $RAW_SIZE (sparse: only written data occupies disk)."
	echo "    The first boot will expand the root filesystem automatically."
else
	echo "    RAW_SIZE=keep - image left at its built size."
fi

echo
echo "    virtual (apparent) size: $(ls -lh "$raw" | awk '{print $5}')"
echo "    actual disk usage:       $(du -h "$raw" | cut -f1)"
echo
echo "[+] Done. Boot it in QEMU:   cd generated.$ARCH && ../bake boot --image-type=raw"
