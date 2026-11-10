.. SPDX-License-Identifier: GPL-2.0-or-later
.. _vfio-apple:

======================
vfio-apple (macOS/ARM)
======================

QEMU supports PCI device passthrough on Apple Silicon Macs using a macOS
DriverKit extension (dext) as the host backend.  Unlike Linux VFIO, which
relies on kernel-managed IOMMU groups and ``/dev/vfio``, the Apple backend
communicates with a userspace driver extension through IOKit's
``IOUserClient`` interface.

Requirements
============

- Apple Silicon Mac running macOS
- A DriverKit extension (``VFIOUserPCIDriver``) installed and running for
  the target PCI device
- QEMU built with ``--enable-hvf`` on a Darwin host
- A guest kernel module that speaks the ``apple-dma-pci`` protocol (see
  below)

Usage
=====

Specify the host PCI device by its bus/device/function address:

.. code-block:: console

  -device vfio-apple-pci,host=01:00.0

This creates a ``vfio-apple-pci`` device that connects to the dext instance
managing the given host PCI BDF.  A companion ``apple-dma-pci`` device is
automatically created on the same bus.

Architecture
============

The implementation consists of several components:

``vfio-apple-pci``
  A QOM subclass of ``vfio-pci`` that overrides realize/exit to set up the
  Apple-specific IOMMU container and interrupt delivery.

``vfio-iommu-apple``
  An IOMMU container backend.  DMA map/unmap through the container are
  no-ops because DMA is managed separately through the companion device
  (see below).

``apple-dma-pci``
  A paravirtualized PCI device that provides batched DMA map/unmap
  operations between the guest and the dext.  The guest driver writes a
  command page GPA to BAR0 registers, fills request/response buffers in
  RAM, then triggers processing with a single doorbell write (one VMEXIT
  per batch).

``Dext communication``
  All host device access (config space, BAR MMIO, DMA registration,
  interrupt notification) goes through IOKit ``IOConnectCallMethod`` calls to
  the dext's ``IOUserClient``.

Guest BAR addresses and the PCI I/O and memory decode bits are virtualized.
The host retains its physical BARs and decode settings when the guest
disables decoding to reassign resources. Other command bits, including bus
mastering, are still forwarded; this is not a DMA readiness gate.

DMA mapping
-----------

On Linux, VFIO programs the hardware IOMMU directly via kernel ioctls.
QEMU chooses IOVAs and the kernel maps them through the IOMMU.

