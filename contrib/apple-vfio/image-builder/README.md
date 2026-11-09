# Ubuntu Desktop Image Builder

This directory contains the first pass of the prebaked guest-image pipeline for
`apple-vfio`.

The goal is to produce a versioned Ubuntu Desktop `qcow2` artifact that can be
downloaded by a future launcher CLI and booted with the QEMU runtime already
bundled into `VFIOUserHostApp`.

## Current approach

The builder supports Ubuntu's official arm64 or amd64 **cloud image** as the
base and provisions it into a desktop image on first boot. `GUEST_ARCH`
defaults to the host architecture (`arm64` on Apple Silicon, `x86_64` on
Intel); set it explicitly to select another target.

The current default target is Ubuntu 26.04 LTS (`resolute`, Resolute Raccoon)
for arm64. The x86_64 target uses the matching amd64 cloud image,
`qemu-system-x86_64`, the `q35` machine, and x86_64 UEFI firmware.

**Intel passthrough is experimental and unverified.** Neither the Darwin
backend nor the guest DMA module has been validated end-to-end on an Intel
Mac with macOS 15.8. The guest module currently rejects coalesced or
non-identity original scatter-gather mappings, a direct SG backend without
DMA operations, and host mappings whose segments are not contiguous for a
single guest range. It also requires a kernel with per-device DMA operations.
These limitations can prevent passthrough workloads from working.
The x86 guest module now requires the explicit-ops DMA API extension in
`contrib/apple-dma/linux/apple-dma-explicit-ops-v7.0.patch`. It targets upstream
Linux v7.0 and applies against the Ubuntu 7.0.0-43.43 downstream DMA mapping
delta. The patched DMA-core object and DKMS module objects compile against that
source/config in a disposable guest. A full kernel build, patched
`Module.symvers` generation, and final DKMS `.ko` link have not been run.
The module deliberately fails to compile against x86 headers without this
extension. The extension lets x86 wrappers call the captured original DMA ops
without changing the device's shared `dma_ops` pointer. A mock checks that
delegation keeps the wrapper installed while calls use the original ops table.

The existing ARM64 path and behavior are preserved and still temporarily
replace a managed device's DMA operations while delegating. The interception
fix does not validate hook installation/removal synchronization, active
callback lifetime during teardown, guest mapping coverage, outstanding DMA
drain, or hardware behavior. Do not use physical guest DMA on either
architecture until those remaining lifecycle issues are resolved and
validated.

### macOS lifecycle diagnostics

When an extension is already active, the host app's **Update Driver** button
submits an activation request for its bundled extension without first
uninstalling the active one. Shut down VMs using the device and release all
claims before updating; the app cannot detect every external claim. The update
may require approval or a reboot, and the active version must be checked
separately. An update does not establish device quiescence or reset safety.

The host app logs extension requests and observed state changes under unified
logging subsystem `uk.co.hitwithwrench.vfio`, category `extension`. DriverKit
service and user-client lifecycle messages use the `vfio-user-dext:` prefix.
The `vfio-user-dext: lifecycle:` messages record whether Start retained the
PCI Open, the owner and blocked flags on service Stop, and whether the close
and superclass Stop were reached. Client Stop logs its claim, DMA and
interrupt cleanup state. These are point-in-time observations, not proof of
DMA quiescence. If no Stop entry is visible, the call may not have reached the
driver, but missing or filtered logs cannot prove that. The diagnostics are
available only while the instrumented dext is actually running; an older
service terminating during its upgrade cannot emit them.
To additionally log the start, PID and exit of each two-second status poll,
launch the signed app executable with `VFIO_TRACE_POLLING=1`:

```sh
VFIO_TRACE_POLLING=1 /Applications/VFIOUserHostApp.app/Contents/MacOS/VFIOUserHostApp
```

After an incident, export the relevant recent messages, adjusting the time
window as needed:

