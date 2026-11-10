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
these probes. Relevant Linux v7.0 sources are
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
including RAM, PCI apertures, firmware and reserved areas. ``candidate``
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
