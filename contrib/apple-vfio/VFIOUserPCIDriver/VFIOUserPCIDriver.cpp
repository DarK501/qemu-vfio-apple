//
//  VFIOUserPCIDriver.cpp
//  VFIOUserPCIDriver
//
//  Created by scottjg on 3/18/26.
//

#include <os/log.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <DriverKit/OSMetaClass.h>
#include <DriverKit/OSData.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOUserServer.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IODispatchQueue.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <PCIDriverKit/IOPCIFamilyDefinitions.h>

#include "VFIOUserPCIDriver.h"
#include "VFIOUserPCIDriverUserClient.h"
#include "../../apple-dma/apple-dma-protocol.h"

enum {
    kVFIOUserPCIDriverUserClientMethodGetIdentity = 0,
    kVFIOUserPCIDriverUserClientMethodClaim = 1,
    kVFIOUserPCIDriverUserClientMethodTerminate = 2,
    kVFIOUserPCIDriverUserClientMethodAllocateDMABuffer = 3,
    kVFIOUserPCIDriverUserClientMethodFreeDMABuffer = 4,
    kVFIOUserPCIDriverUserClientMethodRegisterDMARegion = 5,
    kVFIOUserPCIDriverUserClientMethodUnregisterDMARegion = 6,
    kVFIOUserPCIDriverUserClientMethodProbeDMARegion = 7,
    kVFIOUserPCIDriverUserClientMethodConfigRead = 8,
    kVFIOUserPCIDriverUserClientMethodConfigWrite = 9,
    kVFIOUserPCIDriverUserClientMethodGetBARInfo = 10,
    kVFIOUserPCIDriverUserClientMethodMMIORead = 11,
    kVFIOUserPCIDriverUserClientMethodMMIOWrite = 12,
    kVFIOUserPCIDriverUserClientMethodSetupInterrupts = 13,
    kVFIOUserPCIDriverUserClientMethodCheckInterrupt = 14,
    kVFIOUserPCIDriverUserClientMethodWaitInterrupt = 15,
    kVFIOUserPCIDriverUserClientMethodSetIRQMask = 16,
    kVFIOUserPCIDriverUserClientMethodResetDevice = 17,
    kVFIOUserPCIDriverUserClientMethodProbePCIConfig = 18,
    kVFIOUserPCIDriverUserClientMethodClaimSelected = 19,
    kVFIOUserPCIDriverUserClientMethodReleaseClaim = 20,
};

enum {
    kVFIOUserPCIDriverUserClientMemoryTypeDMABuffer = 0,
    kVFIOUserPCIDriverUserClientMemoryTypeBAR0 = 1,
    kVFIOUserPCIDriverUserClientMemoryTypeBAR1 = 2,
    kVFIOUserPCIDriverUserClientMemoryTypeBAR2 = 3,
    kVFIOUserPCIDriverUserClientMemoryTypeBAR3 = 4,
    kVFIOUserPCIDriverUserClientMemoryTypeBAR4 = 5,
    kVFIOUserPCIDriverUserClientMemoryTypeBAR5 = 6,
    kVFIOUserPCIDriverUserClientMemoryTypeIRQState = 7,
};

/*
 * Keep enough live DMA region slots for large bring-up bursts while we map
 * page-sized control requests one-by-one.
 */
#define VFIO_USER_MAX_CLIENT_DMA_REGIONS 65536
#define VFIO_USER_MAX_CLIENT_DMA_CHUNKS 64
#define VFIO_USER_DMA_CHUNK_SIZE (1536ULL * 1024ULL * 1024ULL)
#define VFIO_USER_MAX_IRQ_VECTORS 256
#define VFIO_USER_IRQ_PENDING_WORDS 4  /* 4 × 64 = 256 vectors */

struct VFIOUserClientDMAChunk {
    bool valid;
    uint64_t iova;
    uint64_t size;
    IOMemoryDescriptor *memoryDescriptor;
    IODMACommand *dmaCommand;
};

struct VFIOUserClientDMARegion {
    bool valid;
    uint64_t iova;
    uint64_t size;
    uint32_t chunkCount;
    VFIOUserClientDMAChunk chunks[VFIO_USER_MAX_CLIENT_DMA_CHUNKS];
};

struct VFIOUserPCIDriver_IVars {
    VFIOUserPCIDriverUserClient *openerClient;
    bool claimBlocked;
    bool startOpen;
};

struct VFIOUserPCIDriverUserClient_IVars {
    bool claimed;
    IOMemoryDescriptor *barDescCache[6];
    IOBufferMemoryDescriptor *dmaBuffer;
    IODMACommand *dmaCommand;
    uint64_t dmaBufferSize;
    uint64_t dmaFlags;
    uint32_t dmaSegmentsCount;
    IOAddressSegment dmaSegments[32];
    VFIOUserClientDMARegion clientDMARegions[VFIO_USER_MAX_CLIENT_DMA_REGIONS];
    uint64_t activeDMARegionCount;
    uint64_t activeDMAChunkCount;
    uint64_t activeDMABytes;
    uint64_t peakDMARegionCount;
    uint64_t peakDMAChunkCount;
    uint64_t peakDMABytes;
    uint64_t dmaCompleteFailureCount;

    bool interruptsSetUp;
    uint32_t numInterrupts;
    uint32_t pendingInterruptCancelCount;
    IOInterruptDispatchSource *interruptSources[VFIO_USER_MAX_IRQ_VECTORS];
    IODispatchQueue *irqQueue;

    /*
     * IRQ pending/enabled bitmaps live in a shared page that the client
     * can map via IOConnectMapMemory64 (memory type kMemoryTypeIRQState).
     * The pointers below reference into that page so both the dext and
     * the client operate on the same cache lines without Mach IPC.
     *
     * Layout of the shared page (64 bytes used):
     *   [0x00..0x1F]  irqPending[4]  — dext sets bits, client clears
     *   [0x20..0x3F]  irqEnabled[4]  — client writes, dext reads
     */
    IOBufferMemoryDescriptor *irqSharedBuffer;
    volatile uint64_t *irqPending;   /* -> shared page offset 0x00 */
    volatile uint64_t *irqEnabled;   /* -> shared page offset 0x20 */
    OSAction *pendingInterruptNotify;
};

struct VFIOUserPCIDeviceIdentity {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendorID;
    uint16_t deviceID;
    uint32_t classCode;
};

static double
vfio_user_dma_bytes_to_mb(uint64_t bytes)
{
    return (double)bytes / (1024.0 * 1024.0);
}

static void
vfio_user_dma_log(const char *fmt, ...)
{
    char message[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    IOLog("vfio-user-dext: %s\n", message);
    os_log(OS_LOG_DEFAULT, "vfio-user-dext: %{public}s", message);
}

static void
vfio_user_note_dma_region_added(VFIOUserPCIDriverUserClient *client,
                                uint64_t size,
                                uint32_t chunkCount)
{
    if (client == nullptr || client->ivars == nullptr) {
        return;
    }

    client->ivars->activeDMARegionCount++;
    client->ivars->activeDMAChunkCount += chunkCount;
    client->ivars->activeDMABytes += size;

    if (client->ivars->activeDMARegionCount > client->ivars->peakDMARegionCount) {
        client->ivars->peakDMARegionCount = client->ivars->activeDMARegionCount;
    }
    if (client->ivars->activeDMAChunkCount > client->ivars->peakDMAChunkCount) {
        client->ivars->peakDMAChunkCount = client->ivars->activeDMAChunkCount;
    }
    if (client->ivars->activeDMABytes > client->ivars->peakDMABytes) {
        client->ivars->peakDMABytes = client->ivars->activeDMABytes;
    }
}

static void
vfio_user_note_dma_region_removed(VFIOUserPCIDriverUserClient *client,
                                  uint64_t size,
                                  uint32_t chunkCount)
{
    if (client == nullptr || client->ivars == nullptr) {
        return;
    }

    if (client->ivars->activeDMARegionCount > 0) {
        client->ivars->activeDMARegionCount--;
    }
    if (client->ivars->activeDMAChunkCount >= chunkCount) {
        client->ivars->activeDMAChunkCount -= chunkCount;
    } else {
        client->ivars->activeDMAChunkCount = 0;
    }
    if (client->ivars->activeDMABytes >= size) {
        client->ivars->activeDMABytes -= size;
    } else {
        client->ivars->activeDMABytes = 0;
    }
}

static kern_return_t
vfio_user_release_dma_region_chunks(VFIOUserPCIDriverUserClient *client,
                                    VFIOUserClientDMARegion *region)
{
    kern_return_t firstFailure = kIOReturnSuccess;

    if (client == nullptr || client->ivars == nullptr || region == nullptr) {
        return kIOReturnBadArgument;
    }

    for (uint32_t chunkIdx = 0; chunkIdx < region->chunkCount; chunkIdx++) {
        VFIOUserClientDMAChunk *chunk = &region->chunks[chunkIdx];

        if (chunk->dmaCommand != nullptr) {
            kern_return_t ret;

            ret = chunk->dmaCommand->CompleteDMA(
                kIODMACommandCompleteDMANoOptions);
            if (ret != kIOReturnSuccess) {
                client->ivars->dmaCompleteFailureCount++;
                if (firstFailure == kIOReturnSuccess) {
                    firstFailure = ret;
                }
                vfio_user_dma_log(
                    "CompleteDMA failed region=%#llx chunk=%#llx size=%llu "
                    "index=%u kr=%#x active_regions=%llu active_chunks=%llu "
                    "active_bytes=%.1f MB complete_failures=%llu",
                    region->iova,
                    chunk->iova,
                    chunk->size,
                    (unsigned int)chunkIdx,
                    ret,
                    client->ivars->activeDMARegionCount,
                    client->ivars->activeDMAChunkCount,
                    vfio_user_dma_bytes_to_mb(client->ivars->activeDMABytes),
                    client->ivars->dmaCompleteFailureCount);
                continue;
            }

            chunk->dmaCommand->release();
            chunk->dmaCommand = nullptr;
        }

        if (chunk->memoryDescriptor != nullptr) {
            chunk->memoryDescriptor->release();
            chunk->memoryDescriptor = nullptr;
        }

        chunk->valid = false;
    }

    return firstFailure;
}

static const char *
vfio_user_bar_type_string(uint8_t barType)
{
    switch (barType) {
    case kPCIBARTypeM32:
        return "mem32";
    case kPCIBARTypeIO:
        return "io";
    case kPCIBARTypeM64:
        return "mem64";
    case kPCIBARTypeM32PF:
        return "mem32-prefetch";
    case kPCIBARTypeM64PF:
        return "mem64-prefetch";
    default:
        return "unknown";
    }
}

static IOPCIDevice *
vfio_user_get_pci_device(IOService *service)
{
    if (service == nullptr) {
        return nullptr;
    }

    IOService *provider = service->GetProvider();
    IOPCIDevice *typedProvider = OSDynamicCast(IOPCIDevice, provider);
    return typedProvider;
}

static kern_return_t
vfio_user_registry_u32(IOPCIDevice *device, const char *name, uint32_t *value)
{
    OSContainer *property = nullptr;
    kern_return_t ret = device->SearchProperty(name, "IOService", 0, &property);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "vfio-user-dext: registry property %{public}s failed: %#x",
               name, ret);
        return ret;
    }

    OSData *data = OSDynamicCast(OSData, property);
    OSNumber *number = OSDynamicCast(OSNumber, property);
    if (data != nullptr && data->getLength() == 4 &&
        data->getBytesNoCopy() != nullptr) {
        const uint8_t *bytes = static_cast<const uint8_t *>(data->getBytesNoCopy());
        *value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                 ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    } else if (number != nullptr && number->unsigned64BitValue() <= UINT32_MAX) {
        *value = number->unsigned32BitValue();
    } else {
        os_log(OS_LOG_DEFAULT, "vfio-user-dext: registry property %{public}s has invalid type or length",
               name);
        OSSafeReleaseNULL(property);
        return kIOReturnBadArgument;
    }
    OSSafeReleaseNULL(property);
    return kIOReturnSuccess;
}