```sh
sudo log show --last 15m --info --style compact \
  --predicate 'subsystem == "uk.co.hitwithwrench.vfio" OR process == "kernelmanagerd" OR process == "sysextd" OR eventMessage CONTAINS[c] "vfio-user-dext"' \
  > ~/Desktop/vfio-lifecycle.log
```

Review the file for sensitive information before sharing. Logs can be lost
during a panic; retain the panic report as well. These diagnostics do not
identify which kernel code acquired a leaked lock, and neither an activated
extension nor a registered service proves exclusive access. `list-devices`
shows other direct services alongside a VFIO match; their presence does not
by itself identify the owner of the PCI open session.
For a PCI-tunnelled display device with recognizable AMD graphics services,
`list-devices` and the launcher suggest closing applications using the eGPU
and, if safe, using the menu bar eGPU disconnect action before checking the
device list again. This is advisory: service names do not prove the GPU is
accelerating the internal display or identify the exclusive-open owner.
Disconnecting may remove the GPU from the PCI registry altogether, rather
than leave it available to VFIO; reconnecting may cause macOS to bind it again.
It can interrupt applications or external displays and does not establish
isolation, exclusive access, or DMA safety.
The PCI-matching extension can be started by macOS without a QEMU claim and
its startup reads identity from PCI registry properties and queries BAR
metadata, not live configuration space. A normal claim opens the PCI device
and checks live vendor/device and command values against the registry before
enabling bus mastering. Opening and closing a PCI device can themselves alter
its state and do not establish device quiescence. The existing claim
operation enables bus mastering, which can allow device-initiated DMA even
without a guest. Do not run a guest using the physical device or attempt DMA
while the safety blocker remains.
The BAR descriptor request is made using the DriverKit PCI service that owns
the open session, and the user client retains a cached descriptor until Stop.
Mocks check descriptor reference accounting only. BAR0, BAR2 and BAR5 mapping
on the tested RX 5700 XT previously failed with `kIOReturnNotOpen`; this owner
correction has not been validated on hardware, and no MMIO fallback is proven
to work.

The host app also offers an explicit **Claim GPU** button. It selects a display
device by registry identity and, if no VFIO service is present, requests a PCI
rescan and waits up to five seconds for one. It attempts the rescan and claim
even if other services are attached. The rescan can affect other devices and
can fail to produce a VFIO match. If VFIO matches, the app requests a retained
PCI Open and enables memory-space decoding and bus mastering, verifying the
command bits after the write. This can let the device initiate DMA and corrupt
memory or data if it is not quiescent or properly isolated; the user must
explicitly confirm the warning. Open, Close, rescan, and device removal can
disrupt macOS and other devices. The app retains the user-client connection
until **Release GPU**, window teardown, or process exit; it cannot hand that
claim to QEMU directly. Release GPU synchronously asks the dext to reject
release while DMA resources remain, clear and verify the PCI Bus Master Enable
bit, and release the app's claim. A cleared bus-master bit does not prove that
already-issued DMA has drained. If any step fails, the app retains its
connection and reports the failure. The dext retains its Start-time PCI Open
after a successful app release so it continues to own the nub; a subsequent
VFIO user client can acquire the released claim. A successful claim does not
validate the guest DMA path or prevent forced DriverKit teardown.
QEMU requests the same explicit release after the VFIO owner and DMA companion
have dropped their shared connection references. Interrupt cancellation is
asynchronous, so QEMU retries a pending release briefly. On failure it reports
the error and retains the connection rather than knowingly closing it; this
does not guarantee safety if QEMU exits or DriverKit forces teardown. Do not
run physical-device guest DMA until the guest DMA interception race and the
complete mapping and shutdown lifecycle have been resolved and validated.
On normal process exit, QEMU also attempts release for remaining published
connections because QEMU does not necessarily unrealize devices on `quit`.
This does not unmap live DMA resources: the dext refuses the release if such
resources remain, and the process will still exit on failure. Abrupt
termination can bypass the exit handler. A successful command-register
readback does not establish that outstanding DMA transactions have drained.
The app's `vfio-gpu-claim:` messages and the dext's `vfio-user-dext:` messages
are recorded by the macOS unified log with timestamps. To capture them:

```sh
sudo log show --last 15m --info --style compact \
  --predicate 'eventMessage CONTAINS[c] "vfio-gpu-claim:" OR eventMessage CONTAINS[c] "vfio-user-dext:"' \
  > ~/Desktop/vfio-gpu-claim.log
```

For an explicitly chosen device, the standalone `scripts/probe-pci-config.c`
tool can request a one-shot live vendor/device/command read. It requires an
exact BDF and vendor/device match and refuses ambiguous matches. Compile with
`clang -std=c11 -Wall -Wextra -Werror -framework IOKit -framework CoreFoundation
contrib/apple-vfio/scripts/probe-pci-config.c -o /tmp/vfio-probe-pci` from the
repository root. Run `/tmp/vfio-probe-pci 44:00.0 1002:731f` only after
checking the current BDF and device ID. This is not a read-only operation:
DriverKit Open can map device memory, and Close can clear bus-master and memory
enable bits. The probe does not set those bits, but they might already be set.
It does not prove that the device is quiescent or that DMA is safe.

`scripts/probe-bar.c` is a host-only BAR check (no guest, no DMA). It claims the
device, reads BAR0/2/5 info, maps and unmaps the BAR2 and BAR5 descriptors
without touching their contents, then releases the claim. Compile it the same
way as the config probe and run `/tmp/vfio-probe-bar 44:00.0 1002:731f`. It
stops at the first failure, and claiming can change PCI state. It is unverified
on hardware.

For an explicitly selected PCI registry entry, `scripts/rescan-pci.c` offers
an opt-in PCI rescan request. It requires the exact BDF and vendor/device IDs,
rejects ambiguous BDFs, and may rescan even when other services are attached,
and does not open or claim the device. Compile it from the repository root:

```sh
clang -std=c11 -Wall -Wextra -Werror -framework IOKit -framework CoreFoundation \
  contrib/apple-vfio/scripts/rescan-pci.c -o /tmp/vfio-rescan-pci
```

For example, `/tmp/vfio-rescan-pci 44:00.0 1002:731f --confirm-rescan`
requests a scan on that entry. A scan can reconfigure PCI resources or affect
other devices, even though it targets a specific entry. It does not check
for quiescence. It may fail, remove the entry,
re-attach macOS drivers, or have no effect. A successful request does not
prove that VFIO matched or that PCI Open succeeded; inspect `list-devices`.
Do not use the normal claim or guest DMA to check the outcome.

The alternative catalogue-matching pathway is not exposed: Apple's published
kernel code requires a private catalogue-management entitlement, can report
success without performing the request when unauthorized, and marks the old
start-matching operation removed. Adding an already-present personality may
not trigger matching. This project cannot assume that root or ad-hoc signing
authorizes it.

The DMA companion must share the matching `vfio-apple-pci` connection; the
normal `dma-companion=on` configuration creates it automatically. Standalone
companions must be realized after their matching VFIO device, and a second
companion for the same host device is rejected. If the VFIO owner is removed
before a separately configured companion, new DMA maps are rejected; the
companion can still request unmaps of its existing mappings.

