# Apple-VFIO host architecture selection

Apple-VFIO is built on Darwin hosts. Its host backend selection is architecture
specific:

- x86_64 Darwin (Intel Macs): `darwin-intel-iommu`
- arm64 Darwin (Apple Silicon): `darwin-apple-dart`

Both backends use the DriverKit/PCIDriverKit service and the existing QEMU-to-
DriverKit client interface. The DriverKit extension's `IODMACommand` is the
host-side abstraction that prepares device-visible DMA segments; the platform
implementation is expected to use the Intel IOMMU on Intel hosts and DART on
Apple Silicon. The PCI config/BAR, reset, and interrupt operations continue to
use the shared DriverKit client operations.

This change provides architecture-specific backend identity and selection. It
does not, by itself, prove that all Intel Mac models, macOS releases, devices,
or IOMMU configurations work, nor does it implement a separate raw VT-d
programming interface in QEMU. Do not advertise a hardware/OS combination as
supported until the DriverKit path and DMA results have been verified there.

The current guest DMA companion protocol can remain shared only if it receives
and correctly uses the device-visible DMA segments returned by the host backend;
it must not assume that a guest physical address is itself the host DMA address.
