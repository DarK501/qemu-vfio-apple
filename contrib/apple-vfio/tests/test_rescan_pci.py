#!/usr/bin/env python3
"""Exercise the PCI rescan guard and request using mocked IOKit."""

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-vfio/scripts/rescan-pci.c"

CF_HEADER = r"""
#include <stddef.h>
typedef const void *CFTypeRef;
typedef const char *CFStringRef;
typedef const void *CFDataRef;
typedef void *CFMutableDictionaryRef;
typedef unsigned char UInt8;
typedef long CFIndex;
#define CFSTR(s) (s)
#define kCFAllocatorDefault NULL
#define kCFStringEncodingUTF8 0
unsigned long CFGetTypeID(CFTypeRef);
unsigned long CFStringGetTypeID(void);
unsigned long CFDataGetTypeID(void);
int CFStringGetCString(CFStringRef, char *, CFIndex, unsigned int);
CFIndex CFDataGetLength(CFDataRef);
const UInt8 *CFDataGetBytePtr(CFDataRef);
void CFRelease(CFTypeRef);
"""

IO_HEADER = r"""
#include <CoreFoundation/CoreFoundation.h>
typedef unsigned int io_service_t;
typedef unsigned int io_object_t;
typedef unsigned int io_iterator_t;
typedef int kern_return_t;
#define IO_OBJECT_NULL 0
#define KERN_SUCCESS 0
#define kIOMainPortDefault 0
#define kIOServicePlane "IOService"
CFMutableDictionaryRef IOServiceMatching(const char *);
kern_return_t IOServiceGetMatchingServices(int, CFMutableDictionaryRef, io_iterator_t *);
io_object_t IOIteratorNext(io_iterator_t);
kern_return_t IOObjectRelease(io_object_t);
CFTypeRef IORegistryEntryCreateCFProperty(io_service_t, CFStringRef, void *, int);
kern_return_t IORegistryEntryGetChildIterator(io_service_t, const char *, io_iterator_t *);
kern_return_t IOServiceRequestProbe(io_service_t, unsigned int);
"""

HARNESS = r"""
#include <assert.h>
#include <string.h>
#define main rescan_main
#include "rescan-pci.c"
#undef main

static int device_count = 1, index_value, has_child, identity_bad;
static int child_error, scan_error, scans;
static unsigned int last_options;
static const char *debug_value = "68:0:0";
static unsigned char vendor_data[2] = {0x02, 0x10};
static unsigned char product_data[2] = {0x1f, 0x73};
unsigned long CFGetTypeID(CFTypeRef p) { return p == vendor_data || p == product_data ? 2 : 1; }
unsigned long CFStringGetTypeID(void) { return 1; }
unsigned long CFDataGetTypeID(void) { return 2; }
int CFStringGetCString(CFStringRef s, char *out, CFIndex n, unsigned int encoding) {
    (void)encoding;
    if (strlen(s) >= (size_t)n) return 0;
    strcpy(out, s); return 1;
}
CFIndex CFDataGetLength(CFDataRef p) { (void)p; return 2; }
const UInt8 *CFDataGetBytePtr(CFDataRef p) { return p; }
void CFRelease(CFTypeRef p) { (void)p; }
CFMutableDictionaryRef IOServiceMatching(const char *s) { (void)s; return (void *)1; }
kern_return_t IOServiceGetMatchingServices(int port, CFMutableDictionaryRef m, io_iterator_t *it) {
    (void)port; (void)m; index_value = 0; *it = 10; return 0;
}
io_object_t IOIteratorNext(io_iterator_t it) {
    if (it == 10) return index_value++ < device_count ? 1 : 0;
    if (it == 11 && has_child) { has_child = 0; return 2; }
    return 0;
}
kern_return_t IOObjectRelease(io_object_t o) { (void)o; return 0; }
CFTypeRef IORegistryEntryCreateCFProperty(io_service_t s, CFStringRef key, void *a, int options) {
    (void)s; (void)a; (void)options;
    if (!strcmp(key, "pcidebug")) return debug_value;
    if (identity_bad) return NULL;
    if (!strcmp(key, "vendor-id")) return vendor_data;
    if (!strcmp(key, "device-id")) return product_data;
    return NULL;
}
kern_return_t IORegistryEntryGetChildIterator(io_service_t s, const char *plane, io_iterator_t *it) {
    (void)s; (void)plane; *it = 11; return child_error;
}
kern_return_t IOServiceRequestProbe(io_service_t s, unsigned int opts) {
    assert(s == 1); scans++; last_options = opts; return scan_error;
}

int main(void) {
    char *args[] = {"rescan", "44:00.0", "1002:731f", "--confirm-rescan"};
    assert(rescan_main(4, args) == 0 && scans == 1);
    assert(last_options == (0x00200000u | 0x80000000u));
    device_count = 2;
    assert(rescan_main(4, args) == 1 && scans == 1);
    device_count = 0;
    assert(rescan_main(4, args) == 1 && scans == 1);
    device_count = 1; identity_bad = 1;
    assert(rescan_main(4, args) == 1 && scans == 1);
    identity_bad = 0; has_child = 1;
    assert(rescan_main(4, args) == 1 && scans == 1);
    child_error = 1;
    assert(rescan_main(4, args) == 1 && scans == 1);
    child_error = 0; scan_error = 42;
    assert(rescan_main(4, args) == 1 && scans == 2);
    scan_error = 0; debug_value = "68:0:0(0:1)";
    assert(rescan_main(4, args) == 0 && scans == 3);
    debug_value = "68:0:0garbage";
    assert(rescan_main(4, args) == 1 && scans == 3);
    args[3] = "--wrong";
    assert(rescan_main(4, args) == 2 && scans == 3);
    args[3] = "--confirm-rescan"; args[1] = "44:20.0";
    assert(rescan_main(4, args) == 2 && scans == 3);
    return 0;
}
"""


def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        (root / "CoreFoundation").mkdir()
        (root / "IOKit").mkdir()
        (root / "CoreFoundation/CoreFoundation.h").write_text(
            "#pragma once\n" + CF_HEADER)
        (root / "IOKit/IOKitLib.h").write_text(IO_HEADER)
        (root / "test.c").write_text(HARNESS)
        binary = root / "test"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(root), "-I", str(SOURCE.parent),
                        str(root / "test.c"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