static kern_return_t
vfio_user_read_identity(IOService *service, VFIOUserPCIDeviceIdentity *identity)
{
    if (service == nullptr || identity == nullptr) {
        return kIOReturnBadArgument;
    }

    IOPCIDevice *pciDevice = vfio_user_get_pci_device(service);
    if (pciDevice == nullptr) {
        return kIOReturnUnsupported;
    }

    identity->bus = 0;
    identity->device = 0;
    identity->function = 0;
    identity->vendorID = 0xffff;
    identity->deviceID = 0xffff;
    identity->classCode = 0xffffffff;

    kern_return_t ret = pciDevice->GetBusDeviceFunction(&identity->bus,
                                                        &identity->device,
                                                        &identity->function);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    uint32_t vendor, device, classCode;
    ret = vfio_user_registry_u32(pciDevice, "vendor-id", &vendor);
    if (ret != kIOReturnSuccess) {
        return ret;
    }
    ret = vfio_user_registry_u32(pciDevice, "device-id", &device);
    if (ret != kIOReturnSuccess) {
        return ret;
    }
    ret = vfio_user_registry_u32(pciDevice, "class-code", &classCode);
    if (ret != kIOReturnSuccess) {
        return ret;
    }
    if (vendor >= 0xffff || device > 0xffff || classCode > 0xffffff) {
        os_log(OS_LOG_DEFAULT, "vfio-user-dext: invalid PCI registry identity");
        return kIOReturnBadArgument;
    }
    identity->vendorID = (uint16_t)vendor;
    identity->deviceID = (uint16_t)device;
    identity->classCode = classCode;
    return kIOReturnSuccess;
}

static bool
vfio_user_service_claimed(VFIOUserPCIDriverUserClient *client)
{
    VFIOUserPCIDriver *driverService;

    if (client == nullptr || client->ivars == nullptr ||
        !client->ivars->claimed) {
        return false;
    }

    driverService = OSDynamicCast(VFIOUserPCIDriver, client->GetProvider());
    return driverService != nullptr &&
           driverService->ivars != nullptr &&
           __atomic_load_n(&driverService->ivars->openerClient,
                           __ATOMIC_ACQUIRE) == client;
}

static kern_return_t
vfio_user_claim_device(VFIOUserPCIDriverUserClient *client,
                       IOService *driverService,
                       const VFIOUserPCIDeviceIdentity *identity)
{
    VFIOUserPCIDriver *driver;
    IOPCIDevice *pciDevice;
    kern_return_t ret;

    if (client == nullptr || client->ivars == nullptr || driverService == nullptr ||
        identity == nullptr) {
        return kIOReturnBadArgument;
    }

    driver = OSDynamicCast(VFIOUserPCIDriver, driverService);
    if (driver == nullptr || driver->ivars == nullptr) {
        return kIOReturnUnsupported;
    }

    if (vfio_user_service_claimed(client)) {
        return kIOReturnSuccess;
    }

    pciDevice = vfio_user_get_pci_device(driverService);
    if (pciDevice == nullptr) {
        return kIOReturnUnsupported;
    }

    VFIOUserPCIDriverUserClient *expected = nullptr;
    if (!__atomic_compare_exchange_n(&driver->ivars->openerClient, &expected,
                                     client, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        return kIOReturnExclusiveAccess;
    }
    if (__atomic_load_n(&driver->ivars->claimBlocked, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&driver->ivars->openerClient, nullptr, __ATOMIC_RELEASE);
        vfio_user_dma_log("claim rejected after failed DMA cleanup");
        return kIOReturnNotReady;
    }

    os_log(OS_LOG_DEFAULT, "vfio-user-dext: claim opening PCI device %02x:%02x.%u (enables bus mastering after open)",
           (unsigned int)identity->bus, (unsigned int)identity->device,
           (unsigned int)identity->function);
    ret = driver->ivars->startOpen ? kIOReturnSuccess : pciDevice->Open(driverService, 0);
    os_log(OS_LOG_DEFAULT, "vfio-user-dext: claim PCI Open returned %#x (held since Start: %d)", ret, (int)driver->ivars->startOpen);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT,
               "vfio-user-dext: failed to open PCI device for %02x:%02x.%u: %#x",
               (unsigned int)identity->bus,
               (unsigned int)identity->device,
               (unsigned int)identity->function, ret);
        if (!driver->ivars->startOpen) pciDevice->Close(driverService, 0);
        __atomic_store_n(&driver->ivars->openerClient, nullptr, __ATOMIC_RELEASE);
        return ret;
    }

    {
        uint16_t vendor = 0xffff;
        uint16_t device = 0xffff;
        uint16_t cmd = 0;
        uint16_t wanted;

        pciDevice->ConfigurationRead16(kIOPCIConfigurationOffsetVendorID, &vendor);
        pciDevice->ConfigurationRead16(kIOPCIConfigurationOffsetDeviceID, &device);
        pciDevice->ConfigurationRead16(0x04, &cmd);
        if (vendor == 0xffff || device == 0xffff || cmd == 0xffff ||
            vendor != identity->vendorID || device != identity->deviceID) {
            os_log(OS_LOG_DEFAULT,
                   "vfio-user-dext: claim configuration inaccessible or identity mismatch: registry=%04x:%04x config=%04x:%04x command=%04x",
                   (unsigned int)identity->vendorID,
                   (unsigned int)identity->deviceID,
                   (unsigned int)vendor, (unsigned int)device, (unsigned int)cmd);
            if (!driver->ivars->startOpen) pciDevice->Close(driverService, 0);
            __atomic_store_n(&driver->ivars->openerClient, nullptr, __ATOMIC_RELEASE);
            return kIOReturnNotReady;
        }
        os_log(OS_LOG_DEFAULT,
               "vfio-user-dext: PCI command register after Open: %#x", cmd);
        wanted = cmd | 0x06; /* Memory Space Enable + Bus Master Enable */
        if (wanted != cmd) {
            pciDevice->ConfigurationWrite16(0x04, wanted);
            pciDevice->ConfigurationRead16(0x04, &cmd);
            os_log(OS_LOG_DEFAULT,
                   "vfio-user-dext: PCI command register after enable: %#x", cmd);
        }
        if (cmd == 0xffff || (cmd & 0x06) != 0x06) {
            os_log(OS_LOG_DEFAULT,
                   "vfio-user-dext: PCI memory space or bus mastering did not enable");
            if (!driver->ivars->startOpen) pciDevice->Close(driverService, 0);
            __atomic_store_n(&driver->ivars->openerClient, nullptr, __ATOMIC_RELEASE);
            return kIOReturnNotReady;
        }
    }

    client->ivars->claimed = true;
    os_log(OS_LOG_DEFAULT,
           "vfio-user-dext: claimed PCI device %02x:%02x.%u",
           (unsigned int)identity->bus,
           (unsigned int)identity->device,
           (unsigned int)identity->function);
    return kIOReturnSuccess;
}

static kern_return_t
vfio_user_release_claim(VFIOUserPCIDriverUserClient *client)
{
    IOService *driverService;
    VFIOUserPCIDriver *driver;

    if (client == nullptr || client->ivars == nullptr) {
        return kIOReturnBadArgument;
    }
    if (!client->ivars->claimed) {
        return kIOReturnSuccess;
    }

    driverService = client->GetProvider();
    driver = OSDynamicCast(VFIOUserPCIDriver, driverService);
    if (driver == nullptr || driver->ivars == nullptr) {
        return kIOReturnNotAttached;
    }

    if (__atomic_load_n(&driver->ivars->openerClient, __ATOMIC_ACQUIRE) !=
        client) {
        vfio_user_dma_log("claim release rejected: client is not the owner");
        return kIOReturnNotOpen;
    }

    if (__atomic_load_n(&driver->ivars->claimBlocked, __ATOMIC_ACQUIRE) ||
        client->ivars->dmaCompleteFailureCount != 0) {
        vfio_user_dma_log(
            "claim release rejected: DMA cleanup is blocked "
            "(completion_failures=%llu)",
            client->ivars->dmaCompleteFailureCount);
        return kIOReturnNotReady;
    }
    if (client->ivars->activeDMARegionCount != 0 ||
        client->ivars->activeDMAChunkCount != 0 ||
        client->ivars->activeDMABytes != 0 ||
        client->ivars->dmaBuffer != nullptr ||
        client->ivars->dmaCommand != nullptr) {
        vfio_user_dma_log(
            "claim release rejected: DMA resources remain "
            "(regions=%llu, chunks=%llu, bytes=%llu, buffer=%d, command=%d)",
            client->ivars->activeDMARegionCount,
            client->ivars->activeDMAChunkCount,
            client->ivars->activeDMABytes,
            client->ivars->dmaBuffer != nullptr,
            client->ivars->dmaCommand != nullptr);
        return kIOReturnStillOpen;
    }

    IOPCIDevice *pciDevice = vfio_user_get_pci_device(driverService);
    if (pciDevice == nullptr) {
        vfio_user_dma_log("claim release: PCI provider is unavailable");
        return kIOReturnNotAttached;
    }

    uint16_t command = 0xffff;
    pciDevice->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &command);
    if (command == 0xffff) {
        os_log(OS_LOG_DEFAULT,
               "vfio-user-dext: claim release could not read PCI command register");
        return kIOReturnNotReady;
    }
    if ((command & 0x04) != 0) {
        pciDevice->ConfigurationWrite16(kIOPCIConfigurationOffsetCommand,
                                        command & ~0x04);
        pciDevice->ConfigurationRead16(kIOPCIConfigurationOffsetCommand,
                                       &command);
    }
    os_log(OS_LOG_DEFAULT,
           "vfio-user-dext: PCI command register after bus-master disable: %#x",
           command);
    if (command == 0xffff || (command & 0x04) != 0) {
        os_log(OS_LOG_DEFAULT,
               "vfio-user-dext: claim release refused; bus mastering remains enabled");
        return kIOReturnNotReady;
    }

    os_log(OS_LOG_DEFAULT,
           "vfio-user-dext: bus mastering verified disabled; releasing PCI claim");
    if (!driver->ivars->startOpen) {
        pciDevice->Close(driverService, 0);
        os_log(OS_LOG_DEFAULT, "vfio-user-dext: PCI claim Close returned");
    } else {
        os_log(OS_LOG_DEFAULT,
               "vfio-user-dext: retained Start-time PCI Open; bus mastering is off");
    }
    client->ivars->claimed = false;
    __atomic_store_n(&driver->ivars->claimBlocked, false, __ATOMIC_RELEASE);
    __atomic_store_n(&driver->ivars->openerClient, nullptr, __ATOMIC_RELEASE);
    return kIOReturnSuccess;
}

