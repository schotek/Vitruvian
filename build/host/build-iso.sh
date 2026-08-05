#!/usr/bin/env bash
#
# Builds a bootable Vitruvian ISO (chroot + image) from a non-Debian host
# (tested on Ubuntu 26.04 / glibc 2.43 / gcc 15). The host compiles against a
# Debian-trixie chroot, so the result runs regardless of the host's lib versions.
#
# Run as a NORMAL user (NOT under sudo!):
#     bash build/host/build-iso.sh
#
# The script requests sudo itself, only where needed (apt install, debootstrap
# inside bake). Do not run it under sudo - the output files would end up owned
# by root.
#
# Optional environment variables:
#     ARCH=amd64   target architecture (default amd64 -> generated.amd64)
#
set -euo pipefail

# Derive the repo path from the script's own location (build/host/ -> repo) so
# this works for anyone after `git clone`, not just on one machine.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
ARCH="${ARCH:-amd64}"
GEN="$REPO/generated.$ARCH"

if [ "$(id -u)" -eq 0 ]; then
	echo "!! Do not run as root/sudo. Run as a normal user:" >&2
	echo "     bash $0" >&2
	echo "   (the script requests sudo itself for apt and debootstrap.)" >&2
	exit 1
fi

if [ ! -d "$GEN" ]; then
	echo "!! Could not find $GEN - has the tree been configured yet?" >&2
	echo "   Configure the build tree first (see wiki.w-os.dev)." >&2
	exit 1
fi

echo "==> (1/4) Installing the last build dependency (libbfd-dev)..."
sudo apt install -y libbfd-dev

cd "$GEN"

echo "==> (2/4) Creating chroot (debootstrap - downloads ~hundreds of MB, a few minutes)..."
# Distro packages land in the image rootfs ONLY when the chroot is
# (re)created (chroot_create in build/scripts/lib/chroot.sh) — an existing
# image_tree/chroot silently keeps its old package set. The check must run
# BEFORE ../configure: cmake resolves libraries from the chroot sysroot
# (pkg_check_modules), so against a stale chroot configure hard-fails and a
# later regeneration step would never be reached — a permanent wedge.
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
	# Stamp immediately after the regeneration: a compile failure later in
	# this run must not re-trigger a full debootstrap on the next attempt.
	printf '%s\n' "$_pkg_hash" > "$_pkg_stamp"
fi

# --enable-wayland: build the Vitrine nested Wayland compositor (amd64 only
# so far). Without it a fresh generated dir would silently configure with the
# compositor off and the ISO smoke tests would test the wrong image.
_configure_extra=""
if [ "$ARCH" = "amd64" ]; then
	_configure_extra="--enable-wayland"
fi
../configure --chroot-build --buildtools=../buildtools $_configure_extra
# A fresh tree had no chroot before configure — it was just created from the
# current package lists, so record them (no-op when already stamped above).
printf '%s\n' "$_pkg_hash" > "$_pkg_stamp"

echo "==> (3/4) Baking the ISO image (bake build --image-type=iso)..."
../bake build --image-type=iso

echo "==> (4/4) Locating the resulting ISO..."
iso="$(find "$GEN/image_tree" "$GEN/output" -maxdepth 2 -name '*.iso' 2>/dev/null | head -1 || true)"
if [ -n "$iso" ]; then
	ls -lh "$iso"
	echo
	echo "[+] Done. Boot it in QEMU:   bash build/host/boot.sh"
else
	echo "!! ISO not found - check the bake output above." >&2
	exit 1
fi
