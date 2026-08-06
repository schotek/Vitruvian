#!/usr/bin/env bash
#
# Author: Vláďa Janeček <vlada@janecek.cloud>
#
# Boots a built Vitruvian RAW disk image (UEFI/OVMF) in QEMU/KVM. Needs no
# root (just membership in the `kvm` group). Unlike the live ISO this is a
# persistent system: changes survive reboots, and the very first boot grows
# the root filesystem to fill the virtual disk (vos-resize-root).
#
# Sound: an Intel HDA card (output + mic) is emulated automatically when
# the host QEMU offers a usable audio backend (pipewire/pa/sdl/alsa); see
# build/scripts/lib/qemu.sh.
#
# Usage:
#     bash build/host/boot-raw.sh                          # graphical QEMU window
#     bash build/host/boot-raw.sh --enable-console-stdout  # + serial console to terminal
#
# Optional environment variables:
#     ARCH=amd64      target architecture (default amd64 -> generated.amd64)
#     VM_MEM_MB=8192  guest RAM in MiB (amd64 default 8192). Keep it generous:
#                     the VM has no virtual GPU, so Mesa falls back to
#                     llvmpipe and every texture a 3D app would put in VRAM
#                     lands in guest RAM instead.
#
# Note: the SSH forward (localhost:2222) is shared with the ISO boot — do
# not run both VMs at the same time.
set -euo pipefail

# Derive the repo path from the script's own location (build/host/ -> repo).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
ARCH="${ARCH:-amd64}"
GEN="$REPO/generated.$ARCH"

if [ ! -e /dev/kvm ]; then
	echo "!! /dev/kvm does not exist - KVM is not available." >&2
	exit 1
fi

if [ ! -f "$GEN/output/vitruvian.raw" ]; then
	echo "!! Could not find $GEN/output/vitruvian.raw - build it first:" >&2
	echo "     bash build/host/build-raw.sh" >&2
	exit 1
fi

if [ "$ARCH" = "amd64" ] && [ ! -f /usr/share/OVMF/OVMF_CODE_4M.fd ]; then
	echo "!! /usr/share/OVMF/OVMF_CODE_4M.fd missing - install the 'ovmf' package." >&2
	exit 1
fi

cd "$GEN"
echo "==> Booting Vitruvian RAW image in QEMU (close the QEMU window to quit)..."
exec ../bake boot --image-type=raw "$@"