static kern_return_t
vfio_user_probe_pci_config(VFIOUserPCIDriverUserClient *client,
                           IOService *service,
                           const VFIOUserPCIDeviceIdentity *identity,
                           uint64_t *output)
{
    if (client == nullptr || client->ivars == nullptr || service == nullptr ||
        identity == nullptr || output == nullptr) {
        return kIOReturnBadArgument;
    }
    VFIOUserPCIDriver *driver = OSDynamicCast(VFIOUserPCIDriver, service);
    IOPCIDevice *pciDevice = vfio_user_get_pci_device(service);
    if (driver == nullptr || driver->ivars == nullptr || pciDevice == nullptr) {
        return kIOReturnNotAttached;
    }
    VFIOUserPCIDriverUserClient *expected = nullptr;
    if (!__atomic_compare_exchange_n(&driver->ivars->openerClient, &expected,
                                     client, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        return kIOReturnExclusiveAccess;
    }
    if (__atomic_load_n(&driver->ivars->claimBlocked, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&driver->ivars->openerClient, nullptr, __ATOMIC_RELEASE);
        return kIOReturnNotReady;
    }

    os_log(OS_LOG_DEFAULT, "vfio-user-dext: diagnostic probe opening %02x:%02x.%u",
           (unsigned int)identity->bus, (unsigned int)identity->device,
           (unsigned int)identity->function);
    kern_return_t ret = pciDevice->Open(service, 0);
    os_log(OS_LOG_DEFAULT, "vfio-user-dext: diagnostic probe Open returned %#x", ret);
    if (ret == kIOReturnSuccess) {
        uint16_t vendor = 0xffff, device = 0xffff, command = 0xffff;
        pciDevice->ConfigurationRead16(kIOPCIConfigurationOffsetVendorID, &vendor);
        pciDevice->ConfigurationRead16(kIOPCIConfigurationOffsetDeviceID, &device);
        pciDevice->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &command);
        os_log(OS_LOG_DEFAULT,
               "vfio-user-dext: diagnostic probe config vendor=%04x device=%04x command=%04x",
               (unsigned int)vendor, (unsigned int)device, (unsigned int)command);
        if (vendor == 0xffff || device == 0xffff || command == 0xffff ||
            vendor != identity->vendorID || device != identity->deviceID) {
            ret = kIOReturnNotReady;
        } else {
            output[0] = vendor;
            output[1] = device;
            output[2] = command;
        }
    }
    os_log(OS_LOG_DEFAULT, "vfio-user-dext: diagnostic probe calling Close (may change PCI command bits)");
    pciDevice->Close(service, 0);
    os_log(OS_LOG_DEFAULT, "vfio-user-dext: diagnostic probe Close returned; result=%#x", ret);
    __atomic_store_n(&driver->ivars->openerClient, nullptr, __ATOMIC_RELEASE);
    return ret;
}

static bool
vfio_user_identity_matches_input(const VFIOUserPCIDeviceIdentity *identity,
                                 const uint64_t *input)
{
    return input[0] == identity->bus && input[1] == identity->device &&
           input[2] == identity->function && input[3] == identity->vendorID &&
           input[4] == identity->deviceID;
}

static kern_return_t
vfio_user_release_dma_buffer(VFIOUserPCIDriverUserClient *client)
{
    if (client == nullptr || client->ivars == nullptr) {
        return kIOReturnBadArgument;
    }

    if (client->ivars->dmaCommand != nullptr) {
        kern_return_t ret = client->ivars->dmaCommand->CompleteDMA(
            kIODMACommandCompleteDMANoOptions);
        if (ret != kIOReturnSuccess) {
            vfio_user_dma_log("DMA buffer CompleteDMA failed: %#x", ret);
            return ret;
        }
    }

    OSSafeReleaseNULL(client->ivars->dmaCommand);
    OSSafeReleaseNULL(client->ivars->dmaBuffer);

    client->ivars->dmaBufferSize = 0;
    client->ivars->dmaFlags = 0;
    client->ivars->dmaSegmentsCount = 0;
    memset(client->ivars->dmaSegments, 0, sizeof(client->ivars->dmaSegments));
    return kIOReturnSuccess;
}

static kern_return_t
vfio_user_allocate_dma_buffer(VFIOUserPCIDriverUserClient *client,
                              uint64_t requestedSize,
                              uint64_t requestedAlignment)
{
    if (client == nullptr || client->ivars == nullptr) {
        return kIOReturnBadArgument;
    }

    if (!vfio_user_service_claimed(client)) {
        return kIOReturnNotOpen;
    }

    if (requestedSize == 0 || requestedSize > VFIO_USER_DMA_CHUNK_SIZE) {
        return kIOReturnBadArgument;
    }

    IOService *driverService = client->GetProvider();
    if (driverService == nullptr) {
        return kIOReturnNotAttached;
    }

    IOPCIDevice *pciDevice = vfio_user_get_pci_device(driverService);
    if (pciDevice == nullptr) {
        return kIOReturnUnsupported;
    }

    kern_return_t releaseRet = vfio_user_release_dma_buffer(client);
    if (releaseRet != kIOReturnSuccess) {
        return releaseRet;
    }

    uint64_t alignment = requestedAlignment;
    if (alignment == 0) {
        alignment = 4096;
    }

    IOBufferMemoryDescriptor *dmaBuffer = nullptr;
    kern_return_t ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOutIn,
                                                         requestedSize,
                                                         alignment,
                                                         &dmaBuffer);
    if (ret != kIOReturnSuccess || dmaBuffer == nullptr) {
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }

    ret = dmaBuffer->SetLength(requestedSize);
    if (ret != kIOReturnSuccess) {
        dmaBuffer->release();
        return ret;
    }

    IODMACommandSpecification specification = {};
    specification.options = kIODMACommandSpecificationNoOptions;
    specification.maxAddressBits = 64;

    IODMACommand *dmaCommand = nullptr;
    ret = IODMACommand::Create(pciDevice,
                               kIODMACommandCreateNoOptions,
                               &specification,
                               &dmaCommand);
    if (ret != kIOReturnSuccess || dmaCommand == nullptr) {
        dmaBuffer->release();
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }

    uint64_t dmaFlags = 0;
    uint32_t dmaSegmentsCount = 32;
    ret = dmaCommand->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                                    dmaBuffer,
                                    0,
                                    requestedSize,
                                    &dmaFlags,
                                    &dmaSegmentsCount,
                                    client->ivars->dmaSegments);
    if (ret != kIOReturnSuccess) {
        dmaCommand->release();
        dmaBuffer->release();
        return ret;
    }

    client->ivars->dmaBuffer = dmaBuffer;
    client->ivars->dmaCommand = dmaCommand;
    client->ivars->dmaBufferSize = requestedSize;
    client->ivars->dmaFlags = dmaFlags;
    client->ivars->dmaSegmentsCount = dmaSegmentsCount;

    VFIOUserPCIDeviceIdentity identity;
    if (vfio_user_read_identity(driverService, &identity) == kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT,
               "vfio-user-dext: allocated DMA buffer size=%llu alignment=%llu segments=%u first=%#llx/%#llx for %02x:%02x.%u",
               requestedSize,
               alignment,
               (unsigned int)dmaSegmentsCount,
               dmaSegmentsCount > 0 ? client->ivars->dmaSegments[0].address : 0,
               dmaSegmentsCount > 0 ? client->ivars->dmaSegments[0].length : 0,
               (unsigned int)identity.bus,
               (unsigned int)identity.device,
               (unsigned int)identity.function);
    }

    return kIOReturnSuccess;
}