The DMA companion inhibits QEMU RAM discard while it is active, so configurations
that require RAM discard cannot be used with it. This does not synchronize
concurrent discards such as postcopy migration. If DMA teardown fails, QEMU
retains the connection and RAM mappings until process exit; that is not a
guarantee that the device is quiescent or that shutdown is safe. Unplug,
process exit, and DriverKit shutdown remain unverified.
If the DriverKit registration reply is lost or malformed, mapping ownership
is unknown. The companion retains the backing and refuses to unregister a
potentially unrelated mapping; the guest treats the failed unmap as fatal.
The dext allows unclaimed clients to query device identity, but PCI access
and DMA operations require an exclusive claim. Claims are reserved atomically
because DriverKit's default dispatch queue belongs to each `IOUserClient`:
see Apple's [IOService][driverkit-service] and [IOUserClient][driverkit-client]
declarations. If DMA completion fails during client shutdown, later claims
are blocked and the user-client stop is refused with its claim and DMA backing
retained for cleanup retry. This deliberately leaves the device unavailable
if retries continue to fail; it does not guarantee that DriverKit or the
operating system will preserve the service through forced termination. Device
quiescence and shutdown safety remain unverified.
Interrupt-source cancellation is asynchronous. The client stop retains its
state and returns `kIOReturnStillOpen` until cancellation completes. A later
Stop invocation is required to finish cleanup; automatic retry behavior is not
verified, and DriverKit may still force teardown.
QEMU reports each backend reset request and result. The dext logs the
function reset attempt and, if that fails, the hot-reset fallback and their
DriverKit return codes as `vfio-user-dext: reset:`. A successful return does
not prove the GPU is ready for another guest driver initialization; a hot
reset can also affect devices on the same downstream bus.
The connection, claim, BAR isolation, guest protocol, mapping, response and teardown mock
checks run from the repository root with
`python3 contrib/apple-dma/tests/run_tests.py`. They require C and C++
compilers and do not replace macOS builds or hardware testing.

[driverkit-service]: https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/iokit/DriverKit/IOService.iig
[driverkit-client]: https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/iokit/DriverKit/IOUserClient.iig

That is a deliberate compromise:

- it matches the existing repo workflow in `scripts/make-geekbench-vm.sh`
- it is straightforward to automate under QEMU/HVF on macOS
- it preserves a UEFI/GPT bootable image layout
- it gives us a reproducible starting point for desktop customization

The long-term product can still switch to a Desktop-ISO-derived image if we
decide that provenance matters more than automation simplicity. For now, the
priority is to start generating a working prebaked image.

## What the builder does

`build-ubuntu-desktop-image.sh`:

1. downloads the official Ubuntu cloud image for `GUEST_ARCH`
2. creates a writable build disk on top of it
3. boots that disk under QEMU/HVF with a `cloud-init` seed
4. installs `ubuntu-desktop` and desktop-adjacent runtime packages
5. optionally enables extra provisioning hooks for FEX / Steam / NVIDIA /
   `apple-dma`
6. powers the guest off when provisioning completes
7. flattens the resulting disk into a compressed release-ready `qcow2`
8. writes a small JSON manifest next to the artifact

## Current scope

The initial provisioning flow is intentionally conservative:

- `ubuntu-desktop` is installed and configured as the default target
- `qemu-guest-agent` and a few desktop-friendly packages are added
- image cleanup is performed so the result behaves like a reusable golden image
- On the default arm64 target, FEX is wired by default and ships with:
  - the official Ubuntu 24.04 RootFS extracted into a directory tree
  - a minimal `Config.json` enabling only the GL and Vulkan thunks
  - per-app overrides for the Steam `client` and `steamwebhelper`
- On arm64, NVIDIA is wired by default: the latest `nvidia-driver-XXX(-open)` package is
  installed in the guest, and the matching x86_64 `.run` file is unpacked into
  the FEX RootFS so DLSS / NGX work for x86 binaries (Proton, etc.)
- `apple-dma` is wired by default: the kernel module sources from
  `contrib/apple-dma/linux/` are bundled into the cloud-init seed and
  registered with DKMS in the guest so the module gets rebuilt automatically
  on every kernel upgrade, with the upstream `modprobe.d` / `modules-load.d`
  configs installed for autoload at boot
- Steam is installed by default from Valve's official `steam_latest.deb`
  (`dpkg -i --force-all`) — the launcher runs natively on arm64 and FEX-Emu
  transparently emulates the x86_64 Steam bootstrap + steamwebhelper using
  the Ubuntu 24.04 FEX RootFS. On x86_64, FEX and the arm64-specific NVIDIA
  setup default to disabled.

