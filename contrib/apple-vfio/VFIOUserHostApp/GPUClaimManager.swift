import Combine
import Foundation
import IOKit

nonisolated struct ClaimGPU: Identifiable, Sendable {
    let id: UInt64
    let bus: UInt64
    let device: UInt64
    let function: UInt64
    let vendor: UInt64
    let product: UInt64

    var label: String {
        String(format: "%02llx:%02llx.%llx %04llx:%04llx",
               bus, device, function, vendor, product)
    }

    var identity: [UInt64] { [bus, device, function, vendor, product] }
}

nonisolated private enum GPUClaimError: LocalizedError {
    case message(String)
    case closeFailed(io_connect_t, String)
    var errorDescription: String? {
        switch self {
        case .message(let text), .closeFailed(_, let text): return text
        }
    }
}

nonisolated private func errorCode(_ kr: kern_return_t) -> String {
    String(format: "0x%08x", UInt32(bitPattern: kr))
}

nonisolated private func properties(_ entry: io_registry_entry_t) throws -> [String: Any] {
    var raw: Unmanaged<CFMutableDictionary>?
    let kr = IORegistryEntryCreateCFProperties(entry, &raw, kCFAllocatorDefault, 0)
    guard kr == KERN_SUCCESS, let result = raw?.takeRetainedValue() as? [String: Any] else {
        throw GPUClaimError.message("Registry properties unavailable (\(errorCode(kr))).")
    }
    return result
}

nonisolated private func pciNumber(_ property: Any?, bytes: Int) -> UInt64? {
    guard let data = property as? Data, data.count >= bytes else { return nil }
    return data.prefix(bytes).enumerated().reduce(UInt64(0)) {
        $0 | (UInt64($1.element) << (8 * $1.offset))
    }
}

nonisolated private func gpuIdentity(_ entry: io_service_t) throws -> ClaimGPU? {
    let dict = try properties(entry)
    guard let classCode = pciNumber(dict["class-code"], bytes: 3),
          classCode >> 16 == 3,
          let vendor = pciNumber(dict["vendor-id"], bytes: 2), vendor < 0xffff,
          let product = pciNumber(dict["device-id"], bytes: 2), product < 0xffff,
          let debug = dict["pcidebug"] as? String else { return nil }
    let parts = debug.prefix { $0 != "(" }.split(separator: ":")
    guard parts.count == 3, let bus = UInt64(parts[0]), bus <= 255,
          let device = UInt64(parts[1]), device <= 31,
          let function = UInt64(parts[2]), function <= 7 else { return nil }
    var id: UInt64 = 0
    let kr = IORegistryEntryGetRegistryEntryID(entry, &id)
    guard kr == KERN_SUCCESS else {
        throw GPUClaimError.message("Registry identity unavailable (\(errorCode(kr))).")
    }
    return ClaimGPU(id: id, bus: bus, device: device, function: function,
                    vendor: vendor, product: product)
}

nonisolated private func enumerateGPUs() throws -> [ClaimGPU] {
    guard let matching = IOServiceMatching("IOPCIDevice") else {
        throw GPUClaimError.message("Unable to create PCI matching dictionary.")
    }
    var iterator: io_iterator_t = 0
    let kr = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator)
    guard kr == KERN_SUCCESS else {
        throw GPUClaimError.message("PCI enumeration failed (\(errorCode(kr))).")
    }
    defer { IOObjectRelease(iterator) }
    var result: [ClaimGPU] = []
    var entry = IOIteratorNext(iterator)
    while entry != 0 {
        defer { IOObjectRelease(entry); entry = IOIteratorNext(iterator) }
        if let gpu = try gpuIdentity(entry) { result.append(gpu) }
    }
    return result.sorted { $0.label < $1.label }
}

