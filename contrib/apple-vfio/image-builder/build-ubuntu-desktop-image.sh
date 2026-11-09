#!/usr/bin/env bash
#
# Build a prebaked Ubuntu Desktop qcow2 artifact for apple-vfio.
#
# The pipeline selects an Ubuntu cloud image and QEMU target from GUEST_ARCH,
# then provisions it into a desktop-oriented image under QEMU/HVF on macOS.
#
# Usage:
#   ./contrib/apple-vfio/image-builder/build-ubuntu-desktop-image.sh [output-dir]
#
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
        DEFAULT_BASE_IMAGE="resolute-server-cloudimg-arm64.img"
        EFI_CODE_NAME="edk2-aarch64-code.fd"
        EFI_VARS_NAMES=("edk2-arm-vars.fd" "edk2-aarch64-vars.fd")
        DEFAULT_INSTALL_FEX=1
        DEFAULT_INSTALL_STEAM=1
        DEFAULT_INSTALL_NVIDIA=1
        ;;
    x86_64)
        QEMU_TARGET="x86_64"
        QEMU_MACHINE="q35"
        DEFAULT_IMAGE_NAME="ubuntu-desktop-resolute-x86_64"
        DEFAULT_BASE_IMAGE="resolute-server-cloudimg-amd64.img"
        EFI_CODE_NAME="edk2-x86_64-code.fd"
        EFI_VARS_NAMES=("edk2-i386-vars.fd" "edk2-x86_64-vars.fd")
        DEFAULT_INSTALL_FEX=0
        DEFAULT_INSTALL_STEAM=1
        DEFAULT_INSTALL_NVIDIA=0
        ;;
    *)
        printf '[build-ubuntu-desktop-image] error: unsupported GUEST_ARCH=%s (use arm64 or x86_64)\n' "$GUEST_ARCH" >&2
        exit 1
        ;;
esac

HOST_ARCH="$(uname -m)"
if [ "$GUEST_ARCH" != "$HOST_ARCH" ]; then
    printf '[build-ubuntu-desktop-image] error: HVF requires GUEST_ARCH=%s on this %s host\n' \
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
CACHE_DIR="$OUT_DIR/cache"
WORK_DIR="$OUT_DIR/work"
ARTIFACT_DIR="$OUT_DIR/artifacts"

IMAGE_NAME="${IMAGE_NAME:-$DEFAULT_IMAGE_NAME}"
BASE_IMAGE="${BASE_IMAGE:-$DEFAULT_BASE_IMAGE}"
BASE_IMAGE_URL="${BASE_IMAGE_URL:-https://cloud-images.ubuntu.com/resolute/current/$BASE_IMAGE}"
DISK_SIZE="${DISK_SIZE:-96G}"
CPUS="${CPUS:-8}"
MEMORY="${MEMORY:-12G}"

BUILD_HOSTNAME="${BUILD_HOSTNAME:-ubuntu-resolute-desktop}"
BUILD_USER="${BUILD_USER:-ubuntu}"
BUILD_PASSWORD="${BUILD_PASSWORD:-ubuntu}"

INSTALL_FEX="${INSTALL_FEX:-$DEFAULT_INSTALL_FEX}"
INSTALL_STEAM="${INSTALL_STEAM:-1}"
INSTALL_NVIDIA="${INSTALL_NVIDIA:-$DEFAULT_INSTALL_NVIDIA}"
INSTALL_APPLE_DMA="${INSTALL_APPLE_DMA:-1}"

APPLE_DMA_SRC_DIR="${APPLE_DMA_SRC_DIR:-$REPO_ROOT/contrib/apple-dma/linux}"
APPLE_DMA_PROTOCOL_HEADER="${APPLE_DMA_PROTOCOL_HEADER:-$REPO_ROOT/contrib/apple-dma/apple-dma-protocol.h}"
APPLE_DMA_VERSION="${APPLE_DMA_VERSION:-0.1.0}"

