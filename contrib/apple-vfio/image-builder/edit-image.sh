#!/usr/bin/env bash
#
# Boot the prebaked qcow2 artifact in an interactive QEMU session so
# you can click through GUI-only steps that cloud-init can't do:
#   - complete Steam's initial sign-in / EULA / offline mode prompt
#   - right-click Steam's desktop icon and "Allow launching" so it
#     loses the red-X untrusted badge
#   - accept the NVIDIA driver's "first boot" prompts if any show up
#   - any other manual GNOME / first-run gunk that is easier to
#     click through than to script
#
# The edit happens on a scratch copy in work/, not on the released
# artifact, so repeated edit/seal cycles are cheap and the original
# is never modified in-place. `seal-image.sh` recompacts the scratch
# back into the release artifact when you're done.
#
# Usage:
#   ./contrib/apple-vfio/image-builder/edit-image.sh [output-dir]
#
# Environment: same as build-ubuntu-desktop-image.sh. The script
# imports that file to avoid duplicating QEMU discovery, firmware
# paths, image names, etc.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

OUTPUT_DIR="${1:-}"

GUEST_ARCH="${GUEST_ARCH:-$(uname -m)}"
case "$GUEST_ARCH" in
    arm64)
        QEMU_TARGET="aarch64"
        QEMU_MACHINE="virt,highmem=on"
        DEFAULT_IMAGE_NAME="ubuntu-desktop-resolute-arm64"
        EFI_CODE_NAME="edk2-aarch64-code.fd"
        EFI_VARS_NAMES=("edk2-arm-vars.fd" "edk2-aarch64-vars.fd")
        ;;
    x86_64)
        QEMU_TARGET="x86_64"
        QEMU_MACHINE="q35"
        DEFAULT_IMAGE_NAME="ubuntu-desktop-resolute-x86_64"
        EFI_CODE_NAME="edk2-x86_64-code.fd"
        EFI_VARS_NAMES=("edk2-i386-vars.fd" "edk2-x86_64-vars.fd")
        ;;
    *)
        printf '[edit-image] error: unsupported GUEST_ARCH=%s\n' "$GUEST_ARCH" >&2
        exit 1
        ;;
esac
HOST_ARCH="$(uname -m)"
if [ "$GUEST_ARCH" != "$HOST_ARCH" ]; then
    printf '[edit-image] error: HVF requires GUEST_ARCH=%s on this %s host\n' \
        "$HOST_ARCH" "$HOST_ARCH" >&2
    exit 1
fi
if [ -n "$OUTPUT_DIR" ]; then
    OUT_DIR="$OUTPUT_DIR"
elif [ "$GUEST_ARCH" = "arm64" ]; then
    OUT_DIR="$REPO_ROOT/contrib/apple-vfio/image-builder/out/ubuntu-desktop-image"
else
    OUT_DIR="$REPO_ROOT/contrib/apple-vfio/image-builder/out/ubuntu-desktop-image-x86_64"
fi
WORK_DIR="$OUT_DIR/work"
ARTIFACT_DIR="$OUT_DIR/artifacts"
IMAGE_NAME="${IMAGE_NAME:-$DEFAULT_IMAGE_NAME}"
CPUS="${CPUS:-8}"
MEMORY="${MEMORY:-12G}"
EDIT_SSH_PORT="${EDIT_SSH_PORT:-2233}"
QEMU_DISPLAY="${QEMU_DISPLAY:-cocoa,full-grab=on}"

FINAL_IMAGE="$ARTIFACT_DIR/${IMAGE_NAME}.qcow2"
EDIT_IMAGE="$WORK_DIR/${IMAGE_NAME}.edit.qcow2"
EDIT_EFI_VARS="$WORK_DIR/${IMAGE_NAME}.edit-vars.fd"
EDIT_LOG="$WORK_DIR/${IMAGE_NAME}.edit.log"

QEMU_BIN="${QEMU_BIN:-}"
QEMU_IMG_BIN="${QEMU_IMG_BIN:-}"
QEMU_DATA_DIR="${QEMU_DATA_DIR:-}"
EFI_CODE_SRC="${EFI_CODE_SRC:-}"
EFI_VARS_SRC="${EFI_VARS_SRC:-}"

log() { printf '[edit-image] %s\n' "$*"; }
die() { printf '[edit-image] error: %s\n' "$*" >&2; exit 1; }

pick_existing() {
    local c
    for c in "$@"; do
        if [ -n "$c" ] && [ -e "$c" ]; then
            printf '%s\n' "$c"
            return 0
        fi
    done
    return 1
}