static kern_return_t
vfio_user_register_client_dma_region(VFIOUserPCIDriverUserClient *client,
                                     uint64_t iova,
                                     uint64_t clientVA,
                                     uint64_t size,
                                     AppleDMAMapResult *outResult)
{
    if (client == nullptr || client->ivars == nullptr) {
        vfio_user_dma_log("register: bad client iova=%#llx size=%llu",
                          iova, size);
        return kIOReturnBadArgument;
    }

    if (!vfio_user_service_claimed(client)) {
        vfio_user_dma_log("register: service not claimed iova=%#llx size=%llu",
                          iova, size);
        return kIOReturnNotOpen;
    }

    if (size == 0 || clientVA == 0 ||
        size - 1 > UINT64_MAX - iova ||
        size - 1 > UINT64_MAX - clientVA) {
        vfio_user_dma_log("register: bad args iova=%#llx va=%#llx size=%llu",
                          iova, clientVA, size);
        return kIOReturnBadArgument;
    }

    IOService *driverService = client->GetProvider();
    if (driverService == nullptr) {
        vfio_user_dma_log("register: no provider iova=%#llx size=%llu",
                          iova, size);
        return kIOReturnNotAttached;
    }

    IOPCIDevice *pciDevice = vfio_user_get_pci_device(driverService);
    if (pciDevice == nullptr) {
        vfio_user_dma_log("register: no PCI device iova=%#llx size=%llu",
                          iova, size);
        return kIOReturnUnsupported;
    }

    int freeSlot = -1;
    for (int i = 0; i < VFIO_USER_MAX_CLIENT_DMA_REGIONS; i++) {
        if (client->ivars->clientDMARegions[i].valid &&
            client->ivars->clientDMARegions[i].iova == iova) {
            /*
             * The guest driver owns uniqueness of iova and is expected to
             * coalesce/refcount duplicate maps on its side before they reach
             * the dext.  A duplicate arriving here is a guest bug; log it
             * loudly and refuse so the problem is visible.
             */
            vfio_user_dma_log(
                "register: duplicate iova=%#llx size=%llu existing_size=%llu",
                iova, size, client->ivars->clientDMARegions[i].size);
            return kIOReturnExclusiveAccess;
        }
        if (!client->ivars->clientDMARegions[i].valid && freeSlot < 0) {
            freeSlot = i;
        }
    }

    if (freeSlot < 0) {
        vfio_user_dma_log("out of DMA region slots iova=%#llx size=%llu max=%u",
                          iova, size,
                          (unsigned int)VFIO_USER_MAX_CLIENT_DMA_REGIONS);
        return kIOReturnNoSpace;
    }

    if (outResult == nullptr) {
        vfio_user_dma_log("register: bad out params iova=%#llx size=%llu",
                          iova, size);
        return kIOReturnBadArgument;
    }
    memset(outResult, 0, sizeof(*outResult));
    outResult->mapping_id = iova;

    uint64_t chunkCountNeeded =
        (size - 1) / VFIO_USER_DMA_CHUNK_SIZE + 1;
    if (chunkCountNeeded > VFIO_USER_MAX_CLIENT_DMA_CHUNKS) {
        os_log(OS_LOG_DEFAULT,
               "vfio-user-dext: DMA region iova=%#llx size=%llu needs %llu chunks, max is %u",
               iova, size, chunkCountNeeded,
               (unsigned int)VFIO_USER_MAX_CLIENT_DMA_CHUNKS);
        return kIOReturnNoSpace;
    }

    VFIOUserClientDMARegion *region = &client->ivars->clientDMARegions[freeSlot];
    memset(region, 0, sizeof(*region));
    region->iova = iova;
    region->size = size;

    IODMACommandSpecification specification = {};
    specification.options = kIODMACommandSpecificationNoOptions;
    specification.maxAddressBits = 64;

    uint64_t remaining = size;
    uint64_t chunkOffset = 0;
    uint32_t chunkCount = 0;
    kern_return_t ret = kIOReturnSuccess;

    while (remaining > 0) {
        uint64_t chunkSize = remaining > VFIO_USER_DMA_CHUNK_SIZE ?
                             VFIO_USER_DMA_CHUNK_SIZE : remaining;
        uint64_t chunkIOVA = iova + chunkOffset;
        uint64_t chunkClientVA = clientVA + chunkOffset;
        IOAddressSegment clientSegment;
        clientSegment.address = chunkClientVA;
        clientSegment.length = chunkSize;

        IOMemoryDescriptor *memDesc = nullptr;
        ret = client->CreateMemoryDescriptorFromClient(kIOMemoryDirectionOutIn,
                                                       1,
                                                       &clientSegment,
                                                       &memDesc);
        if (ret != kIOReturnSuccess || memDesc == nullptr) {
            vfio_user_dma_log(
                "CreateMemoryDescriptorFromClient failed iova=%#llx va=%#llx "
                "size=%llu: %#x",
                chunkIOVA, chunkClientVA, chunkSize, ret);
            ret = ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
            goto rollback;
        }

        IODMACommand *dmaCmd = nullptr;
        ret = IODMACommand::Create(pciDevice,
                                   kIODMACommandCreateNoOptions,
                                   &specification,
                                   &dmaCmd);
        if (ret != kIOReturnSuccess || dmaCmd == nullptr) {
            vfio_user_dma_log(
                "IODMACommand::Create failed iova=%#llx size=%llu kr=%#x "
                "active_regions=%llu active_chunks=%llu active_bytes=%.1f MB",
                chunkIOVA, chunkSize, ret,
                client->ivars->activeDMARegionCount,
                client->ivars->activeDMAChunkCount,
                vfio_user_dma_bytes_to_mb(client->ivars->activeDMABytes));
            memDesc->release();
            ret = ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
            goto rollback;
        }

        uint64_t dmaFlags = 0;
        uint32_t dmaSegmentsCount = APPLE_DMA_MAX_SEGMENTS;
        IOAddressSegment dmaSegments[APPLE_DMA_MAX_SEGMENTS];
        uint64_t coveredLength = 0;
        memset(dmaSegments, 0, sizeof(dmaSegments));
        ret = dmaCmd->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                                    memDesc,
                                    0,
                                    chunkSize,
                                    &dmaFlags,
                                    &dmaSegmentsCount,
                                    dmaSegments);
        if (ret != kIOReturnSuccess) {
            vfio_user_dma_log(
                "PrepareForDMA failed iova=%#llx size=%llu kr=%#x "
                "active_regions=%llu active_chunks=%llu active_bytes=%.1f MB "
                "peak_regions=%llu peak_chunks=%llu peak_bytes=%.1f MB "
                "complete_failures=%llu max_regions=%u",
                chunkIOVA,
                chunkSize,
                ret,
                client->ivars->activeDMARegionCount,
                client->ivars->activeDMAChunkCount,
                vfio_user_dma_bytes_to_mb(client->ivars->activeDMABytes),
                client->ivars->peakDMARegionCount,
                client->ivars->peakDMAChunkCount,
                vfio_user_dma_bytes_to_mb(client->ivars->peakDMABytes),
                client->ivars->dmaCompleteFailureCount,
                (unsigned int)VFIO_USER_MAX_CLIENT_DMA_REGIONS);
            dmaCmd->release();
            memDesc->release();
            goto rollback;
        }

        VFIOUserClientDMAChunk *chunk = &region->chunks[chunkCount];
        chunk->valid = true;
        chunk->iova = chunkIOVA;
        chunk->size = chunkSize;
        chunk->memoryDescriptor = memDesc;
        chunk->dmaCommand = dmaCmd;
        chunkCount++;

        if (dmaSegmentsCount == 0 ||
            dmaSegmentsCount > APPLE_DMA_MAX_SEGMENTS) {
            vfio_user_dma_log(
                "PrepareForDMA returned invalid segment count=%u iova=%#llx",
                (unsigned int)dmaSegmentsCount, chunkIOVA);
            ret = kIOReturnBadArgument;
            goto rollback;
        }

        for (uint32_t seg = 0; seg < dmaSegmentsCount; seg++) {
            uint64_t segLength = dmaSegments[seg].length;

            if (segLength == 0 || segLength > chunkSize - coveredLength ||
                segLength - 1 > UINT64_MAX - dmaSegments[seg].address ||
                outResult->segment_count >= APPLE_DMA_MAX_SEGMENTS) {
                vfio_user_dma_log(
                    "invalid or excessive DMA segment iova=%#llx "
                    "segment=%u length=%llu covered=%llu chunk=%llu",
                    chunkIOVA, (unsigned int)seg, segLength, coveredLength,
                    chunkSize);
                ret = kIOReturnNoSpace;
                goto rollback;
            }

            AppleDMASegment *resultSegment =
                &outResult->segments[outResult->segment_count++];
            resultSegment->address = dmaSegments[seg].address;
            resultSegment->length = segLength;
            resultSegment->offset = chunkOffset + coveredLength;
            coveredLength += segLength;
        }

        if (coveredLength != chunkSize) {
            vfio_user_dma_log(
                "DMA segments do not cover requested chunk iova=%#llx "
                "covered=%llu chunk=%llu",
                chunkIOVA, coveredLength, chunkSize);
            ret = kIOReturnNotAligned;
            goto rollback;
        }

        chunkOffset += chunkSize;
        remaining -= chunkSize;
    }

    region->valid = true;
    region->chunkCount = chunkCount;
    vfio_user_note_dma_region_added(client, size, chunkCount);

    outResult->status = 0;
    return kIOReturnSuccess;

rollback:
    memset(outResult, 0, sizeof(*outResult));
    region->chunkCount = chunkCount;
    if (vfio_user_release_dma_region_chunks(client, region) !=
        kIOReturnSuccess) {
        region->valid = true;
        vfio_user_note_dma_region_added(client, size, chunkCount);
        outResult->mapping_id = iova;
        outResult->status = APPLE_DMA_MAP_STATUS_RETAINED;
        return kIOReturnSuccess;
    }
    memset(region, 0, sizeof(*region));
    return ret;
}

static kern_return_t
vfio_user_unregister_client_dma_region(VFIOUserPCIDriverUserClient *client,
                                       uint64_t iova)
{
    if (client == nullptr || client->ivars == nullptr) {
        return kIOReturnBadArgument;
    }

    for (int i = 0; i < VFIO_USER_MAX_CLIENT_DMA_REGIONS; i++) {
        VFIOUserClientDMARegion *region = &client->ivars->clientDMARegions[i];
        if (!region->valid || region->iova != iova) {
            continue;
        }

        kern_return_t releaseRet =
            vfio_user_release_dma_region_chunks(client, region);

        if (releaseRet != kIOReturnSuccess) {
            vfio_user_dma_log(
                "DMA region teardown saw CompleteDMA failure "
                "iova=%#llx size=%llu chunks=%u first_error=%#x",
                region->iova, region->size,
                (unsigned int)region->chunkCount, releaseRet);
            return releaseRet;
        }

        vfio_user_note_dma_region_removed(client, region->size,
                                          region->chunkCount);

        memset(region, 0, sizeof(*region));
        return kIOReturnSuccess;
    }

    return kIOReturnNotFound;
}

static void
vfio_user_release_all_client_dma_regions(VFIOUserPCIDriverUserClient *client)
{
    if (client == nullptr || client->ivars == nullptr) {
        return;
    }

    for (int i = 0; i < VFIO_USER_MAX_CLIENT_DMA_REGIONS; i++) {
        VFIOUserClientDMARegion *region = &client->ivars->clientDMARegions[i];
        if (!region->valid) {
            continue;
        }

        kern_return_t releaseRet =
            vfio_user_release_dma_region_chunks(client, region);

        if (releaseRet != kIOReturnSuccess) {
            vfio_user_dma_log(
                "DMA region cleanup at client stop saw CompleteDMA failure "
                "iova=%#llx size=%llu chunks=%u first_error=%#x",
                region->iova, region->size,
                (unsigned int)region->chunkCount, releaseRet);
            continue;
        }

        vfio_user_note_dma_region_removed(client, region->size,
                                          region->chunkCount);
        memset(region, 0, sizeof(*region));
    }
}

static kern_return_t
vfio_user_probe_client_dma_region(VFIOUserPCIDriverUserClient *client,
                                  uint64_t iova,
                                  uint64_t offset,
                                  uint64_t *outWord)
{
    if (client == nullptr || client->ivars == nullptr || outWord == nullptr) {
        return kIOReturnBadArgument;
    }

    for (int i = 0; i < VFIO_USER_MAX_CLIENT_DMA_REGIONS; i++) {
        VFIOUserClientDMARegion *region = &client->ivars->clientDMARegions[i];
        if (!region->valid || region->iova != iova) {
            continue;
        }

        if (offset + sizeof(uint64_t) > region->size) {
            return kIOReturnOverrun;
        }

        uint64_t targetIOVA = iova + offset;
        for (uint32_t chunkIdx = 0; chunkIdx < region->chunkCount; chunkIdx++) {
            VFIOUserClientDMAChunk *chunk = &region->chunks[chunkIdx];
            if (!chunk->valid || targetIOVA < chunk->iova ||
                targetIOVA + sizeof(uint64_t) > chunk->iova + chunk->size) {
                continue;
            }

            // Step 1: normal CreateMapping (kernel picks address)
            IOMemoryMap *map = nullptr;
            kern_return_t ret = chunk->memoryDescriptor->CreateMapping(
                0, 0, 0, 0, 0, &map);
            if (ret != kIOReturnSuccess || map == nullptr) {
                os_log(OS_LOG_DEFAULT,
                       "vfio-user-dext: probe CreateMapping failed iova=%#llx chunk=%#llx: %#x",
                       iova, chunk->iova, ret);
                return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
            }

            uint64_t mappedAddr = map->GetAddress();
            uint64_t mapLength = map->GetLength();
            uint64_t chunkOffset = targetIOVA - chunk->iova;
            *outWord = *(volatile uint64_t *)(mappedAddr + chunkOffset);

            // Step 2: test kIOMemoryMapFixedAddress — remap at the same VA
            uint64_t testAddr = mappedAddr;
            map->release();
            map = nullptr;

            IOMemoryMap *fixedMap = nullptr;
            kern_return_t fixedRet = chunk->memoryDescriptor->CreateMapping(
                kIOMemoryMapFixedAddress,  // 0x1
                testAddr,                  // map at the same address we just freed
                0,                         // offset
                mapLength,                 // length
                0,                         // alignment
                &fixedMap);

            if (fixedRet == kIOReturnSuccess && fixedMap != nullptr) {
                uint64_t fixedAddr = fixedMap->GetAddress();
                uint64_t fixedWord = *(volatile uint64_t *)(fixedAddr + chunkOffset);
                vfio_user_dma_log("*** kIOMemoryMapFixedAddress TEST PASSED *** "
                       "requested=%#llx got=%#llx match=%d value_match=%d",
                       testAddr, fixedAddr, testAddr == fixedAddr, *outWord == fixedWord);
                fixedMap->release();
            } else {
                vfio_user_dma_log("*** kIOMemoryMapFixedAddress TEST FAILED *** "
                       "requested=%#llx ret=%#x",
                       testAddr, fixedRet);
            }

            return kIOReturnSuccess;
        }

        return kIOReturnNotFound;
    }

    return kIOReturnNotFound;
}

/*
 * Synthesize a PCI BAR register value from GetBARInfo() metadata.
 *
 * The raw config space BAR registers contain host-assigned addresses that
 * must not be exposed as guest BAR addresses. GetBARInfo() returns the BAR
 * metadata (type, size) from IOPCIFamily.
 *
 * The address portion is left as zero — QEMU derives BAR sizes from
 * get_region_info, not from BAR probing.
 *
 * The PCIDriverKit BAR type encoding matches the PCI spec BAR register format:
 *   bit 0 = IO, bits 2:1 = memory type (10b = 64-bit), bit 3 = prefetchable
 * so the type value can be returned directly.
 */