QEMU_BIN="${QEMU_BIN:-}"
QEMU_IMG_BIN="${QEMU_IMG_BIN:-}"
QEMU_DATA_DIR="${QEMU_DATA_DIR:-}"
EFI_CODE_SRC="${EFI_CODE_SRC:-}"
EFI_VARS_SRC="${EFI_VARS_SRC:-}"

BASE_IMAGE_PATH="$CACHE_DIR/$BASE_IMAGE"
BUILD_DISK="$WORK_DIR/${IMAGE_NAME}.build.qcow2"
SEED_ISO="$WORK_DIR/${IMAGE_NAME}.seed.iso"
EFI_CODE_PATH="$WORK_DIR/$EFI_CODE_NAME"
EFI_VARS_PATH="$WORK_DIR/${EFI_VARS_NAMES[0]}"
BUILD_LOG="$WORK_DIR/${IMAGE_NAME}.build.log"
FINAL_IMAGE="$ARTIFACT_DIR/${IMAGE_NAME}.qcow2"
MANIFEST_PATH="$ARTIFACT_DIR/${IMAGE_NAME}.manifest.json"

pick_existing() {
    local candidate
    for candidate in "$@"; do
        if [ -n "$candidate" ] && [ -e "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

log() {
    printf '[build-ubuntu-desktop-image] %s\n' "$*"
}

die() {
    printf '[build-ubuntu-desktop-image] error: %s\n' "$*" >&2
    exit 1
}

need_tool() {
    command -v "$1" >/dev/null 2>&1 || die "required tool not found: $1"
}

resolve_qemu_img() {
    if [ -z "$QEMU_IMG_BIN" ]; then
        QEMU_IMG_BIN="$(pick_existing \
            "$REPO_ROOT/dist/qemu-img" \
            "$REPO_ROOT/build/qemu-img" \
        )" || die "qemu-img not found in dist/ or build/; set QEMU_IMG_BIN"
    fi
}

ensure_inputs() {
    need_tool curl
    need_tool hdiutil
    need_tool python3
    need_tool shasum

    if [ -z "$QEMU_BIN" ]; then
        QEMU_BIN="$(pick_existing \
            "$REPO_ROOT/dist/qemu-system-$QEMU_TARGET" \
            "$REPO_ROOT/build/qemu-system-$QEMU_TARGET" \
        )" || die "qemu-system-$QEMU_TARGET not found in dist/ or build/"
    fi

    resolve_qemu_img

    if [ -z "$QEMU_DATA_DIR" ]; then
        QEMU_DATA_DIR="$(pick_existing \
            "$REPO_ROOT/dist/share/qemu" \
            "$REPO_ROOT/build/qemu-bundle/usr/local/share/qemu" \
            "$REPO_ROOT/build/pc-bios" \
        )" || die "QEMU data directory not found; set QEMU_DATA_DIR explicitly"
    fi

    if [ -z "$EFI_CODE_SRC" ]; then
        EFI_CODE_SRC="$(pick_existing \
            "$REPO_ROOT/build/pc-bios/$EFI_CODE_NAME" \
            "$REPO_ROOT/build/qemu-bundle/usr/local/share/qemu/$EFI_CODE_NAME" \
            "$REPO_ROOT/dist/share/qemu/$EFI_CODE_NAME" \
        )" || die "$EFI_CODE_NAME not found; set EFI_CODE_SRC to a valid firmware blob"
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

prepare_dirs() {
    mkdir -p "$CACHE_DIR" "$WORK_DIR" "$ARTIFACT_DIR"
}

download_base_image() {
    if [ -f "$BASE_IMAGE_PATH" ]; then
        log "using cached base image: $BASE_IMAGE_PATH"
        return
    fi

    log "downloading base image: $BASE_IMAGE_URL"
    curl -L --fail --continue-at - --progress-bar \
        -o "$BASE_IMAGE_PATH" "$BASE_IMAGE_URL"
}

prepare_build_disk() {
    [ ! -e "$BUILD_DISK" ] || die "build disk already exists: $BUILD_DISK"
    log "creating build disk: $BUILD_DISK"
    "$QEMU_IMG_BIN" create -f qcow2 \
        -b "$BASE_IMAGE_PATH" -F qcow2 \
        "$BUILD_DISK" "$DISK_SIZE" >/dev/null
}

prepare_firmware() {
    cp -f "$EFI_CODE_SRC" "$EFI_CODE_PATH"
    cp -f "$EFI_VARS_SRC" "$EFI_VARS_PATH"
}

render_seed_iso() {
    local seed_dir="$WORK_DIR/seed-files"
    local seed_output="$WORK_DIR/${IMAGE_NAME}.seed"
    rm -rf "$seed_dir"
    mkdir -p "$seed_dir"
    cp "$SCRIPT_DIR/cloud-init/meta-data" "$seed_dir/meta-data"

    local apple_dma_tgz="$WORK_DIR/apple-dma-src.tgz"
    local apple_dma_tar="$WORK_DIR/apple-dma-src.tar"
    log "tarring apple-dma source from $APPLE_DMA_SRC_DIR"
    [ -f "$APPLE_DMA_PROTOCOL_HEADER" ] ||
        die "apple-dma protocol header not found: $APPLE_DMA_PROTOCOL_HEADER"
    tar -cf "$apple_dma_tar" -C "$APPLE_DMA_SRC_DIR" \
        --exclude='*.o' --exclude='*.ko' --exclude='*.mod*' \
        --exclude='Module.symvers' --exclude='modules.order' \
        --exclude='.tmp_versions' --exclude='.*.cmd' \
        --exclude='test-kmod-build.sh' \
        .
    tar -rf "$apple_dma_tar" -C "$(dirname "$APPLE_DMA_PROTOCOL_HEADER")" \
        "$(basename "$APPLE_DMA_PROTOCOL_HEADER")"
    gzip -c "$apple_dma_tar" > "$apple_dma_tgz"
    rm -f "$apple_dma_tar"

    BUILD_HOSTNAME="$BUILD_HOSTNAME" \
    BUILD_USER="$BUILD_USER" \
    BUILD_PASSWORD="$BUILD_PASSWORD" \
    INSTALL_FEX="$INSTALL_FEX" \
    INSTALL_STEAM="$INSTALL_STEAM" \
    INSTALL_NVIDIA="$INSTALL_NVIDIA" \
    INSTALL_APPLE_DMA="$INSTALL_APPLE_DMA" \
    APPLE_DMA_VERSION="$APPLE_DMA_VERSION" \
    APPLE_DMA_TGZ="$apple_dma_tgz" \
    python3 - "$SCRIPT_DIR/cloud-init/user-data.template" \
        "$SCRIPT_DIR/guest/provision-desktop.sh" \
        "$seed_dir/user-data" <<'PY'
from pathlib import Path
import base64
import os
import sys

template = Path(sys.argv[1]).read_text()
guest = Path(sys.argv[2]).read_text().rstrip().splitlines()
indented_guest = "\n".join(
    ("      " + line) if line else "      "
    for line in guest
)

apple_dma_b64 = base64.b64encode(Path(os.environ["APPLE_DMA_TGZ"]).read_bytes()).decode("ascii")
apple_dma_lines = [apple_dma_b64[i : i + 76] for i in range(0, len(apple_dma_b64), 76)]
indented_apple_dma = "\n".join("      " + line for line in apple_dma_lines)

replacements = {
    "@@BUILD_HOSTNAME@@": os.environ["BUILD_HOSTNAME"],
    "@@BUILD_USER@@": os.environ["BUILD_USER"],
    "@@BUILD_PASSWORD@@": os.environ["BUILD_PASSWORD"],
    "@@INSTALL_FEX@@": os.environ["INSTALL_FEX"],
    "@@INSTALL_STEAM@@": os.environ["INSTALL_STEAM"],
    "@@INSTALL_NVIDIA@@": os.environ["INSTALL_NVIDIA"],
    "@@INSTALL_APPLE_DMA@@": os.environ["INSTALL_APPLE_DMA"],
    "@@APPLE_DMA_VERSION@@": os.environ["APPLE_DMA_VERSION"],
    "@@PROVISION_SCRIPT@@": indented_guest,
    "@@APPLE_DMA_SRC_B64@@": indented_apple_dma,
}

for old, new in replacements.items():
    template = template.replace(old, new)

Path(sys.argv[3]).write_text(template + "\n")
PY

    rm -rf "$SEED_ISO" "$seed_output" "$seed_output.iso" "$seed_output.cdr" \
        "$seed_output.iso.cdr"

    hdiutil makehybrid -iso -joliet -default-volume-name cidata \
        -o "$seed_output" "$seed_dir" >/dev/null

    if [ -f "$seed_output.iso" ]; then
        mv "$seed_output.iso" "$SEED_ISO"
    elif [ -f "$seed_output.cdr" ]; then
        mv "$seed_output.cdr" "$SEED_ISO"
    elif [ -f "$seed_output.iso.cdr" ]; then
        mv "$seed_output.iso.cdr" "$SEED_ISO"
    elif [ -f "$seed_output" ]; then
        mv "$seed_output" "$SEED_ISO"
    else
        die "failed to create cloud-init seed image"
    fi
}

run_builder_vm() {
    rm -f "$BUILD_LOG"
    log "booting builder VM"
    if "$QEMU_BIN" \
        -L "$QEMU_DATA_DIR" \
        -accel hvf \
        -cpu host \
        -machine "$QEMU_MACHINE" \
        -smp "$CPUS" \
        -m "$MEMORY" \
        -serial mon:stdio \
        -display none \
        -device virtio-rng-pci \
        -drive if=pflash,format=raw,readonly=on,file="$EFI_CODE_PATH" \
        -drive if=pflash,format=raw,file="$EFI_VARS_PATH" \
        -drive if=virtio,format=qcow2,file="$BUILD_DISK",discard=unmap,detect-zeroes=unmap \
        -drive if=virtio,format=raw,readonly=on,file="$SEED_ISO" \
        -netdev user,id=net0 \
        -device virtio-net-pci,netdev=net0 \
        2>&1 | tee "$BUILD_LOG"; then
        return 0
    else
        local -a pipeline_status=("${PIPESTATUS[@]}")
        die "builder VM or log writer failed (QEMU=${pipeline_status[0]}, tee=${pipeline_status[1]}); inspect $BUILD_LOG. The build disk was retained; rerun to recover after a confirmed guest shutdown"
    fi
}

assert_success_log() {
    if ! grep -q "provisioning complete, powering off" "$BUILD_LOG"; then
        die "builder VM exited without the success marker; inspect $BUILD_LOG"
    fi
}

emit_artifact() {
    local temporary_image

    [ ! -e "$FINAL_IMAGE" ] || die "artifact already exists: $FINAL_IMAGE"
    log "flattening build disk into artifact"
    temporary_image="$(mktemp "$ARTIFACT_DIR/.${IMAGE_NAME}.qcow2.XXXXXX")" ||
        die "cannot create temporary image in $ARTIFACT_DIR"
    if ! "$QEMU_IMG_BIN" convert -p -O qcow2 -c "$BUILD_DISK" "$temporary_image"; then
        rm -f "$temporary_image"
        die "image conversion failed; retained build disk at $BUILD_DISK"
    fi
    if [ ! -s "$temporary_image" ] ||
       ! "$QEMU_IMG_BIN" check -f qcow2 "$temporary_image"; then
        rm -f "$temporary_image"
        die "converted image is missing or invalid; retained build disk at $BUILD_DISK"
    fi
    if ! ln "$temporary_image" "$FINAL_IMAGE"; then
        rm -f "$temporary_image"
        die "cannot publish artifact (it may already exist): $FINAL_IMAGE"
    fi
    rm -f "$temporary_image"
    [ -s "$FINAL_IMAGE" ] || die "published artifact is missing or empty: $FINAL_IMAGE"
}

recover_build_disk() {
    [ ! -e "$FINAL_IMAGE" ] ||
        die "build disk and artifact already exist; refusing to overwrite either: $FINAL_IMAGE"
    [ ! -e "$MANIFEST_PATH" ] ||
        die "manifest already exists without an artifact; inspect $MANIFEST_PATH before recovery"
    [ -s "$BUILD_LOG" ] || die "build disk exists but its build log is missing: $BUILD_LOG"
    assert_success_log
    grep -q 'reboot: Power down' "$BUILD_LOG" ||
        die "guest shutdown is not confirmed in $BUILD_LOG; refusing to convert $BUILD_DISK"
    resolve_qemu_img
    mkdir -p "$ARTIFACT_DIR"
    log "checking existing build disk before recovery"
    "$QEMU_IMG_BIN" check -f qcow2 "$BUILD_DISK" ||
        die "build disk check failed; ensure the VM is stopped and inspect $BUILD_DISK"
    emit_artifact
    log "artifact recovered: $FINAL_IMAGE"
    log "no manifest was generated: original build settings cannot be verified"
}

write_manifest() {
    python3 - "$MANIFEST_PATH" <<'PY'
from pathlib import Path
import json
import os
import sys

manifest = {
    "name": os.environ["IMAGE_NAME"],
    "guest_arch": os.environ["GUEST_ARCH"],
    "artifact": Path(os.environ["FINAL_IMAGE"]).name,
    "base_image": os.environ["BASE_IMAGE"],
    "base_image_url": os.environ["BASE_IMAGE_URL"],
    "disk_size": os.environ["DISK_SIZE"],
    "cpus": os.environ["CPUS"],
    "memory": os.environ["MEMORY"],
    "build_user": os.environ["BUILD_USER"],
    "features": {
        "desktop": True,
        "fex": os.environ["INSTALL_FEX"] == "1",
        "steam": os.environ["INSTALL_STEAM"] == "1",
        "nvidia": os.environ["INSTALL_NVIDIA"] == "1",
        "apple_dma": any(
            "apple_dma_ready=true" in line
            for line in Path(os.environ["BUILD_LOG"]).read_text().splitlines()
        ),
    },
    "sha256": os.environ["ARTIFACT_SHA256"],
    "size_bytes": int(os.environ["ARTIFACT_SIZE"]),
}

Path(sys.argv[1]).write_text(json.dumps(manifest, indent=2) + "\n")
PY
}

main() {
    if [ -e "$BUILD_DISK" ]; then
        recover_build_disk
        return
    fi
    [ ! -e "$FINAL_IMAGE" ] ||
        die "artifact already exists; refusing to overwrite it: $FINAL_IMAGE"
    [ ! -e "$MANIFEST_PATH" ] ||
        die "manifest already exists; refusing to overwrite it: $MANIFEST_PATH"
    ensure_inputs
    prepare_dirs
    download_base_image
    prepare_build_disk
    prepare_firmware
    render_seed_iso
    run_builder_vm
    assert_success_log
    emit_artifact

    ARTIFACT_SHA256="$(shasum -a 256 "$FINAL_IMAGE" | awk '{print $1}')"
    ARTIFACT_SIZE="$(stat -f '%z' "$FINAL_IMAGE")"
    export ARTIFACT_SHA256 ARTIFACT_SIZE
    export GUEST_ARCH IMAGE_NAME FINAL_IMAGE BASE_IMAGE BASE_IMAGE_URL DISK_SIZE CPUS MEMORY
    export BUILD_USER INSTALL_FEX INSTALL_STEAM INSTALL_NVIDIA INSTALL_APPLE_DMA
    export BUILD_LOG

    write_manifest

    log "artifact ready: $FINAL_IMAGE"
    log "manifest ready: $MANIFEST_PATH"
    log "build log: $BUILD_LOG"
}

main "$@"