resolve_qemu() {
    if [ -z "$QEMU_BIN" ]; then
        QEMU_BIN="$(pick_existing \
            "$REPO_ROOT/dist/qemu-system-$QEMU_TARGET" \
            "$REPO_ROOT/build/qemu-system-$QEMU_TARGET" \
        )" || die "qemu-system-$QEMU_TARGET not found in dist/ or build/"
    fi
    if [ -z "$QEMU_IMG_BIN" ]; then
        QEMU_IMG_BIN="$(pick_existing \
            "$REPO_ROOT/dist/qemu-img" \
            "$REPO_ROOT/build/qemu-img" \
        )" || die "qemu-img not found in dist/ or build/"
    fi
    if [ -z "$QEMU_DATA_DIR" ]; then
        QEMU_DATA_DIR="$(pick_existing \
            "$REPO_ROOT/dist/share/qemu" \
            "$REPO_ROOT/build/qemu-bundle/usr/local/share/qemu" \
            "$REPO_ROOT/build/pc-bios" \
        )" || die "QEMU data dir not found; set QEMU_DATA_DIR"
    fi
    if [ -z "$EFI_CODE_SRC" ]; then
        EFI_CODE_SRC="$(pick_existing \
            "$REPO_ROOT/build/pc-bios/$EFI_CODE_NAME" \
            "$REPO_ROOT/build/qemu-bundle/usr/local/share/qemu/$EFI_CODE_NAME" \
            "$REPO_ROOT/dist/share/qemu/$EFI_CODE_NAME" \
        )" || die "$EFI_CODE_NAME not found; set EFI_CODE_SRC"
    fi
    if [ -z "$EFI_VARS_SRC" ]; then
        local vars_candidates=()
        local vars_name
        for vars_name in "${EFI_VARS_NAMES[@]}"; do
            vars_candidates+=(
                "$REPO_ROOT/build/pc-bios/$vars_name"
                "$REPO_ROOT/build/qemu-bundle/usr/local/share/qemu/$vars_name"
                "$REPO_ROOT/dist/share/qemu/$vars_name"
            )
        done
        EFI_VARS_SRC="$(pick_existing "${vars_candidates[@]}")" ||
            die "firmware vars template not found for $GUEST_ARCH; set EFI_VARS_SRC"
    fi
}

prepare_edit_disk() {
    [ -f "$FINAL_IMAGE" ] || die "no artifact at $FINAL_IMAGE — run build-ubuntu-desktop-image.sh first"
    mkdir -p "$WORK_DIR"

    if [ ! -f "$EDIT_IMAGE" ]; then
        log "copying $FINAL_IMAGE -> $EDIT_IMAGE (initial scratch disk)"
        # Uncompressed convert so the disk writes fast during edits.
        # seal-image.sh re-compresses on the way back to the artifact.
        "$QEMU_IMG_BIN" convert -p -O qcow2 "$FINAL_IMAGE" "$EDIT_IMAGE"
    else
        log "reusing existing scratch disk at $EDIT_IMAGE"
        log "  (delete it to start over from the released artifact)"
    fi

    if [ ! -f "$EDIT_EFI_VARS" ]; then
        cp -f "$EFI_VARS_SRC" "$EDIT_EFI_VARS"
    fi
}

run_edit_vm() {
    log "booting edit VM (ssh fwd: 127.0.0.1:$EDIT_SSH_PORT -> guest:22)"
    log "close the QEMU window cleanly via the menu, OR run 'sudo poweroff'"
    log "inside the guest. A hard window-close may corrupt the scratch disk."
    log ""
    log "when done, run: ./contrib/apple-vfio/image-builder/seal-image.sh"
    log ""

    "$QEMU_BIN" \
        -L "$QEMU_DATA_DIR" \
        -accel hvf \
        -cpu host \
        -machine "$QEMU_MACHINE" \
        -smp "$CPUS" \
        -m "$MEMORY" \
        -device virtio-rng-pci \
        -drive if=pflash,format=raw,readonly=on,file="$EFI_CODE_SRC" \
        -drive if=pflash,format=raw,file="$EDIT_EFI_VARS" \
        -drive if=virtio,format=qcow2,file="$EDIT_IMAGE",discard=unmap,detect-zeroes=unmap \
        -netdev user,id=net0,hostfwd=tcp::"$EDIT_SSH_PORT"-:22 \
        -device virtio-net-pci,netdev=net0 \
        -device qemu-xhci,id=xhci \
        -device usb-kbd,bus=xhci.0 \
        -device usb-tablet,bus=xhci.0 \
        -device virtio-gpu-pci \
        -display "$QEMU_DISPLAY" \
        2>&1 | tee "$EDIT_LOG"
}

main() {
    resolve_qemu
    prepare_edit_disk
    run_edit_vm
    log "edit session ended. scratch disk: $EDIT_IMAGE"
    log "to commit changes into the release artifact, run:"
    log "  $SCRIPT_DIR/seal-image.sh $OUT_DIR"
}

main "$@"