static uint32_t
vfio_user_synthesize_bar_reg(IOPCIDevice *pciDevice, uint8_t barIndex)
{
    uint8_t memoryIndex = 0;
    uint64_t barSize = 0;
    uint8_t barType = 0;

    kern_return_t kr = pciDevice->GetBARInfo(barIndex, &memoryIndex,
                                              &barSize, &barType);
    if (kr != kIOReturnSuccess || barSize == 0) {
        return 0;
    }

    /*
     * The PCIDriverKit BAR type encoding matches the PCI spec BAR register
     * format, so return it directly.  GetBARInfo returns errors for upper
     * halves of 64-bit BARs and unused BARs, handled above.
     */
    return (uint32_t)barType;
}

static kern_return_t
vfio_user_config_read(VFIOUserPCIDriverUserClient *client,
                      uint64_t offset,
                      uint64_t width,
                      uint64_t *outValue)
{
    if (client == nullptr || client->ivars == nullptr || outValue == nullptr) {
        return kIOReturnBadArgument;
    }

    IOService *driverService = client->GetProvider();
    if (driverService == nullptr) {
        return kIOReturnNotAttached;
    }

    IOPCIDevice *pciDevice = vfio_user_get_pci_device(driverService);
    if (pciDevice == nullptr) {
        return kIOReturnUnsupported;
    }

    /*
     * BAR registers (0x10-0x27): synthesize proper values from GetBARInfo()
     * instead of returning host-assigned addresses as guest BAR values.
     */
    if (width == 4 && offset >= 0x10 && offset <= 0x24 &&
        (offset & 3) == 0) {
        *outValue = vfio_user_synthesize_bar_reg(pciDevice,
                                                  (offset - 0x10) / 4);
        return kIOReturnSuccess;
    }

    switch (width) {
    case 1: {
        uint8_t val = 0xff;
        pciDevice->ConfigurationRead8(offset, &val);
        *outValue = val;
        return kIOReturnSuccess;
    }
    case 2: {
        uint16_t val = 0xffff;
        pciDevice->ConfigurationRead16(offset, &val);
        *outValue = val;
        return kIOReturnSuccess;
    }
    case 4: {
        uint32_t val = 0xffffffff;
        pciDevice->ConfigurationRead32(offset, &val);
        *outValue = val;
        return kIOReturnSuccess;
    }
    default:
        return kIOReturnBadArgument;
    }
}

static kern_return_t
vfio_user_config_write(VFIOUserPCIDriverUserClient *client,
                       uint64_t offset,
                       uint64_t width,
                       uint64_t value)
{
    if (client == nullptr || client->ivars == nullptr) {
        return kIOReturnBadArgument;
    }

    IOService *driverService = client->GetProvider();
    if (driverService == nullptr) {
        return kIOReturnNotAttached;
    }

    IOPCIDevice *pciDevice = vfio_user_get_pci_device(driverService);
    if (pciDevice == nullptr) {
        return kIOReturnUnsupported;
    }

    switch (width) {
    case 1:
        pciDevice->ConfigurationWrite8(offset, (uint8_t)value);
        return kIOReturnSuccess;
    case 2:
        pciDevice->ConfigurationWrite16(offset, (uint16_t)value);
        return kIOReturnSuccess;
    case 4:
        pciDevice->ConfigurationWrite32(offset, (uint32_t)value);
        return kIOReturnSuccess;
    default:
        return kIOReturnBadArgument;
    }
}

static kern_return_t
vfio_user_mmio_read(VFIOUserPCIDriverUserClient *client,
                    uint64_t memoryIndex,
                    uint64_t offset,
                    uint64_t width,
                    uint64_t *outValue)
{
    if (client == nullptr || client->ivars == nullptr || outValue == nullptr) {
        return kIOReturnBadArgument;
    }

    IOService *driverService = client->GetProvider();
    if (driverService == nullptr) {
        return kIOReturnNotAttached;
    }

    IOPCIDevice *pciDevice = vfio_user_get_pci_device(driverService);
    if (pciDevice == nullptr) {
        return kIOReturnUnsupported;
    }

    switch (width) {
    case 1: {
        uint8_t val = 0xff;
        pciDevice->MemoryRead8((uint8_t)memoryIndex, offset, &val);
        *outValue = val;
        return kIOReturnSuccess;
    }
    case 2: {
        uint16_t val = 0xffff;
        pciDevice->MemoryRead16((uint8_t)memoryIndex, offset, &val);
        *outValue = val;
        return kIOReturnSuccess;
    }
    case 4: {
        uint32_t val = 0xffffffff;
        pciDevice->MemoryRead32((uint8_t)memoryIndex, offset, &val);
        *outValue = val;
        return kIOReturnSuccess;
    }
    case 8: {
        uint64_t val = 0xffffffffffffffffULL;
        pciDevice->MemoryRead64((uint8_t)memoryIndex, offset, &val);
        *outValue = val;
        return kIOReturnSuccess;
    }
    default:
        return kIOReturnBadArgument;
    }
}

static kern_return_t
vfio_user_mmio_write(VFIOUserPCIDriverUserClient *client,
                     uint64_t memoryIndex,
                     uint64_t offset,
                     uint64_t width,
                     uint64_t value)
{
    if (client == nullptr || client->ivars == nullptr) {
        return kIOReturnBadArgument;
    }

    IOService *driverService = client->GetProvider();
    if (driverService == nullptr) {
        return kIOReturnNotAttached;
    }

    IOPCIDevice *pciDevice = vfio_user_get_pci_device(driverService);
    if (pciDevice == nullptr) {
        return kIOReturnUnsupported;
    }

    switch (width) {
    case 1:
        pciDevice->MemoryWrite8((uint8_t)memoryIndex, offset, (uint8_t)value);
        return kIOReturnSuccess;
    case 2:
        pciDevice->MemoryWrite16((uint8_t)memoryIndex, offset, (uint16_t)value);
        return kIOReturnSuccess;
    case 4:
        pciDevice->MemoryWrite32((uint8_t)memoryIndex, offset, (uint32_t)value);
        return kIOReturnSuccess;
    case 8:
        pciDevice->MemoryWrite64((uint8_t)memoryIndex, offset, value);
        return kIOReturnSuccess;
    default:
        return kIOReturnBadArgument;
    }
}

kern_return_t
IMPL(VFIOUserPCIDriver, Start)
{
    os_log(OS_LOG_DEFAULT, "vfio-user-dext: service Start entered; calling super");
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    os_log(OS_LOG_DEFAULT, "vfio-user-dext: service super Start returned %#x", ret);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "vfio-user-dext: super Start failed: %#x", ret);
        return ret;
    }

    ivars = IONewZero(VFIOUserPCIDriver_IVars, 1);
    if (ivars == nullptr) {
        os_log(OS_LOG_DEFAULT, "vfio-user-dext: service Start allocation failed");
        return kIOReturnNoMemory;
    }

    VFIOUserPCIDeviceIdentity identity;
    os_log(OS_LOG_DEFAULT, "vfio-user-dext: service Start reading PCI identity");
    ret = vfio_user_read_identity(this, &identity);
    os_log(OS_LOG_DEFAULT, "vfio-user-dext: service Start identity returned %#x", ret);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "vfio-user-dext: failed to read PCI identity: %#x", ret);
        IOSafeDeleteNULL(ivars, VFIOUserPCIDriver_IVars, 1);
        return ret;
    }

    IOPCIDevice *pciDevice = vfio_user_get_pci_device(this);
    if (pciDevice == nullptr) {
        os_log(OS_LOG_DEFAULT, "vfio-user-dext: provider is not an IOPCIDevice");
        IOSafeDeleteNULL(ivars, VFIOUserPCIDriver_IVars, 1);
        return kIOReturnUnsupported;
    }

    os_log(OS_LOG_DEFAULT,
           "vfio-user-dext: idle service registry identity %02x:%02x.%u vendor=%04x device=%04x class=%06x",
           (unsigned int)identity.bus,
           (unsigned int)identity.device,
           (unsigned int)identity.function,
           (unsigned int)identity.vendorID,
           (unsigned int)identity.deviceID,
           (unsigned int)identity.classCode);

    for (uint8_t bar = 0; bar < 6; ++bar) {
        uint8_t memoryIndex = 0;
        uint64_t barSize = 0;
        uint8_t barType = 0;

        os_log(OS_LOG_DEFAULT, "vfio-user-dext: service Start querying BAR%u", (unsigned int)bar);
        kern_return_t barRet = pciDevice->GetBARInfo(bar, &memoryIndex, &barSize, &barType);
        os_log(OS_LOG_DEFAULT, "vfio-user-dext: service Start BAR%u returned %#x",
               (unsigned int)bar, barRet);
        if (barRet == kIOReturnSuccess) {
            os_log(OS_LOG_DEFAULT,
                   "vfio-user-dext: BAR%u memoryIndex=%u size=%llu type=%{public}s",
                   (unsigned int)bar,
                   (unsigned int)memoryIndex,
                   barSize,
                   vfio_user_bar_type_string(barType));
        }
    }

    ret = pciDevice->Open(this, 0);
    os_log(OS_LOG_DEFAULT, "vfio-user-dext: service Start early PCI Open returned %#x", ret);
    ivars->startOpen = (ret == kIOReturnSuccess);

    os_log(OS_LOG_DEFAULT, "vfio-user-dext: service Start calling RegisterService");
    RegisterService();
    os_log(OS_LOG_DEFAULT, "vfio-user-dext: service Start registered; returning success");
    return kIOReturnSuccess;
}

kern_return_t
IMPL(VFIOUserPCIDriver, NewUserClient)
{
    os_log(OS_LOG_DEFAULT, "vfio-user-dext: NewUserClient entered type=%u", type);
    if (type != 0) {
        os_log(OS_LOG_DEFAULT, "vfio-user-dext: unsupported user-client type %u", type);
        return kIOReturnUnsupported;
    }

    IOService *clientService = nullptr;
    kern_return_t ret = Create(this, "VFIOUserPCIDriverUserClientProperties", &clientService);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "vfio-user-dext: failed to create user client: %#x", ret);
        return ret;
    }

    IOUserClient *typedClient = OSDynamicCast(IOUserClient, clientService);
    if (typedClient == nullptr) {
        clientService->release();
        os_log(OS_LOG_DEFAULT, "vfio-user-dext: created service is not an IOUserClient");
        return kIOReturnUnsupported;
    }

    *userClient = typedClient;
    os_log(OS_LOG_DEFAULT, "vfio-user-dext: NewUserClient returning success");
    return kIOReturnSuccess;
}

kern_return_t
IMPL(VFIOUserPCIDriver, Stop)
{
    os_log(OS_LOG_DEFAULT, "vfio-user-dext: service Stop entered");
    if (ivars != nullptr &&
        __atomic_load_n(&ivars->openerClient, __ATOMIC_ACQUIRE) != nullptr) {
        os_log(OS_LOG_DEFAULT,
               "vfio-user-dext: provider stop request rejected while PCI "
               "device remains claimed; DriverKit may still force teardown");
        return kIOReturnStillOpen;
    }
    if (ivars != nullptr && ivars->startOpen) {
        IOPCIDevice *stopDevice = vfio_user_get_pci_device(this);
        if (stopDevice != nullptr) {
            stopDevice->Close(this, 0);
            os_log(OS_LOG_DEFAULT, "vfio-user-dext: service Stop closed early PCI Open");
        }
    }
    IOSafeDeleteNULL(ivars, VFIOUserPCIDriver_IVars, 1);
    os_log(OS_LOG_DEFAULT, "vfio-user-dext: service Stop calling super");
    kern_return_t ret = Stop(provider, SUPERDISPATCH);
    os_log(OS_LOG_DEFAULT, "vfio-user-dext: service super Stop returned %#x", ret);
    return ret;
}

