Darwin Intel host backend
==========================

On Darwin x86_64, QEMU selects the ``darwin-intel-iommu`` backend. The QEMU
backend communicates with the ``VFIOUserPCIDriver`` DriverKit extension using
the existing IOUserClient protocol. The extension creates ``IODMACommand``
objects for the device and prepares DMA mappings; Darwin's PCI/DMA stack
programs Intel VT-d and returns the device-visible DMA segments. QEMU does not
write VT-d registers or infer DMA addresses from guest physical addresses.

The Darwin arm64 build selects ``darwin-apple-dart`` and uses the same DriverKit
client protocol, with the platform DMA stack preparing DART mappings. PCI config
space, BAR access, reset, and interrupt forwarding are shared operations; the
host IOMMU-specific behavior is provided by the OS's IODMACommand implementation.

The guest DMA companion reports the DMA segments returned by the host mapping
operation. Consumers must use the returned DMA address and length, not assume
identity mapping from guest physical address. This backend work does not by
itself certify support for every Intel Mac, macOS release, or PCI device; it
establishes the x86_64 selection and dispatch path.