On macOS, the DART (Apple's IOMMU) is managed entirely by the DriverKit
extension.  QEMU cannot choose IOVAs — it can only request that a host
virtual address be mapped for DMA, and the platform assigns the resulting
IOVA.  This means the guest cannot assume any particular IOVA layout;
the ``apple-dma-pci`` companion device returns the platform-assigned IOVA
and bus address in its response entries.

Because of this architecture, a **guest kernel module** is required to
drive the ``apple-dma-pci`` device.  The module discovers the companion
device, submits map/unmap batches, and translates between guest physical
addresses and the platform-assigned DMA addresses that the passthrough
device will use.

Stock-Linux pool feasibility preflight
--------------------------------------

The existing ``dma-bounce-size`` path is an experimental host-allocated pool,
not a validated stock-Linux DMA path. Its restricted-pool device-tree wiring
is specific to the ARM ``virt`` machine; it does not attach a pool to a PCI
device on an x86 ACPI guest. The app currently enables the companion instead.
The experimental allocator accepts ``dma-bounce-alignment`` (a power of two,
at least 4096 bytes; default 4096) to request a suitably aligned DMA address.
For example, ``dma-bounce-alignment=128M`` requests the 128 MiB alignment that
Linux x86 memory hotplug reported as necessary in the probe below. This
property does not add x86 ACPI memory-device or DMA-pool attachment support.
The dext client rejects malformed allocation replies, overflowing first DMA
segments, and CPU mappings that are too short or whose requested range
overflows the host pointer width. It attempts to complete the allocation if
mapping fails. A reported cleanup failure means the device's shutdown safety
remains unverified; it is not evidence that the DMA mapping was released.
Neither path provides a proven physical bus-master readiness or device-DMA
drain gate. Do not enable physical guest DMA to evaluate this experiment.
In Linux v7.0, ``acpi_arch_dma_setup()`` is an empty inline function on x86;
the ARM64 ACPI ``_DMA`` setup therefore cannot be assumed to provide an
address-offset translation on x86.

Linux v7.0 configures PCI DMA via the host bridge's OF or ACPI path. Its OF
path can attach a ``restricted-dma-pool``; the ACPI configuration path does
not perform that attachment. Restricted SWIOTLB pools enable both forced
bouncing and allocation from the pool. In contrast, the default pool's
``swiotlb=force`` setting alone does not set the restricted pool's
``for_alloc`` behavior, so it cannot establish that coherent allocations
also use host-mapped pages. No stock x86 ACPI equivalent is established by
these probes. Moreover, in Linux v7.0 ``arch/x86/Kconfig`` the only
``select OF`` entries are the 32-bit ``X86_INTEL_CE`` and ``OLPC`` platforms
(``X86_64`` is not among them), so a stock x86-64 kernel is expected to lack
the OF code that restricted pools depend on. Distribution configuration
should be confirmed with ``CONFIG_OF`` and ``CONFIG_DMA_RESTRICTED_POOL``
in the guest's kernel config. Relevant Linux v7.0 sources are
`PCI DMA configuration <https://github.com/torvalds/linux/blob/v7.0/drivers/pci/pci-driver.c>`_,
`OF DMA configuration <https://github.com/torvalds/linux/blob/v7.0/drivers/of/device.c>`_,
`SWIOTLB <https://github.com/torvalds/linux/blob/v7.0/kernel/dma/swiotlb.c>`_,
and `direct DMA allocation <https://github.com/torvalds/linux/blob/v7.0/kernel/dma/direct.c>`_.

The dext's current buffer allocation requires a PCI claim, and the claim
enables bus mastering. Successful allocations log the returned flags and
every segment's address, length and offset, grouped by sample ID in the dext
logs; these addresses may be sensitive. The first segment alone is not proof
that a multi-segment mapping is contiguous. Clearing the enable bit later is
not proof that outstanding transactions have drained. A host allocation
experiment therefore requires a separately established device-quiescence and
mapping-lifetime protocol; the synthetic probes below do not provide one.

For a host-only mapping observation, first shut down any VM using the device,
release any claim held in the host app, and ensure the GPU is idle. Build and run
``contrib/apple-vfio/scripts/probe-dma-allocation.c`` from the repository root
on the Mac. It claims the selected device, asks the dext to prepare a 256 MiB
DMA buffer, then completes and frees it and releases the claim. It does not
start a VM or issue device work, but claiming enables bus mastering. The probe
is not a physical-DMA test and does not establish that the device is safe to
use for DMA.

Capture dext logs in one terminal before running the probe in another::

  sudo log stream --style compact --level info \
    --predicate 'eventMessage CONTAINS[c] "vfio-user-dext: DMA buffer"'

Build and run the probe from the repository root in the second terminal::

  clang -Wall -Wextra -Werror \
    -framework IOKit -framework CoreFoundation \
    -o /tmp/probe-dma-allocation \
    contrib/apple-vfio/scripts/probe-dma-allocation.c
  /tmp/probe-dma-allocation BUS:DEVICE.FUNCTION VENDOR:DEVICE [ALIGNMENT]

Replace the last arguments with the GPU's exact BDF and hexadecimal PCI ID
(for example, ``02:00.0 1002:731f``). The optional alignment is a power of two
in bytes, defaulting to 4096. Stop the log stream with Control-C after
the probe exits. The probe output reports the allocation result and segment
count; the dext log reports each segment's address, length and offset grouped
by sample ID. Treat those addresses as sensitive. A passing run demonstrates
only that DriverKit prepared and then completed that host mapping; it does not
prove guest placement, Linux pool attachment, or physical DMA correctness.

With the device idle and no VM using it, the optional CPU alias check maps the
prepared DriverKit buffer, uses ``mach_vm_remap`` without copying to request
a second mapping, tests writes in both directions at three offsets, unmaps,
completes DMA and releases the claim::

  /tmp/probe-dma-allocation BUS:DEVICE.FUNCTION VENDOR:DEVICE 0x8000000 --cpu-alias

The two CPU mappings must be distinct and cover the requested allocation.
The remap may be rejected by the host; report that as a failed probe, not as
evidence of a DMA failure.
This tests only CPU visibility between two host mappings. It neither maps the
buffer into QEMU nor proves device coherency, guest visibility or safety. A
failed completion or unmap requires investigation; do not assume the DMA
mapping was safely released.

To investigate guest RAM visibility without a physical device, boot a
**separate disposable** stock x86 Linux VM without any Apple VFIO device or
companion. In addition to its normal boot and disk arguments, configure a
synthetic 256 MiB memory device and a local QMP socket in a private directory.
Set a restrictive process umask (for example ``umask 077``) before starting
QEMU; QMP grants control of the VM::

  -m 2G,slots=1,maxmem=2304M
  -object memory-backend-ram,id=poolprobe,size=256M
  -device pc-dimm,id=poolprobe,memdev=poolprobe
  -qmp unix:pool-probe-qmp.sock,server=on,wait=off

Connect a QMP client to the socket and, after receiving its greeting, send
these messages in sequence, waiting for each response::

  {"execute":"qmp_capabilities"}
  {"execute":"query-memory-devices"}

Record ``data.addr`` and ``data.size`` from the entry whose ``data.id`` is
``poolprobe``. Copy
``contrib/apple-vfio/scripts/dma_pool_guest_probe.py`` into the guest and run::

  sudo python3 dma_pool_guest_probe.py <addr> <size>

The arguments can be decimal or prefixed with ``0x``. The probe checks that a
single unredacted ``System RAM`` entry in ``/proc/iomem`` covers the range
and reads every page's flags from ``/proc/kpageflags``. A nonzero exit means
the range was not covered, contains missing/offline pages, or the probe could
not gather its evidence (including when ``CONFIG_PROC_PAGE_MONITOR`` or
permissions are missing). Keep its JSON and errors with the QMP result. These
are ordinary guest RAM pages that Linux may use; the probe does not reserve
them, establish a linear mapping, or attach a DMA pool. A passing result is
only a firmware/page-metadata observation. In particular, this synthetic RAM
is not backed by a DriverKit DMA mapping or a host-selected device address.

To test whether QEMU accepts a particular *synthetic* guest address in the
same x86 memory sizes, run on the host from the repository root::

  python3 contrib/apple-vfio/scripts/dma_pool_placement_probe.py \
    --qemu "$PWD/dist/qemu-system-x86_64" \
    --firmware "$PWD/dist/share/qemu" --address 0x100000000

The tool starts a paused TCG VM with no guest disk or passthrough device,
requests a 256 MiB ``pc-dimm`` at that address, checks the QMP-reported
address and size, and exits. A launch error includes QEMU stderr; it must
not automatically be interpreted as an address rejection because other
startup failures are possible. TCG uses a different CPU model and accelerator
from an HVF guest, so the resulting address-space layout can differ. This
does not boot Linux or test DriverKit.
QEMU's x86 machine restricts ``pc-dimm`` placement to its device-memory
window and applies additional alignment and capacity checks; an arbitrary
host-selected DMA address is not necessarily usable as a DIMM address.

Live DriverKit-to-QEMU alias diagnostic
---------------------------------------

The Darwin-only ``memory-backend-apple-dma`` backend and
``contrib/apple-vfio/scripts/dma_pool_alias_probe.py`` test the next boundary:
the backend allocates and maps a DriverKit DMA buffer, reports that live
allocation's device address, then the script asks QEMU to hot-add a ``pc-dimm``
at exactly that address. It uses a paused TCG machine with no disk and no
physical PCI device passed to the guest. Qtest writes through QEMU's guest
physical address space; QMP then checks the same bytes through the backend's
host mapping, and the reverse direction is checked as well.

The backend retains each allocation until process exit, after VM shutdown and
an RCU callback drain. It rejects non-single-segment allocations,
misaligned or overflowing address ranges, incomplete CPU mappings, and
unsupported guest-memfd or preallocation settings. It also blocks migration
while the host-backed RAM exists. A failed cleanup is reported and the result
is not considered a clean test.

This diagnostic still claims the selected host PCI device; DriverKit claim
enables bus mastering. Run it only when the device is idle and not claimed by
another process. The probe performs no GPU work and does not add the physical
PCI device to the VM, but it does not prove that outstanding device DMA has
drained or that physical DMA is safe. The VM remains paused throughout; a
passing result proves only that QEMU's guest-physical address space aliases
the host mapping. It does not test guest CPU/Linux access, guest firmware RAM
description, restricted-pool attachment, GPU coherency, or DMA safety.

Rebuild the existing Darwin x86_64 QEMU build with VFIO support, refresh
``dist/``, then run from the repository root (the example uses the previously
observed BDF and alignment)::

  ninja -C build
  ./contrib/apple-vfio/scripts/make-dist.sh

  python3 contrib/apple-vfio/scripts/dma_pool_alias_probe.py \
    --qemu "$PWD/dist/qemu-system-x86_64" \
    --firmware "$PWD/dist/share/qemu" \
    --bdf 44:00.0 --size 0x10000000 --alignment 0x8000000 \
    --confirm-claim

If the BDF is ambiguous under multiple host PCI roots, add
``--host-root ROOT_NAME``. The binary must include Qtest support. No guest
disk is used, so no qcow2 overlay is needed for this paused diagnostic.

HVF synthetic hotplug diagnostic
--------------------------------

The DriverKit-backed NVDIMM hotplug experiment aborted in ``hv_vm_map`` on
the tested Intel Mac. The cause is not yet known, and an abort may bypass
normal allocation and claim cleanup. Do not run the previously published
NVDIMM guest-CPU procedure: the DAX namespace identity, resource offset and
mapping alignment also remain unverified. In particular, do not create or
modify an unverified NVDIMM namespace, or write to ordinary System RAM.

To check the same hotplug placement with ordinary QEMU RAM and HVF, without
claiming a device, allocating DriverKit memory, attaching a disk or running
the guest CPU, use::

  python3 contrib/apple-vfio/scripts/dma_pool_hvf_baseline.py \
    --qemu "$PWD/dist/qemu-system-x86_64" \
    --firmware "$PWD/dist/share/qemu" --address 0x128000000

The address is the one observed in the failed live test; it is not guaranteed
to be chosen for any future DriverKit allocation. The script starts a paused
q35 machine, hot-adds a 256 MiB NVDIMM backed by ordinary QEMU RAM, and
reports QMP placement and HVF mapping trace output. It does not validate
DriverKit memory, guest CPU access, Linux DMA-pool attachment, device DMA,
coherency or teardown safety. The previous abort occurred in a running VM;
this paused baseline also omits its disks and peripheral devices, so a pass
alone does not isolate the cause. Use ``--device pc-dimm`` for a comparison.
The trace can include host virtual addresses; review it before sharing.

To compare the same paused HVF/NVDIMM setup with a live DriverKit allocation,
replace ``--address`` with ``--bdf 44:00.0 --confirm-claim``. The script reads
the current allocation address from QMP rather than assuming it is stable.
This claims the host PCI device and enables bus mastering: run only while the
device is idle and not claimed elsewhere. HVF may abort again; an abort can
bypass QEMU cleanup and does not prove outstanding device DMA has drained.
This comparison does not boot the guest or validate Linux or physical DMA.

On the tested Intel Mac, the synthetic NVDIMM mapped successfully with RWX
permissions at ``0x128000000``, but the live DriverKit-backed NVDIMM failed
at the same address, length and permissions. In the direct host probe below,
ordinary memory mapped successfully as both RW and RWX; the DriverKit mapping
succeeded as RW and failed as RWX. This identifies a permissions-dependent
difference, not the underlying host restriction or a complete solution.
The host-only ``probe-dma-hvf.c`` compares RW and RWX ``hv_vm_map`` requests
on the same DriverKit mapping and an ordinary anonymous-memory control. It
creates an HVF VM but no vCPU, does not boot a guest, and does not issue GPU
work. It claims the GPU and enables bus mastering; an idle device and
exclusive claim are required. The HVF calls and their cleanup may still fail;
neither an API success nor a disabled bus-master bit proves DMA safety.
Build and sign the probe on an Intel Mac from the repository root::

  clang -Wall -Wextra -Werror \
    -framework Hypervisor -framework IOKit -framework CoreFoundation \
    -o /tmp/probe-dma-hvf contrib/apple-vfio/scripts/probe-dma-hvf.c
  codesign --force --sign - --entitlements accel/hvf/entitlements.plist \
    /tmp/probe-dma-hvf
  /tmp/probe-dma-hvf 44:00.0 1002:731f

The probe reports return codes for each mapping and unmapping attempt and
then destroys the HVF VM before releasing the CPU mapping, DMA buffer and
claim. It requires both ordinary-memory controls and the DriverKit RW map to
succeed for a complete result; an RWX failure is reported rather than hidden.
The output can contain host virtual addresses. Review it before sharing.

For a further paused QEMU experiment, build and distribute the current QEMU
binary and run::

  python3 contrib/apple-vfio/scripts/dma_pool_hvf_baseline.py \
    --qemu "$PWD/dist/qemu-system-x86_64" \
    --firmware "$PWD/dist/share/qemu" \
    --bdf 44:00.0 --confirm-claim --diagnostic-ram-device

The opt-in backend property uses QEMU RAM-device semantics, making HVF request
RW rather than RWX. The runner requires an RW mapping trace for the exact
allocation address and length. It starts no vCPU and claims the device, with
the same bus-mastering and abort/cleanup risks described above. RAM-device
semantics differ from ordinary RAM, and this non-executable mapping must not
be used as guest System RAM or as a supported Linux DMA pool. A pass establishes
neither guest CPU access nor device DMA, coherency, or safe teardown.

On the tested Intel Mac this paused diagnostic reported an RW mapping for the
entire 256 MiB DriverKit allocation at ``0x128000000`` and shut down without
a reported cleanup error. This does not establish that device DMA has drained.
The guest address is not guaranteed to remain the same on later allocations.

The RW-only mapping is not yet a general HVF RAM solution. For example, HVF's
dirty-logging protection and write-fault paths request executable permissions
without accounting for RAM-device regions. The migration blocker prevents
migration while the allocation exists, but other logging users and execution
semantics have not been established. A controlled guest CPU test must avoid
normal System RAM and verify both directions independently before any Linux
pool or physical-device experiment.

Bare-metal guest CPU alias diagnostic
-----------------------------------

``dma_pool_cpu_probe.py`` runs a single bare-metal x86 CPU from a temporary
minimal firmware, not Linux. It hot-adds a dedicated NVDIMM, maps its first
2 MiB through guest page tables, and tests eight bytes at offset zero. The
guest writes one pattern; QEMU is paused before the host checks it and writes
a different pattern. The guest then reads the host pattern and records it in
a separate low-RAM mailbox. For the live allocation, QEMU's backend property
accesses the host mapping; for the synthetic control, Qtest accesses ordinary
QEMU memory. The runner requires the exact RW-only HVF trace before starting
the live guest. A host timeout, incorrect value, missing trace, or QEMU
failure is not a pass.

The scratch qcow2 overlay is persistent and attached read-only. Neither its
backing file nor the overlay is intended to contain a guest OS or useful data.
The directory creation below deliberately fails rather than overwrite an
existing directory. Run from the repository root on the Intel Mac::

  mkdir "$HOME/vfio-dma-cpu-probe" &&
  ./dist/qemu-img create -f qcow2 "$HOME/vfio-dma-cpu-probe/base.qcow2" 1M &&
  ./dist/qemu-img create -f qcow2 -F qcow2 \
    -b "$HOME/vfio-dma-cpu-probe/base.qcow2" \
    "$HOME/vfio-dma-cpu-probe/overlay.qcow2" &&
  python3 contrib/apple-vfio/scripts/dma_pool_cpu_probe.py \
    --qemu "$PWD/dist/qemu-system-x86_64" --firmware "$PWD/dist/share/qemu" \
    --overlay "$HOME/vfio-dma-cpu-probe/overlay.qcow2" \
    --address 0x128000000

The synthetic control claims no device. If it succeeds, the same overlay can
be reused for the live test by replacing ``--address 0x128000000`` with
``--bdf 44:00.0 --confirm-claim``. The live test uses the current allocation
address, claims the GPU and enables bus mastering; use an idle, unclaimed
device. An HVF abort or forced termination may bypass normal QEMU cleanup.
This tests only guest CPU loads/stores through the experimental RAM-device
mapping, not Linux, physical GPU DMA, coherency with the GPU, quiescence, or
general teardown safety. Do not execute code from this buffer or use it as
System RAM. The emitted trace may include host virtual addresses.

On the tested Intel Mac, the synthetic control (RWX ordinary RAM) and the live
DriverKit run (RW-only, 256 MiB at ``0x128000000``) both reported guest-write
and host-write visibility with no reported cleanup error. This shows that a
guest CPU can load and store through the RW-only HVF mapping of the DriverKit
allocation and alias the host mapping, for one eight-byte location at offset
zero. It does not show Linux access, executable use, DMA-pool attachment,
device DMA, coherency with the GPU, quiescence, or general teardown safety.

The standalone ``contrib/apple-vfio/scripts/dma_pool_preflight.py`` checks
necessary address-layout conditions using recorded or synthetic metadata; it
does not allocate memory, open a device, start a VM, or validate DMA. It
accepts a JSON file (or ``-`` for stdin) with unsigned integer byte addresses,
lengths and offsets. For example, this is synthetic data, not a macOS result::

  {
    "size": 8192,
    "cpu_mapping_size": 8192,
    "page_size": 4096,
    "dma_address_bits": 44,
    "guest_physical_bits": 40,
    "device_read": true,
    "device_write": true,
    "segments": [
      {"address": 4294967296, "length": 8192, "offset": 0}
    ],
    "occupied_guest_ranges": [{"start": 0, "size": 4294967296}]
  }

For a real candidate, supply the returned segment list and permissions from
the same prepared host mapping, the size of its CPU mapping, guest page size,
physical address width, and a complete set of occupied guest physical ranges
including RAM, PCI apertures, firmware and reserved areas. To check Linux
memory-hotplug alignment as well, provide ``memory_block_size`` in bytes,
using the target guest's ``/sys/devices/system/memory/block_size_bytes``
(which is hexadecimal). For example, ``8000000`` there corresponds to the
JSON number ``134217728``. Both the DMA address and allocation size must be
multiples of that block size. If omitted, hotplug alignment is listed as
unverified. These recorded values must be rechecked against the current live
allocation and the actual guest; a previous host address is not a reservation.
``candidate``
means only that the supplied values pass these necessary checks; it does not
prove that the range list is complete, firmware can expose the region as RAM,
Linux can attach it to the target device, or the physical device can safely
use it. Exit status 1 reports a rejected layout and 2 invalid input.

The app's diagnostic export includes guest ``/proc/iomem`` (via ``sudo -n``),
memory information, CPU information, selected kernel configuration, and
default SWIOTLB debugfs counters when available. These are diagnostic
evidence, not an exhaustive QEMU guest address-space map or proof of a
per-device restricted pool.

Platform constraints
--------------------

The macOS DART imposes limits that do not exist with Linux VFIO:

- **No IOVA alignment guarantees**: the platform may return any address.
  The guest companion must reject mappings that do not meet the Linux DMA
  API's alignment requirements; coherent allocations now reject misaligned
  replacement handles. Streaming mappings reject a replacement address
  whose guest-page offset differs from the original GPA. GPU-page alignment
  for guest page sizes and devices still requires further validation.
- **DMA address masks**: streaming mappings use the device's streaming mask;
  DMA allocations use its coherent mask. These are currently checked after
  host mapping and are not yet requested from DriverKit.
- **Total DMA memory limit**: approximately 1.5 GB of guest memory can be
  registered for DMA at any time.
- **Mapping count limit**: approximately 64k concurrent DMA mappings.

These limits are enforced by the DART hardware and DriverKit, not by QEMU.
Exceeding them will cause map requests to fail.

``apple-dma-pci`` register interface
-------------------------------------

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Access
     - Description
   * - 0x00
     - VERSION
     - R
     - Protocol version
   * - 0x04
     - MANAGED_BDF
     - R
     - Guest BDF this device maps for
   * - 0x08
     - MAX_ENTRIES
     - R
     - Maximum entries per batch
   * - 0x0C
     - STATUS
     - R
     - Result of last doorbell
   * - 0x10
     - CMD_GPA_LO
     - W
     - Command page GPA [31:0]
   * - 0x14
     - CMD_GPA_HI
     - W
     - Command page GPA [63:32]
   * - 0x18
     - DOORBELL
     - W
     - Any write triggers batch processing
   * - 0x1C
     - FEATURES
     - R
     - Optional protocol features; zero on older devices

Version 3 optionally advertises ``APPLE_DMA_FEATURE_MAPPING_ID``. A guest
that observes this bit can supply a monotonically increasing, nonzero 31-bit
identifier in the map request flags; QEMU uses the corresponding opaque
64-bit token for registration and unmap. IDs are not reused during the
companion driver's lifetime. Exhaustion fails mapping rather than reusing an
ID. Older guests continue to use GPA-based tokens, and newer guests fall back
to GPA-based tokens when the feature is not advertised. The feature does not
make the DMA interception, callback context or teardown paths safe by itself.

On Darwin, QEMU rejects device initialization when its physical PCIe
extended-capability scan detects an invalid chain or ReBAR layout, rather
than passing the snapshot to the PCI capability parser. The error appears in
the QEMU guest-errors log. Writes to the physical ReBAR capability, including
its header, are blocked; duplicate BAR entries are hidden from the guest.

Guest sync callbacks translate subranges back to the original guest DMA
mapping and fail fatally if the range has no unambiguous owner. This does not
establish host-side DMA coherency or ordering.

When a coherent allocation's replacement DMA handle violates its mask or
allocation-order alignment, the guest kernel log reports the address, size,
mask and required alignment before rolling the mapping back. The app's
diagnostic export includes the guest kernel log when SSH collection succeeds.

If reported host DMA segments overlap a live window in device address space,
the guest stops with a fatal error instead of unregistering an overlapping
window whose effect on the other mapping is unknown. Failed or malformed
responses may not describe all live segments, so this is not complete overlap
protection or proof that a guest panic or host teardown safely quiesces the
device.

The guest module currently fails new streaming mappings in atomic context and
nonblocking DMA allocations. Unmapping in an atomic or otherwise unusable
context is still fatal because the backend cannot safely defer host unmaps.
Linux permits these calls, so this is a temporary safeguard, not DMA API
conformance. MMIO resource DMA mappings are also rejected when they reach the
guest hook; the companion only registers guest RAM. Linux DMA dispatch can
also bypass the hook, and its lifetime is not fully synchronized. Do not
enable physical guest DMA until those issues are resolved and validated.

Interrupts
----------

The dext stops interrupt setup if both MSI-X configuration and its MSI
fallback fail, and reports the failure in the host unified log. A successful
configuration does not yet establish complete guest-visible masking, pending,
vector-mode or reset semantics. Failed IRQ reads, notification rearming and
asynchronous notifications are reported in QEMU's guest-errors log; a rearm
failure can leave subsequent interrupts undelivered.

The dext tracks pending hardware interrupts (MSI/MSI-X) in a bitfield
(one bit per vector, up to 256 vectors).  When a hardware interrupt fires,
the dext sets the corresponding bit and completes an asynchronous
notification to QEMU via ``IOConnectCallAsyncMethod``.  The notification
is dispatched through a GCD queue, which wakes the QEMU main loop via an
``EventNotifier`` pipe.  QEMU then atomically reads and clears the pending
bits and delivers each flagged vector to the guest.

Limitations
===========

- **Guest kernel module required**: the ``apple-dma-pci`` protocol is not
  handled by standard guest drivers.
- **No migration support**: the Apple container does not support dirty page
  tracking.
- **Interrupt delivery**: interrupts are delivered asynchronously via IOKit
  rather than directly by the kernel, adding some overhead compared to
  Linux VFIO.
- **No hot-plug**: devices must be configured at VM startup.
- **DMA constraints**: see `Platform constraints`_ above.
- **Darwin only**: the base ``vfio-pci`` device type is not user-creatable
  on Darwin; use ``vfio-apple-pci`` instead.