kern_return_t
IMPL(VFIOUserPCIDriverUserClient, Start)
{
    os_log(OS_LOG_DEFAULT, "vfio-user-dext: user client Start entered; calling super");
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    os_log(OS_LOG_DEFAULT, "vfio-user-dext: user client super Start returned %#x", ret);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "vfio-user-dext: user client super Start failed: %#x", ret);
        return ret;
    }

    if (OSDynamicCast(VFIOUserPCIDriver, provider) == nullptr) {
        os_log(OS_LOG_DEFAULT, "vfio-user-dext: user client provider is not VFIOUserPCIDriver");
        return kIOReturnUnsupported;
    }

    ivars = IONewZero(VFIOUserPCIDriverUserClient_IVars, 1);
    if (ivars == nullptr) {
        os_log(OS_LOG_DEFAULT, "vfio-user-dext: user client Start allocation failed");
        return kIOReturnNoMemory;
    }

    os_log(OS_LOG_DEFAULT, "vfio-user-dext: user client Start returning success");
    return kIOReturnSuccess;
}

static kern_return_t
vfio_user_release_all_interrupts(VFIOUserPCIDriverUserClient *);

kern_return_t
IMPL(VFIOUserPCIDriverUserClient, Stop)
{
    os_log(OS_LOG_DEFAULT, "vfio-user-dext: user client Stop entered");
    if (ivars != nullptr &&
        (ivars->activeDMARegionCount != 0 || ivars->dmaCompleteFailureCount != 0)) {
        vfio_user_dma_log(
            "stopping user client with active_regions=%llu "
            "active_chunks=%llu active_bytes=%.1f MB peak_regions=%llu "
            "peak_chunks=%llu peak_bytes=%.1f MB complete_failures=%llu",
            ivars->activeDMARegionCount,
            ivars->activeDMAChunkCount,
            vfio_user_dma_bytes_to_mb(ivars->activeDMABytes),
            ivars->peakDMARegionCount,
            ivars->peakDMAChunkCount,
            vfio_user_dma_bytes_to_mb(ivars->peakDMABytes),
            ivars->dmaCompleteFailureCount);
    }

    kern_return_t irqRet = vfio_user_release_all_interrupts(this);
    if (irqRet != kIOReturnSuccess) {
        vfio_user_dma_log(
            "client stop deferred while interrupt cancellation is pending "
            "(kr=%#x); retaining client state", irqRet);
        return kIOReturnStillOpen;
    }
    vfio_user_release_all_client_dma_regions(this);
    kern_return_t dmaBufferRet = ivars != nullptr ?
        vfio_user_release_dma_buffer(this) : kIOReturnSuccess;
    if (ivars != nullptr &&
        (ivars->activeDMARegionCount != 0 || dmaBufferRet != kIOReturnSuccess)) {
        VFIOUserPCIDriver *driver = OSDynamicCast(VFIOUserPCIDriver, GetProvider());
        if (driver != nullptr && driver->ivars != nullptr) {
            __atomic_store_n(&driver->ivars->claimBlocked, true, __ATOMIC_RELEASE);
            vfio_user_dma_log(
                "client stop refused: active DMA mappings remain "
                "(regions=%llu, buffer_complete=%#x); retaining claim and "
                "DMA backing in this Stop call; DriverKit may still force teardown",
                ivars->activeDMARegionCount, dmaBufferRet);
        } else {
            vfio_user_dma_log(
                "client stop refused: active DMA mappings remain "
                "(regions=%llu, buffer_complete=%#x); provider state "
                "unavailable; DriverKit may still force teardown",
                ivars->activeDMARegionCount, dmaBufferRet);
        }
        return kIOReturnStillOpen;
    }
    kern_return_t claimRet = vfio_user_release_claim(this);
    if (claimRet != kIOReturnSuccess) {
        VFIOUserPCIDriver *driver = OSDynamicCast(VFIOUserPCIDriver, GetProvider());
        if (driver != nullptr && driver->ivars != nullptr) {
            __atomic_store_n(&driver->ivars->claimBlocked, true, __ATOMIC_RELEASE);
        }
        vfio_user_dma_log(
            "client stop refused: could not close PCI claim (kr=%#x); "
            "retaining claim in this Stop call; DriverKit may still force teardown",
            claimRet);
        return kIOReturnStillOpen;
    }
    if (ivars != nullptr) {
        for (int i = 0; i < 6; i++) {
            OSSafeReleaseNULL(ivars->barDescCache[i]);
        }
    }

    IOSafeDeleteNULL(ivars, VFIOUserPCIDriverUserClient_IVars, 1);
    os_log(OS_LOG_DEFAULT, "vfio-user-dext: user client Stop calling super");
    kern_return_t ret = Stop(provider, SUPERDISPATCH);
    os_log(OS_LOG_DEFAULT, "vfio-user-dext: user client super Stop returned %#x", ret);
    return ret;
}

static kern_return_t
vfio_user_release_all_interrupts(VFIOUserPCIDriverUserClient *client)
{
    if (client == nullptr) {
        return kIOReturnBadArgument;
    }
    if (client->ivars == nullptr) {
        return kIOReturnSuccess;
    }

    if (__atomic_load_n(&client->ivars->pendingInterruptCancelCount,
                        __ATOMIC_ACQUIRE) != 0) {
        return kIOReturnStillOpen;
    }

    __atomic_store_n(&client->ivars->interruptsSetUp, false,
                     __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < client->ivars->numInterrupts; i++) {
        IOInterruptDispatchSource *src = client->ivars->interruptSources[i];
        if (src != nullptr) {
            /*
             * Cancel is async — it schedules a completion block on src's
             * dispatch queue. Keep the client and ivars alive until the
             * completion callback drains, then require Stop to be retried.
             */
            client->retain();
            __atomic_add_fetch(&client->ivars->pendingInterruptCancelCount, 1,
                               __ATOMIC_ACQ_REL);
            src->Cancel(^{
                __atomic_sub_fetch(
                    &client->ivars->pendingInterruptCancelCount, 1,
                    __ATOMIC_ACQ_REL);
                src->release();
                client->release();
            });
            client->ivars->interruptSources[i] = nullptr;
        }
    }

    if (__atomic_load_n(&client->ivars->pendingInterruptCancelCount,
                        __ATOMIC_ACQUIRE) != 0) {
        return kIOReturnStillOpen;
    }

    if (client->ivars->irqQueue != nullptr) {
        client->ivars->irqQueue->release();
        client->ivars->irqQueue = nullptr;
    }

    client->ivars->numInterrupts = 0;

    if (client->ivars->irqPending != nullptr) {
        for (int i = 0; i < VFIO_USER_IRQ_PENDING_WORDS; i++) {
            __atomic_store_n(&client->ivars->irqPending[i], 0, __ATOMIC_RELEASE);
        }
    }

    OSAction *pending = __atomic_exchange_n(
        &client->ivars->pendingInterruptNotify, nullptr, __ATOMIC_ACQ_REL);
    if (pending != nullptr) {
        pending->release();
    }

    client->ivars->irqPending = nullptr;
    client->ivars->irqEnabled = nullptr;
    OSSafeReleaseNULL(client->ivars->irqSharedBuffer);

    os_log(OS_LOG_DEFAULT, "vfio-user-dext: released all interrupt sources");
    return kIOReturnSuccess;
}

static uint32_t
vfio_user_get_requested_interrupt_vectors(IOPCIDevice *pciDevice,
                                          bool *outUsingMSIX)
{
    uint64_t capOffset = 0;
    uint16_t msgCtrl = 0;

    if (outUsingMSIX != nullptr) {
        *outUsingMSIX = false;
    }

    if (pciDevice == nullptr) {
        return 1;
    }

    if (pciDevice->FindPCICapability(kIOPCICapabilityIDMSIX, 0,
                                     &capOffset) == kIOReturnSuccess) {
        pciDevice->ConfigurationRead16((uint32_t)(capOffset + 2), &msgCtrl);
        if (outUsingMSIX != nullptr) {
            *outUsingMSIX = true;
        }
        return (msgCtrl & 0x07ffu) + 1u;
    }

    if (pciDevice->FindPCICapability(kIOPCICapabilityIDMSI, 0,
                                     &capOffset) == kIOReturnSuccess) {
        uint8_t mmc;

        pciDevice->ConfigurationRead16((uint32_t)(capOffset + 2), &msgCtrl);
        mmc = (msgCtrl >> 1) & 0x7;
        return 1u << mmc;
    }

    return 1;
}

static kern_return_t
vfio_user_reset_device(VFIOUserPCIDriverUserClient *client)
{
    if (client == nullptr || client->ivars == nullptr) {
        return kIOReturnBadArgument;
    }

    if (!vfio_user_service_claimed(client)) {
        return kIOReturnNotOpen;
    }

    IOService *driverService = client->GetProvider();
    if (driverService == nullptr) {
        return kIOReturnNotAttached;
    }

    IOPCIDevice *pciDevice = vfio_user_get_pci_device(driverService);
    if (pciDevice == nullptr) {
        return kIOReturnUnsupported;
    }

    /*
     * IOPCIDevice::Reset() handles config save/restore, link training, and
     * device-ready polling internally.  Prefer FLR; fall back to an
     * upstream-port hot reset (secondary bus reset) if the function
     * doesn't advertise FLR.
     */
    kern_return_t ret = pciDevice->Reset(kIOPCIDeviceResetTypeFunctionReset,
                                         kIOPCIDeviceResetOptionNone);
    if (ret == kIOReturnSuccess) {
        return ret;
    }

    return pciDevice->Reset(kIOPCIDeviceResetTypeHotReset,
                            kIOPCIDeviceResetOptionNone);
}

