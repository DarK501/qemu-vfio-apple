#!/usr/bin/env python3
"""Keep the DriverKit PCI match scoped to the selected test GPU."""

import pathlib
import plistlib


ROOT = pathlib.Path(__file__).resolve().parents[3]
DExt = ROOT / "contrib/apple-vfio/VFIOUserPCIDriver"
EXPECTED_MATCH = "0x731F1002"


def main():
    with (DExt / "Info.plist").open("rb") as stream:
        info = plistlib.load(stream)
    personality = info["IOKitPersonalities"]["VFIOUserPCIDriver"]
    assert personality["IOPCIPrimaryMatch"] == EXPECTED_MATCH

    with (DExt / "VFIOUserPCIDriver.entitlements").open("rb") as stream:
        entitlements = plistlib.load(stream)
    assert entitlements["com.apple.developer.driverkit"] is True
    pci = entitlements["com.apple.developer.driverkit.transport.pci"]
    assert len(pci) == 1
    assert pci[0]["IOPCIPrimaryMatch"] == EXPECTED_MATCH


if __name__ == "__main__":
    main()