nonisolated private func findGPU(_ target: ClaimGPU) throws -> io_service_t {
    guard let matching = IOServiceMatching("IOPCIDevice") else {
        throw GPUClaimError.message("Unable to create PCI matching dictionary.")
    }
    var iterator: io_iterator_t = 0
    let kr = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator)
    guard kr == KERN_SUCCESS else {
        throw GPUClaimError.message("PCI enumeration failed (\(errorCode(kr))).")
    }
    defer { IOObjectRelease(iterator) }
    var selected: io_service_t = 0
    defer { if selected != 0 { IOObjectRelease(selected) } }
    var entry = IOIteratorNext(iterator)
    while entry != 0 {
        defer { IOObjectRelease(entry); entry = IOIteratorNext(iterator) }
        var id: UInt64 = 0
        let idResult = IORegistryEntryGetRegistryEntryID(entry, &id)
        guard idResult == KERN_SUCCESS else {
            throw GPUClaimError.message("Registry identity lookup failed (\(errorCode(idResult))).")
        }
        if id == target.id {
            guard let current = try gpuIdentity(entry),
                  current.identity == target.identity else {
                throw GPUClaimError.message("Selected GPU identity changed.")
            }
            IOObjectRetain(entry)
            selected = entry
        }
    }
    guard selected != 0 else {
        throw GPUClaimError.message("Selected GPU is no longer present.")
    }
    let result = selected
    selected = 0
    return result
}

nonisolated private func vfioChild(_ pci: io_service_t) throws -> io_service_t? {
    var iterator: io_iterator_t = 0
    let kr = IORegistryEntryGetChildIterator(pci, kIOServicePlane, &iterator)
    guard kr == KERN_SUCCESS else {
        throw GPUClaimError.message("PCI child lookup failed (\(errorCode(kr))).")
    }
    defer { IOObjectRelease(iterator) }
    var selected: io_service_t = 0
    var child = IOIteratorNext(iterator)
    while child != 0 {
        defer { IOObjectRelease(child); child = IOIteratorNext(iterator) }
        let value = IORegistryEntryCreateCFProperty(child, "IOUserClass" as CFString,
                                                    kCFAllocatorDefault, 0)
        if let cls = value?.takeRetainedValue() as? String,
           cls == "VFIOUserPCIDriver" {
            if selected != 0 {
                IOObjectRelease(selected)
                throw GPUClaimError.message("Multiple VFIO services matched the selected GPU; claim aborted.")
            }
            IOObjectRetain(child)
            selected = child
        }
    }
    return selected == 0 ? nil : selected
}

nonisolated private func claimGPU(_ target: ClaimGPU) throws -> (io_connect_t, String) {
    let pci = try findGPU(target)
    defer { IOObjectRelease(pci) }
    var service = try vfioChild(pci)
    if service == nil {
        NSLog("vfio-gpu-claim: requesting PCI rescan for %@", target.label)
        let kr = IOServiceRequestProbe(pci, 0x80200000)
        guard kr == KERN_SUCCESS else {
            throw GPUClaimError.message("PCI rescan failed (\(errorCode(kr)); no PCI Open attempted).")
        }
        NSLog("vfio-gpu-claim: PCI rescan request returned success for %@", target.label)
        let deadline = ProcessInfo.processInfo.systemUptime + 5
        repeat {
            Thread.sleep(forTimeInterval: 0.05)
            service = try vfioChild(pci)
        } while service == nil && ProcessInfo.processInfo.systemUptime < deadline
    }
    guard let vfio = service else {
        throw GPUClaimError.message("No VFIO service matched after rescan; PCI Open was not attempted.")
    }
    defer { IOObjectRelease(vfio) }
    var connection: io_connect_t = 0
    let openResult = IOServiceOpen(vfio, mach_task_self_, 0, &connection)
    guard openResult == KERN_SUCCESS else {
        throw GPUClaimError.message("Opening VFIO user client failed (\(errorCode(openResult))).")
    }
    var input = target.identity
    let inputCount = UInt32(input.count)
    var output = [UInt64](repeating: 0, count: 3)
    var count: UInt32 = 3
    NSLog("vfio-gpu-claim: attempting retained PCI Open for %@", target.label)
    let result = IOConnectCallScalarMethod(connection, 19, &input, inputCount,
                                           &output, &count)
    guard result == KERN_SUCCESS, count == 3, output[0] == target.vendor,
          output[1] == target.product, (output[2] & 0x06) == 0x06 else {
        let closeResult = IOServiceClose(connection)
        let detail = "Claim failed or returned invalid output " +
            "(\(errorCode(result)), output count \(count))."
        if closeResult != KERN_SUCCESS {
            throw GPUClaimError.closeFailed(connection, detail +
                " User-client close also failed (\(errorCode(closeResult))); claim state is uncertain.")
        }
        throw GPUClaimError.message(detail)
    }
    return (connection, "PCI Open retained; memory space and bus mastering enabled.")
}

