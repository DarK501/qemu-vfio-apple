enum DriverState {
    case ours([String])
    case other([String])
    case none
    case unknown
}

/// Registry evidence of AMD graphics services, not proof of an open owner.
func hasExternalAMDGraphicsServices(classCode: UInt32, tunnelled: Bool,
                                   driver: DriverState) -> Bool {
    guard tunnelled, (classCode >> 16) & 0xff == 0x03 else { return false }
    let names: [String]
    switch driver {
    case .ours(let other), .other(let other): names = other
    case .none, .unknown: return false
    }
    return names.contains { $0 == "AMDSupport" || $0.hasPrefix("AMDRadeon") }
}

func externalGraphicsAdvisory(bdf: String) -> [String] {
    return [
        "External GPU \(bdf): AMD graphics services are attached; macOS may still be using it.",
        "If safe, close apps using the eGPU and use the menu bar eGPU disconnect action; then rerun list-devices.",
        "Disconnect may remove the PCI device entirely, not release it for VFIO. It does not prove exclusive access or DMA safety."
    ]
}