static kern_return_t
vfio_user_setup_interrupts(VFIOUserPCIDriverUserClient *client)
{
    if (client == nullptr || client->ivars == nullptr) {
        return kIOReturnBadArgument;
    }

    if (!vfio_user_service_claimed(client)) {
        return kIOReturnNotOpen;
    }

    if (__atomic_load_n(&client->ivars->interruptsSetUp, __ATOMIC_ACQUIRE)) {
        return kIOReturnStillOpen;
    }

    IOService *driverService = client->GetProvider();
    if (driverService == nullptr) {
        return kIOReturnNotAttached;
    }

    IOPCIDevice *pciDevice = vfio_user_get_pci_device(driverService);
    if (pciDevice == nullptr) {
        return kIOReturnUnsupported;
    }

    IODispatchQueue *irqDispatchQueue = nullptr;
    kern_return_t ret = IODispatchQueue::Create("VFIOUserIRQQueue", 0, 0,
                                                 &irqDispatchQueue);
    if (ret != kIOReturnSuccess || irqDispatchQueue == nullptr) {
        os_log(OS_LOG_DEFAULT,
               "vfio-user-dext: failed to create IRQ dispatch queue: %#x", ret);
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }

    client->ivars->irqQueue = irqDispatchQueue;

    /* Allocate the shared IRQ state page */
    IOBufferMemoryDescriptor *irqBuf = nullptr;
    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOutIn,
                                           16384, 16384, &irqBuf);
    if (ret != kIOReturnSuccess || irqBuf == nullptr) {
        irqDispatchQueue->release();
        client->ivars->irqQueue = nullptr;
        os_log(OS_LOG_DEFAULT,
               "vfio-user-dext: failed to create IRQ shared buffer: %#x", ret);
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }

    IOAddressSegment irqRange = {};
    ret = irqBuf->GetAddressRange(&irqRange);
    if (ret != kIOReturnSuccess || irqRange.address == 0) {
        irqBuf->release();
        irqDispatchQueue->release();
        client->ivars->irqQueue = nullptr;
        return kIOReturnNoMemory;
    }

    auto *shared = reinterpret_cast<volatile uint64_t *>(irqRange.address);
    client->ivars->irqSharedBuffer = irqBuf;
    client->ivars->irqPending = shared;                          /* offset 0x00 */
    client->ivars->irqEnabled = shared + VFIO_USER_IRQ_PENDING_WORDS; /* offset 0x20 */

    for (int i = 0; i < VFIO_USER_IRQ_PENDING_WORDS; i++) {
        __atomic_store_n(&client->ivars->irqPending[i], 0, __ATOMIC_RELEASE);
        __atomic_store_n(&client->ivars->irqEnabled[i], ~0ULL, __ATOMIC_RELEASE);
    }
    client->ivars->pendingInterruptNotify = nullptr;

    bool usingMSIX = false;
    uint32_t requested = vfio_user_get_requested_interrupt_vectors(pciDevice,
                                                                   &usingMSIX);
    if (requested == 0 || requested > VFIO_USER_MAX_IRQ_VECTORS) {
        requested = VFIO_USER_MAX_IRQ_VECTORS;
    }

    ret = pciDevice->ConfigureInterrupts(usingMSIX ?
                                         kIOInterruptTypePCIMessagedX :
                                         kIOInterruptTypePCIMessaged,
                                         1, requested, 0);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT,
               "vfio-user-dext: ConfigureInterrupts(%{public}s, requested=%u) failed: %#x",
               usingMSIX ? "MSI-X" : "MSI",
               (unsigned int)requested, ret);

        if (usingMSIX) {
            usingMSIX = false;
            requested = 1;
            ret = pciDevice->ConfigureInterrupts(kIOInterruptTypePCIMessaged,
                                                 1, requested, 0);
            if (ret != kIOReturnSuccess) {
                os_log(OS_LOG_DEFAULT,
                       "vfio-user-dext: ConfigureInterrupts(MSI fallback) failed: %#x",
                       ret);
            }
        }
    } else {
        os_log(OS_LOG_DEFAULT,
               "vfio-user-dext: ConfigureInterrupts(%{public}s) requested=%u",
               usingMSIX ? "MSI-X" : "MSI",
               (unsigned int)requested);
    }

    uint32_t registered = 0;

    for (uint32_t i = 0; i < requested; i++) {
        IOInterruptDispatchSource *source = nullptr;

        ret = IOInterruptDispatchSource::Create(pciDevice, i,
                                                 irqDispatchQueue, &source);
        if (ret != kIOReturnSuccess || source == nullptr) {
            os_log(OS_LOG_DEFAULT,
                   "vfio-user-dext: IOInterruptDispatchSource::Create failed for vector %u/%u: %#x",
                   (unsigned int)i, (unsigned int)requested, ret);
            break;
        }

        OSAction *action = nullptr;
        ret = client->CreateActionInterruptOccurred(sizeof(uint32_t),
                                                     &action);
        if (ret != kIOReturnSuccess || action == nullptr) {
            source->release();
            os_log(OS_LOG_DEFAULT,
                   "vfio-user-dext: CreateAction failed for vector %u: %#x",
                   i, ret);
            break;
        }

        uint32_t *vectorRef = (uint32_t *)action->GetReference();
        if (vectorRef != nullptr) {
            *vectorRef = i;
        }

        ret = source->SetHandler(action);
        if (ret != kIOReturnSuccess) {
            action->release();
            source->release();
            os_log(OS_LOG_DEFAULT,
                   "vfio-user-dext: SetHandler failed for vector %u: %#x",
                   i, ret);
            break;
        }

        ret = source->SetEnable(true);
        if (ret != kIOReturnSuccess) {
            action->release();
            source->release();
            os_log(OS_LOG_DEFAULT,
                   "vfio-user-dext: SetEnable failed for vector %u: %#x",
                   i, ret);
            break;
        }

        client->ivars->interruptSources[i] = source;
        registered++;
    }

    client->ivars->numInterrupts = registered;
    __atomic_store_n(&client->ivars->interruptsSetUp, registered > 0,
                     __ATOMIC_RELEASE);

    os_log(OS_LOG_DEFAULT,
           "vfio-user-dext: registered %u/%u interrupt vectors (%{public}s)",
           (unsigned int)registered, (unsigned int)requested,
           usingMSIX ? "MSI-X" : "MSI");

    if (registered > 0) {
        return kIOReturnSuccess;
    }

    kern_return_t cleanupRet = vfio_user_release_all_interrupts(client);
    if (cleanupRet != kIOReturnSuccess) {
        return cleanupRet;
    }
    return ret != kIOReturnSuccess ? ret : kIOReturnNotFound;
}

kern_return_t
VFIOUserPCIDriverUserClient::ExternalMethod(uint64_t selector,
                                            IOUserClientMethodArguments *arguments,
                                            const IOUserClientMethodDispatch *dispatch,
                                            OSObject *target,
                                            void *reference)
{
    (void)dispatch;
    (void)target;
    (void)reference;

    if (arguments == nullptr) {
        return kIOReturnBadArgument;
    }

    IOService *driverService = GetProvider();
    if (driverService == nullptr) {
        return kIOReturnNotAttached;
    }

    VFIOUserPCIDeviceIdentity identity;
    kern_return_t ret = vfio_user_read_identity(driverService, &identity);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    if (selector != kVFIOUserPCIDriverUserClientMethodGetIdentity &&
        selector != kVFIOUserPCIDriverUserClientMethodClaim &&
        selector != kVFIOUserPCIDriverUserClientMethodProbePCIConfig &&
        selector != kVFIOUserPCIDriverUserClientMethodClaimSelected &&
        selector != kVFIOUserPCIDriverUserClientMethodReleaseClaim &&
        !vfio_user_service_claimed(this)) {
        return kIOReturnNotOpen;
    }

    switch (selector) {
    case kVFIOUserPCIDriverUserClientMethodProbePCIConfig:
    case kVFIOUserPCIDriverUserClientMethodClaimSelected:
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount != 5 ||
            arguments->scalarOutput == nullptr || arguments->scalarOutputCount < 3) {
            return kIOReturnBadArgument;
        }
        if (!vfio_user_identity_matches_input(&identity, arguments->scalarInput)) {
            return kIOReturnNotReady;
        }
        if (selector == kVFIOUserPCIDriverUserClientMethodProbePCIConfig) {
            ret = vfio_user_probe_pci_config(this, driverService, &identity,
                                             arguments->scalarOutput);
        } else {
            ret = vfio_user_claim_device(this, driverService, &identity);
            if (ret == kIOReturnSuccess) {
                arguments->scalarOutput[0] = identity.vendorID;
                arguments->scalarOutput[1] = identity.deviceID;
                arguments->scalarOutput[2] = 0x06;
            }
        }
        if (ret == kIOReturnSuccess) {
            arguments->scalarOutputCount = 3;
        }
        return ret;

    case kVFIOUserPCIDriverUserClientMethodGetIdentity:
        if (arguments->scalarOutput == nullptr || arguments->scalarOutputCount < 6) {
            return kIOReturnBadArgument;
        }

        arguments->scalarOutput[0] = identity.bus;
        arguments->scalarOutput[1] = identity.device;
        arguments->scalarOutput[2] = identity.function;
        arguments->scalarOutput[3] = identity.vendorID;
        arguments->scalarOutput[4] = identity.deviceID;
        arguments->scalarOutput[5] = identity.classCode;
        arguments->scalarOutputCount = 6;
        return kIOReturnSuccess;

    case kVFIOUserPCIDriverUserClientMethodClaim: {
        return vfio_user_claim_device(this, driverService, &identity);
    }

    case kVFIOUserPCIDriverUserClientMethodReleaseClaim:
        if (ivars != nullptr && ivars->claimed) {
            ret = vfio_user_release_all_interrupts(this);
            if (ret != kIOReturnSuccess) {
                os_log(OS_LOG_DEFAULT,
                       "vfio-user-dext: claim release waiting for interrupt cleanup: %#x",
                       ret);
                return ret;
            }
        }
        return vfio_user_release_claim(this);

    case kVFIOUserPCIDriverUserClientMethodTerminate:
        os_log(OS_LOG_DEFAULT,
               "vfio-user-dext: terminating driver for %02x:%02x.%u (not selected by host app)",
               (unsigned int)identity.bus,
               (unsigned int)identity.device,
               (unsigned int)identity.function);
        driverService->Terminate(0);
        return kIOReturnSuccess;

    case kVFIOUserPCIDriverUserClientMethodAllocateDMABuffer: {
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount < 2) {
            return kIOReturnBadArgument;
        }
        if (arguments->scalarOutput == nullptr || arguments->scalarOutputCount < 6) {
            return kIOReturnBadArgument;
        }

        ret = vfio_user_allocate_dma_buffer(this,
                                            arguments->scalarInput[0],
                                            arguments->scalarInput[1]);
        if (ret != kIOReturnSuccess) {
            return ret;
        }

        arguments->scalarOutput[0] = ivars->dmaBufferSize;
        arguments->scalarOutput[1] = ivars->dmaFlags;
        arguments->scalarOutput[2] = ivars->dmaSegmentsCount;
        arguments->scalarOutput[3] = ivars->dmaSegmentsCount > 0 ? ivars->dmaSegments[0].address : 0;
        arguments->scalarOutput[4] = ivars->dmaSegmentsCount > 0 ? ivars->dmaSegments[0].length : 0;
        arguments->scalarOutput[5] = identity.classCode;
        arguments->scalarOutputCount = 6;
        return kIOReturnSuccess;
    }

    case kVFIOUserPCIDriverUserClientMethodFreeDMABuffer:
        return vfio_user_release_dma_buffer(this);

    case kVFIOUserPCIDriverUserClientMethodRegisterDMARegion: {
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount < 3) {
            return kIOReturnBadArgument;
        }
        if (arguments->structureOutputDescriptor != nullptr ||
            arguments->structureOutputMaximumSize < sizeof(AppleDMAMapResult)) {
            return kIOReturnBadArgument;
        }

        OSData *output = OSData::withCapacity(sizeof(AppleDMAMapResult));
        if (output == nullptr) {
            return kIOReturnNoMemory;
        }

        AppleDMAMapResult result = {};
        ret = vfio_user_register_client_dma_region(
            this, arguments->scalarInput[0], arguments->scalarInput[1],
            arguments->scalarInput[2], &result);
        if (ret != kIOReturnSuccess) {
            vfio_user_dma_log("DMA registration rejected token=%#llx kr=%#x",
                              arguments->scalarInput[0], ret);
            memset(&result, 0, sizeof(result));
            result.mapping_id = arguments->scalarInput[0];
            result.status = APPLE_DMA_MAP_STATUS_ERROR;
        }

        if (!output->appendBytes(&result, sizeof(result))) {
            output->release();
            if (ret == kIOReturnSuccess &&
                vfio_user_unregister_client_dma_region(
                    this, arguments->scalarInput[0]) != kIOReturnSuccess) {
                vfio_user_dma_log("failed to roll back DMA after output failure");
                return kIOReturnStillOpen;
            }
            return kIOReturnNoMemory;
        }
        arguments->structureOutput = output;
        return kIOReturnSuccess;
    }

    case kVFIOUserPCIDriverUserClientMethodUnregisterDMARegion: {
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }

        return vfio_user_unregister_client_dma_region(this,
                                                      arguments->scalarInput[0]);
    }

    case kVFIOUserPCIDriverUserClientMethodProbeDMARegion: {
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount < 2) {
            return kIOReturnBadArgument;
        }
        if (arguments->scalarOutput == nullptr || arguments->scalarOutputCount < 1) {
            return kIOReturnBadArgument;
        }

        uint64_t probeWord = 0;
        ret = vfio_user_probe_client_dma_region(this,
                                                arguments->scalarInput[0],
                                                arguments->scalarInput[1],
                                                &probeWord);
        if (ret != kIOReturnSuccess) {
            return ret;
        }

        arguments->scalarOutput[0] = probeWord;
        arguments->scalarOutputCount = 1;
        return kIOReturnSuccess;
    }

    case kVFIOUserPCIDriverUserClientMethodConfigRead: {
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount < 2) {
            return kIOReturnBadArgument;
        }
        if (arguments->scalarOutput == nullptr || arguments->scalarOutputCount < 1) {
            return kIOReturnBadArgument;
        }

        uint64_t configValue = 0;
        ret = vfio_user_config_read(this,
                                    arguments->scalarInput[0],
                                    arguments->scalarInput[1],
                                    &configValue);
        if (ret != kIOReturnSuccess) {
            return ret;
        }

        arguments->scalarOutput[0] = configValue;
        arguments->scalarOutputCount = 1;
        return kIOReturnSuccess;
    }

    case kVFIOUserPCIDriverUserClientMethodConfigWrite: {
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount < 3) {
            return kIOReturnBadArgument;
        }

        return vfio_user_config_write(this,
                                      arguments->scalarInput[0],
                                      arguments->scalarInput[1],
                                      arguments->scalarInput[2]);
    }

    case kVFIOUserPCIDriverUserClientMethodGetBARInfo: {
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }
        if (arguments->scalarOutput == nullptr || arguments->scalarOutputCount < 3) {
            return kIOReturnBadArgument;
        }

        IOPCIDevice *pciDevice = vfio_user_get_pci_device(driverService);
        if (pciDevice == nullptr) {
            return kIOReturnUnsupported;
        }

        uint8_t barIndex = (uint8_t)arguments->scalarInput[0];
        uint8_t memoryIndex = 0;
        uint64_t barSize = 0;
        uint8_t barType = 0;
        ret = pciDevice->GetBARInfo(barIndex, &memoryIndex, &barSize,
                                     &barType);
        if (ret != kIOReturnSuccess) {
            return ret;
        }

        arguments->scalarOutput[0] = memoryIndex;
        arguments->scalarOutput[1] = barSize;
        arguments->scalarOutput[2] = barType;
        arguments->scalarOutputCount = 3;
        return kIOReturnSuccess;
    }

    case kVFIOUserPCIDriverUserClientMethodMMIORead: {
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount < 3) {
            return kIOReturnBadArgument;
        }
        if (arguments->scalarOutput == nullptr || arguments->scalarOutputCount < 1) {
            return kIOReturnBadArgument;
        }

        uint64_t mmioValue = 0;
        ret = vfio_user_mmio_read(this,
                                  arguments->scalarInput[0],
                                  arguments->scalarInput[1],
                                  arguments->scalarInput[2],
                                  &mmioValue);
        if (ret != kIOReturnSuccess) {
            return ret;
        }

        arguments->scalarOutput[0] = mmioValue;
        arguments->scalarOutputCount = 1;
        return kIOReturnSuccess;
    }

    case kVFIOUserPCIDriverUserClientMethodMMIOWrite: {
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount < 4) {
            return kIOReturnBadArgument;
        }

        return vfio_user_mmio_write(this,
                                    arguments->scalarInput[0],
                                    arguments->scalarInput[1],
                                    arguments->scalarInput[2],
                                    arguments->scalarInput[3]);
    }

    case kVFIOUserPCIDriverUserClientMethodSetupInterrupts: {
        if (arguments->scalarOutput == nullptr || arguments->scalarOutputCount < 1) {
            return kIOReturnBadArgument;
        }

        ret = vfio_user_setup_interrupts(this);
        if (ret != kIOReturnSuccess) {
            return ret;
        }

        arguments->scalarOutput[0] = ivars->numInterrupts;
        arguments->scalarOutputCount = 1;
        return kIOReturnSuccess;
    }

    case kVFIOUserPCIDriverUserClientMethodCheckInterrupt: {
        if (arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < VFIO_USER_IRQ_PENDING_WORDS) {
            return kIOReturnBadArgument;
        }

        if (ivars == nullptr ||
            !__atomic_load_n(&ivars->interruptsSetUp, __ATOMIC_ACQUIRE)) {
            return kIOReturnNotReady;
        }

        for (int i = 0; i < VFIO_USER_IRQ_PENDING_WORDS; i++) {
            arguments->scalarOutput[i] =
                __atomic_exchange_n(&ivars->irqPending[i], 0,
                                    __ATOMIC_ACQ_REL);
        }
        arguments->scalarOutputCount = VFIO_USER_IRQ_PENDING_WORDS;
        return kIOReturnSuccess;
    }

    case kVFIOUserPCIDriverUserClientMethodWaitInterrupt: {
        if (arguments->completion == nullptr) {
            return kIOReturnBadArgument;
        }

        if (ivars == nullptr ||
            !__atomic_load_n(&ivars->interruptsSetUp, __ATOMIC_ACQUIRE)) {
            return kIOReturnNotReady;
        }

        arguments->completion->retain();
        OSAction *old = __atomic_exchange_n(
            &ivars->pendingInterruptNotify,
            arguments->completion, __ATOMIC_ACQ_REL);
        if (old != nullptr) {
            old->release();
        }

        /*
         * If any bits are already pending, complete immediately so the
         * client doesn't miss interrupts that arrived before arming.
         */
        bool pending = false;
        for (int i = 0; i < VFIO_USER_IRQ_PENDING_WORDS; i++) {
            if (__atomic_load_n(&ivars->irqPending[i], __ATOMIC_ACQUIRE)) {
                pending = true;
                break;
            }
        }
        if (pending) {
            OSAction *act = __atomic_exchange_n(
                &ivars->pendingInterruptNotify, nullptr, __ATOMIC_ACQ_REL);
            if (act != nullptr) {
                AsyncCompletion(act, kIOReturnSuccess, nullptr, 0);
                act->release();
            }
        }
        return kIOReturnSuccess;
    }

    case kVFIOUserPCIDriverUserClientMethodResetDevice:
        return vfio_user_reset_device(this);

    case kVFIOUserPCIDriverUserClientMethodSetIRQMask: {
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < VFIO_USER_IRQ_PENDING_WORDS) {
            return kIOReturnBadArgument;
        }

        if (ivars == nullptr ||
            !__atomic_load_n(&ivars->interruptsSetUp, __ATOMIC_ACQUIRE)) {
            return kIOReturnNotReady;
        }

        for (int i = 0; i < VFIO_USER_IRQ_PENDING_WORDS; i++) {
            __atomic_store_n(&ivars->irqEnabled[i],
                             arguments->scalarInput[i], __ATOMIC_RELEASE);
        }
        return kIOReturnSuccess;
    }

    default:
        return kIOReturnUnsupported;
    }
}