This lets us start producing a real desktop artifact before we lock down the
rest of the guest stack.

## Requirements

- built QEMU binaries at either `dist/` or `build/`
- architecture-matched firmware blobs in `build/pc-bios/`
- `python3`
- `curl`
- `hdiutil` (macOS)
- network access for Ubuntu packages during provisioning
- a QEMU build with `slirp` / `-netdev user` support for guest networking during the build

## Usage

From the repository root, the default build matches the host architecture:

```bash
./contrib/apple-vfio/image-builder/build-ubuntu-desktop-image.sh
```

Build an x86_64 guest image with Intel firmware and QEMU:

```bash
GUEST_ARCH=x86_64 \
./contrib/apple-vfio/image-builder/build-ubuntu-desktop-image.sh
```

On an Intel Mac, the launcher defaults to x86_64; `--arch x86_64` selects
the x86_64 image tag, `qemu-system-x86_64`, `q35`, and x86 UEFI files. The
build/edit/seal helpers require `GUEST_ARCH` to match the host because these
flows use HVF rather than cross-architecture emulation. The published QEMU
dist must contain the host target binary and QEMU ROM data before the app can
embed it. Re-run `scripts/make-dist.sh` after updating older distributions
that lack x86 BIOS and VGA ROMs.

The output directory is architecture-specific by default:
`out/ubuntu-desktop-image` for arm64 and
`out/ubuntu-desktop-image-x86_64` for x86_64. The builder does not overwrite
an existing build disk, artifact, or manifest. Choose a different output
directory for a fresh build.

If provisioning completed and the guest powered down but the artifact is
missing, rerun the same builder command with the same `GUEST_ARCH`,
`IMAGE_NAME`, output directory, and `QEMU_IMG_BIN`. It detects the build disk,
checks the log for both the completion marker and `reboot: Power down`, checks
the qcow2 with `qemu-img`, and converts it without booting the guest again.
Make sure the VM is stopped first. Recovery does not create a manifest because
the original build settings cannot be verified; it will refuse to overwrite an
existing image or manifest. If the shutdown marker or disk check is missing or
fails, recovery stops without modifying the build disk.

To override defaults:

```bash
IMAGE_NAME=ubuntu-desktop-resolute-arm64 \
INSTALL_FEX=1 \
INSTALL_STEAM=1 \
INSTALL_NVIDIA=1 \
INSTALL_APPLE_DMA=1 \
./contrib/apple-vfio/image-builder/build-ubuntu-desktop-image.sh \
  contrib/apple-vfio/image-builder/out/ubuntu-desktop-image
```

Useful knobs:

- `IMAGE_NAME`: artifact basename
- `GUEST_ARCH`: `arm64` or `x86_64` (defaults to host architecture); selects the QEMU binary,
  machine type, cloud image, and UEFI firmware
- `DISK_SIZE`: writable build disk size, default `96G`
- `CPUS`: builder vCPU count, default `8`
- `MEMORY`: builder RAM, default `12G`
- `BUILD_USER`: guest username, default `ubuntu`
- `BUILD_PASSWORD`: plaintext password set for `BUILD_USER` inside the guest
  during provisioning, default `ubuntu`
- `QEMU_DATA_DIR`: override the QEMU firmware/data directory passed via `-L`
- `EFI_CODE_SRC`: override path to the architecture-appropriate UEFI code file
- `EFI_VARS_SRC`: override path to the architecture-appropriate UEFI vars file
- `QEMU_DISPLAY`: display option for `edit-image.sh` (default
  `cocoa,full-grab=on`). Try `cocoa,full-grab=on,show-cursor=on` if the mouse
  pointer is invisible; this forces the host cursor and may show two pointers.
- by default the builder now also checks `build/qemu-bundle/usr/local/share/qemu/`
  for those firmware blobs