final class GPUClaimManager: ObservableObject {
    @Published var devices: [ClaimGPU] = []
    @Published var selectedID: UInt64?
    @Published var busy = false
    @Published var held = false
    @Published var message = "Refresh to find display devices."
    private var connection: io_connect_t = 0

    deinit {
        if connection != 0 {
            let kr = IOServiceClose(connection)
            NSLog("vfio-gpu-claim: closing user client during teardown returned %@",
                  errorCode(kr))
        }
    }

    func refresh() {
        guard !busy, !held else { return }
        do {
            devices = try enumerateGPUs()
            if !devices.contains(where: { $0.id == selectedID }) {
                selectedID = devices.first?.id
            }
            message = devices.isEmpty ? "No display devices found." : "Select a GPU to claim."
        } catch {
            devices = []
            selectedID = nil
            message = error.localizedDescription
            NSLog("vfio-gpu-claim: refresh failed: %@", message)
        }
    }

    func claim() {
        guard !busy, !held, let target = devices.first(where: { $0.id == selectedID }) else { return }
        busy = true
        message = "Requesting PCI Open for \(target.label)..."
        NSLog("vfio-gpu-claim: %@", message)
        DispatchQueue.global(qos: .userInitiated).async {
            let result = Result { try claimGPU(target) }
            DispatchQueue.main.async {
                self.busy = false
                switch result {
                case .success(let (connection, detail)):
                    self.connection = connection
                    self.held = true
                    self.message = "\(target.label): \(detail) Release before starting QEMU."
                case .failure(let error):
                    if let claimError = error as? GPUClaimError,
                       case .closeFailed(let connection, _) = claimError {
                        self.connection = connection
                        self.held = true
                    }
                    self.message = "\(target.label): \(error.localizedDescription)"
                }
                NSLog("vfio-gpu-claim: %@", self.message)
            }
        }
    }

    func release() {
        guard !busy, held else { return }
        busy = true
        let currentConnection = connection
        DispatchQueue.global(qos: .userInitiated).async {
            let releaseResult = IOConnectCallScalarMethod(
                currentConnection, 20, nil, 0, nil, nil)
            let closeResult = releaseResult == KERN_SUCCESS
                ? IOServiceClose(currentConnection) : releaseResult
            DispatchQueue.main.async {
                self.busy = false
                if releaseResult != KERN_SUCCESS {
                    self.message = "GPU release failed (\(errorCode(releaseResult))); bus mastering may still be enabled. Claim retained."
                } else if closeResult != KERN_SUCCESS {
                    self.message = "Bus mastering was verified off, but user-client close failed (\(errorCode(closeResult))). Connection retained; retry Release GPU."
                } else {
                    self.connection = 0
                    self.held = false
                    self.message = "GPU claim released; bus mastering is off. The dext retains its Start-time PCI Open."
                }
                NSLog("vfio-gpu-claim: %@", self.message)
            }
        }
    }
}
