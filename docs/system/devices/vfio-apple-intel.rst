# Intel Darwin host status

The Darwin VFIO backend is selected by the host operating system (`host_os ==
'darwin'`), not by the CPU architecture. QEMU builds for both arm64 and x86_64
Darwin therefore use the same DriverKit/PCIDriverKit client backend. The
backend abstraction is wired to the existing DriverKit client API; there is no
separate non-Darwin "Intel" backend selected for Intel Macs.

This is **not a claim of tested Intel-Mac passthrough support**. The host
backend still relies on the DriverKit extension successfully matching and
claiming the device, exposing BAR/config access, mapping guest memory for DMA,
and forwarding interrupts. Those capabilities must be tested on the intended
Intel Mac model, macOS release, and PCI/Thunderbolt device. In particular, the
existing guest companion device and DMA path were developed around the Apple
backend and must be validated against Intel host IOMMU behavior before this is
considered safe or supported.

## Intel validation gate

Before advertising support, test on an Intel Mac (for example, a 2019 MacBook
Pro with a supported external PCIe device):

1. Build the DriverKit extension and QEMU as x86_64 for the target macOS SDK and
   verify extension installation/activation and device matching.
2. Confirm device ownership and test config-space access, BAR mapping/MMIO, and
   reset while the host driver is detached/appropriately relinquished.
3. Exercise DMA map/unmap using `IODMACommand`; verify that the guest-visible
   DMA addresses match actual device transactions and that teardown leaves no
   stale mappings.
4. Test MSI/MSI-X delivery, interruption, VM shutdown, and error rollback.
5. Run representative guest workloads under load and repeat across cold boots
   and sleep/wake cycles. Record hardware, OS, entitlements, and limitations.

Until these tests pass, Intel Darwin should be described as a build/code path
under evaluation, not as supported passthrough hardware.