- `INSTALL_FEX`: `1` by default on arm64, `0` on x86_64
- `INSTALL_STEAM`: `1` by default
- `INSTALL_NVIDIA`: `1` by default on arm64, `0` on x86_64
- `INSTALL_APPLE_DMA`: `1` by default
- `APPLE_DMA_SRC_DIR`: source dir tarred into the guest, default
  `contrib/apple-dma/linux`
- `APPLE_DMA_VERSION`: DKMS package version, default `0.1.0`

## Output

The output directory contains:

- `cache/`: downloaded base image
- `work/`: transient build artifacts and logs
- `artifacts/<image-name>.qcow2`: the flattened prebaked image
- `artifacts/<image-name>.manifest.json`: image metadata for the launcher

## Editing / re-sealing the finished image

Some first-run setup is GUI-only and not worth scripting — signing into
Steam the first time, right-clicking the desktop launcher and choosing
"Allow Launching", accepting a GNOME prompt, etc. Two helper scripts
let you do that manually and then turn the result back into a reusable
golden image:

```bash
GUEST_ARCH=x86_64 \
./contrib/apple-vfio/image-builder/edit-image.sh
# click / type whatever you need; shut down from inside the VM.
GUEST_ARCH=x86_64 \
./contrib/apple-vfio/image-builder/seal-image.sh
```

What each step does:

- `edit-image.sh`
  - One-time copy of `artifacts/<name>.qcow2` into
    `work/<name>.edit.qcow2` (re-run reuses the same scratch disk so
    iterative editing is cheap)
  - Boots the scratch disk under QEMU/HVF with a cocoa window,
    virtio-gpu, USB keyboard/mouse/tablet, and ssh forwarded to
    `127.0.0.1:2233` for convenience
  - Leaves the released artifact untouched — if an edit session goes
    wrong, delete `work/<name>.edit.qcow2` and start over
- `seal-image.sh`
  - Boots the same scratch disk headless
  - SSH'es in (password auth driven by `expect`, so no `sshpass`
    dependency) and runs `guest/seal-desktop.sh`, which wipes
    machine-specific state (ssh host keys, `/etc/machine-id`,
    cloud-init state, DHCP/NM leases, systemd random seed),
    clears caches (apt, journal, per-user `~/.cache` / history),
    runs `fstrim -av`, and issues `systemctl poweroff`
  - Waits for the guest to power off cleanly
  - `qemu-img convert -c -O qcow2` recompresses the scratch disk
    back into `artifacts/<name>.qcow2`, overwriting the original
  - Refreshes `artifacts/<name>.manifest.json` with the new
    sha256 / size_bytes and sets `"sealed": true`

The scratch disk is kept in `work/` after sealing, so the next
`edit-image.sh` run resumes from the sealed artifact unless you
delete it.

User-visible state that is deliberately preserved (edits you made
are the whole point of the workflow): Steam installs / logins, game
libraries, desktop launcher trust flags, GNOME/dconf tweaks, and
anything else under `/home/<BUILD_USER>` that isn't a cache or shell
history. `~/.cache/fex-emu/` is also preserved — it holds FEX's AOT
/ object cache, and keeping it means that if you launch Steam once
during an edit session (just enough to warm the JIT, then quit), the
sealed image ships with the warm cache and subsequent cold Steam
launches drop from ~60s to ~10-15s. If you want a strictly anonymous
snapshot, manually delete `~/.local/share/Steam` (or whatever else)
inside the edit session before you seal.

Useful knobs (on top of the ones documented above):

- `EDIT_SSH_PORT`: host port forwarded to guest sshd, default `2233`
- `SEAL_TIMEOUT`: seconds to wait for sshd-up and guest poweroff,
  default `600`
- `BUILD_USER` / `BUILD_PASSWORD`: must match whatever was set during
  the original build (defaults to `ubuntu` / `ubuntu`)

## Next steps

The next rounds of work should focus on:

- teaching the future launcher how to download and cache the published artifact