void
IMPL(VFIOUserPCIDriverUserClient, InterruptOccurred)
{
    if (ivars == nullptr ||
        !__atomic_load_n(&ivars->interruptsSetUp, __ATOMIC_ACQUIRE)) {
        return;
    }

    uint32_t vector = UINT32_MAX;

    uint32_t *vectorRef = (uint32_t *)action->GetReference();
    if (vectorRef != nullptr) {
        vector = *vectorRef;
    }

    if (vector >= ivars->numInterrupts) {
        return;
    }

    /* Only signal if this vector is enabled by the client */
    uint32_t word = vector / 64;
    uint64_t bit = 1ULL << (vector % 64);

    if (!(__atomic_load_n(&ivars->irqEnabled[word], __ATOMIC_ACQUIRE) & bit)) {
        return;
    }

    /* Set the pending bit for this vector */
    __atomic_fetch_or(&ivars->irqPending[word], bit, __ATOMIC_RELEASE);

    /* Wake the client if it has an outstanding async wait */
    OSAction *notify = __atomic_exchange_n(
        &ivars->pendingInterruptNotify, nullptr, __ATOMIC_ACQ_REL);
    if (notify != nullptr) {
        AsyncCompletion(notify, kIOReturnSuccess, nullptr, 0);
        notify->release();
    }
}

void
IMPL(VFIOUserPCIDriverUserClient, AsyncCompletion)
{
}

kern_return_t
IMPL(VFIOUserPCIDriverUserClient, CopyClientMemoryForType)
{
    if (memory == nullptr || options == nullptr) {
        return kIOReturnBadArgument;
    }

    if (type == kVFIOUserPCIDriverUserClientMemoryTypeIRQState) {
        if (ivars == nullptr || ivars->irqSharedBuffer == nullptr) {
            return kIOReturnNotReady;
        }
        ivars->irqSharedBuffer->retain();
        *options = 0;
        *memory = ivars->irqSharedBuffer;
        return kIOReturnSuccess;
    }

    if (type == kVFIOUserPCIDriverUserClientMemoryTypeDMABuffer) {
        if (ivars == nullptr || ivars->dmaBuffer == nullptr) {
            return kIOReturnNotReady;
        }
        ivars->dmaBuffer->retain();
        *options = 0;
        *memory = ivars->dmaBuffer;
        return kIOReturnSuccess;
    }

    if (type >= kVFIOUserPCIDriverUserClientMemoryTypeBAR0 &&
        type <= kVFIOUserPCIDriverUserClientMemoryTypeBAR5) {
        if (ivars == nullptr) {
            return kIOReturnBadArgument;
        }

        uint8_t barIndex =
            (uint8_t)(type - kVFIOUserPCIDriverUserClientMemoryTypeBAR0);

        if (!vfio_user_service_claimed(this)) {
            return kIOReturnNotOpen;
        }

        if (ivars->barDescCache[barIndex] != nullptr) {
            ivars->barDescCache[barIndex]->retain();
            *options = 0;
            *memory = ivars->barDescCache[barIndex];
            return kIOReturnSuccess;
        }

        IOService *driverService = GetProvider();
        if (driverService == nullptr) {
            return kIOReturnNotAttached;
        }
        IOPCIDevice *pciDevice = vfio_user_get_pci_device(driverService);
        if (pciDevice == nullptr) {
            return kIOReturnUnsupported;
        }

        uint8_t memoryIndex = 0;
        uint64_t barSize = 0;
        uint8_t barType = 0;
        kern_return_t ret;

        ret = pciDevice->GetBARInfo(barIndex,
                                    &memoryIndex, &barSize, &barType);
        if (ret != kIOReturnSuccess) {
            os_log(OS_LOG_DEFAULT,
                   "vfio-user-dext: GetBARInfo failed for BAR%u: %#x",
                   (unsigned int)barIndex, ret);
            return ret;
        }

        IOMemoryDescriptor *barMemory = nullptr;
        ret = pciDevice->_CopyDeviceMemoryWithIndex(memoryIndex,
                                                    &barMemory, this);
        if (ret != kIOReturnSuccess || barMemory == nullptr) {
            os_log(OS_LOG_DEFAULT,
                   "vfio-user-dext: _CopyDeviceMemoryWithIndex failed BAR%u idx=%u: %#x",
                   (unsigned int)barIndex, (unsigned int)memoryIndex, ret);
            return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
        }

        barMemory->retain();
        ivars->barDescCache[barIndex] = barMemory;

        os_log(OS_LOG_DEFAULT,
               "vfio-user-dext: providing BAR%u memory descriptor size=%llu for claimed user client",
               (unsigned int)barIndex, barSize);

        *options = 0;
        *memory = barMemory;
        return kIOReturnSuccess;
    }

    return kIOReturnUnsupported;
}
