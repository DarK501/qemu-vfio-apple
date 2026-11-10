#!/bin/bash
#
# Build, ad-hoc sign, verify and optionally install VFIOUserHostApp.
#
# Usage:
#   contrib/apple-vfio/scripts/build-host-app.sh [--version N] [--install]
#
#   --version N  CURRENT_PROJECT_VERSION for the app and dext. Defaults to the
#                installed /Applications version + 1 (or 1 if none).
#   --install    After verifying, back up /Applications/VFIOUserHostApp.app to
#                ~/VFIOUserHostApp-vN-backup-<timestamp>.app, replace it, and
#                open it. Refuses to run while any qemu-system process exists.
#
# After --install, press "Update Driver" in the app. A replaced dext may stay
# "terminating for upgrade" until the Mac is rebooted.
#
# Signing is ad hoc (-). Entitlements come from the repository.

set -euo pipefail

VERSION=""
INSTALL=0
while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION="${2:?--version needs a number}"; shift 2 ;;
        --install) INSTALL=1; shift ;;
        *) echo "usage: $0 [--version N] [--install]" >&2; exit 2 ;;
    esac
done

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
E="$ROOT/contrib/apple-vfio"
INSTALLED=/Applications/VFIOUserHostApp.app
DERIVED="$ROOT/build-vfio"
APP="$DERIVED/Build/Products/Debug/VFIOUserHostApp.app"

if [ -z "$VERSION" ]; then
    cur="$(/usr/libexec/PlistBuddy -c 'Print CFBundleVersion' \
        "$INSTALLED"/Contents/Library/SystemExtensions/*.dext/Info.plist 2>/dev/null || true)"
    VERSION=$(( ${cur:-0} + 1 ))
fi

cd "$ROOT"
echo "== Commit: $(git log --oneline -1)"
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "warning: tracked files have uncommitted changes" >&2
fi
echo "== Building version $VERSION"

xcodebuild -project "$E/VFIOUserHostApp.xcodeproj" -scheme VFIOUserHostApp \
    -configuration Debug -derivedDataPath "$DERIVED" \
    CODE_SIGNING_ALLOWED=NO CURRENT_PROJECT_VERSION="$VERSION" build \
    > "$ROOT/build-vfio-v$VERSION.log" 2>&1 || {
    echo "build failed; last lines of build-vfio-v$VERSION.log:" >&2
    tail -30 "$ROOT/build-vfio-v$VERSION.log" >&2
    exit 1
}

shopt -s nullglob
DEXTS=("$APP"/Contents/Library/SystemExtensions/*.dext)
[ ${#DEXTS[@]} -eq 1 ] || { echo "expected one dext in $APP" >&2; exit 1; }
DEXT="${DEXTS[0]}"
built="$(/usr/libexec/PlistBuddy -c 'Print CFBundleVersion' "$DEXT/Info.plist")"
[ "$built" = "$VERSION" ] || { echo "dext version is $built, expected $VERSION" >&2; exit 1; }

echo "== Signing (ad hoc)"
for f in "$APP"/Contents/MacOS/lib/*.dylib "$APP"/Contents/MacOS/*.dylib \
         "$APP"/Contents/Frameworks/*.dylib; do
    codesign --force --sign - "$f"
done
HVF_ENT="$ROOT/accel/hvf/entitlements.plist"
for f in "$APP"/Contents/MacOS/qemu-system-*; do
    if [ -f "$HVF_ENT" ]; then
        codesign --force --sign - --entitlements "$HVF_ENT" "$f"
    else
        codesign --force --sign - --preserve-metadata=entitlements "$f"
    fi
done
[ -f "$APP/Contents/MacOS/qemu-img" ] && codesign --force --sign - "$APP/Contents/MacOS/qemu-img"
[ -f "$APP/Contents/MacOS/qemu-vfio-apple" ] && codesign --force --sign - \
    --entitlements "$E/qemu-vfio-apple/qemu-vfio-apple.entitlements" "$APP/Contents/MacOS/qemu-vfio-apple"
codesign --force --sign - --entitlements "$E/VFIOUserPCIDriver/VFIOUserPCIDriver.entitlements" "$DEXT"
codesign --force --sign - --entitlements "$E/VFIOUserHostApp/VFIOUserHostApp.entitlements" "$APP"

echo "== Verifying"
codesign --verify --deep --strict "$APP" && echo VERIFY_OK
echo "dext version: $built"
codesign -d -vvv "$DEXT" 2>&1 | grep -E "^CDHash=" || true
for f in "$APP"/Contents/MacOS/qemu-system-*; do
    codesign -d --entitlements - "$f" 2>&1 | grep -q hypervisor \
        && echo "hypervisor entitlement: $(basename "$f") OK" \
        || echo "warning: $(basename "$f") lacks hypervisor entitlement" >&2
done
echo "built: $APP"

if [ "$INSTALL" -eq 1 ]; then
    if pgrep -fl qemu-system; then
        echo "qemu-system is running; not installing" >&2
        exit 1
    fi
    echo "== Installing"
    osascript -e 'quit app "VFIOUserHostApp"' || true
    sleep 2
    if [ -d "$INSTALLED" ]; then
        BACKUP="$HOME/VFIOUserHostApp-backup-$(date +%Y%m%dT%H%M%S).app"
        sudo mv "$INSTALLED" "$BACKUP"
        echo "previous app saved: $BACKUP"
    fi
    sudo ditto "$APP" "$INSTALLED"
    codesign --verify --deep --strict "$INSTALLED" && echo INSTALLED_VERIFY_OK
    open "$INSTALLED"
    echo "Now press Update Driver in the app, then check:"
    echo "  systemextensionsctl list | grep -i VFIOUserPCIDriver"
fi
