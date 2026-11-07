#include "qemu/osdep.h"

#include <linux/vfio.h>

#include "apple-dext-client.h"
#include "hw/vfio/apple-host-backend.h"
#include "hw/vfio/apple.h"
#include "hw/vfio/vfio-device.h"
#include "hw/vfio/vfio-listener.h"
#include "qapi/error.h"
#include "system/ramblock.h"

static AppleVFIOContainer *apple_vfio_container_connect(AddressSpace *as,
                                                        VFIODevice *vbasedev,
                                                        Error **errp)
{
    VFIOPCIDevice *vdev = VFIO_PCI_DEVICE(vbasedev->dev);
    VFIOApplePCIDevice *adev = VFIO_APPLE_PCI(vbasedev->dev);
    AppleVFIOContainer *container;
    VFIOContainer *bcontainer;
    VFIOAddressSpace *space;
    VFIOIOMMUClass *vioc;
    const AppleVFIOHostBackend *backend;
    const AppleVFIOHostBackendOps *ops;
    int ret;
    char *connect_err = NULL;

    space = vfio_address_space_get(as);
    container = VFIO_IOMMU_APPLE(object_new(TYPE_VFIO_IOMMU_APPLE));
    bcontainer = VFIO_IOMMU(container);
    vioc = VFIO_IOMMU_GET_CLASS(bcontainer);
    backend = apple_vfio_host_backend();
    ops = backend ? backend->ops : NULL;

    container->host_bus = vdev->host.bus;
    container->host_device = vdev->host.slot;
    container->host_function = vdev->host.function;
    container->host_root = (adev->host_root && adev->host_root[0])
                               ? g_strdup(adev->host_root)
                               : NULL;

    ret = ram_block_uncoordinated_discard_disable(true);
    if (ret) {
        error_setg_errno(errp, -ret, "Cannot set discarding of RAM broken");
        goto fail_unref;
    }

    if (!ops || !ops->connect_with_root) {
        error_setg(errp,
                   "vfio-apple: no Darwin host backend available for PCI %02x:%02x.%x",
                   container->host_bus, container->host_device,
                   container->host_function);
        goto fail_discards;
    }

    container->dext_conn = ops->connect_with_root(container->host_bus,
                                                 container->host_device,
                                                 container->host_function,
                                                 container->host_root,
                                                 &connect_err);
    if (container->dext_conn == IO_OBJECT_NULL) {
        if (connect_err) {
            error_setg(errp, "vfio-apple: %s", connect_err);
            free(connect_err);
        } else {
            error_setg(errp,
                       "vfio-apple: could not connect to dext for host PCI "
                       "%02x:%02x.%x",
                       container->host_bus, container->host_device,
                       container->host_function);
        }
        goto fail_discards;
    }

    if (ops->claim(container->dext_conn) != KERN_SUCCESS) {
        error_setg(errp,
                   "vfio-apple: failed to claim dext-backed PCI device "
                   "%02x:%02x.%x",
                   container->host_bus, container->host_device,
                   container->host_function);
        goto fail_release_conn;
    }

    if (!apple_vfio_dext_publish(container->host_bus, container->host_device,
                                container->host_function,
                                container->host_root,
                                container->dext_conn)) {
        error_setg(errp,
                   "vfio-apple: duplicate dext owner for host PCI %02x:%02x.%x"
                   "%s%s",
                   container->host_bus, container->host_device,
                   container->host_function,
                   container->host_root ? "@" : "",
                   container->host_root ? container->host_root : "");
        goto fail_release_conn;
    }

    if (!vioc->setup(bcontainer, errp)) {
        goto fail_shared_conn;
    }

    vfio_address_space_insert(space, bcontainer);

    if (!vfio_listener_register(bcontainer, errp)) {
        goto fail_address_space;
    }

    bcontainer->initialized = true;
    return container;

fail_address_space:
    vfio_listener_unregister(bcontainer);
    QLIST_REMOVE(bcontainer, next);
    bcontainer->space = NULL;
fail_shared_conn:
    apple_vfio_dext_release(container->host_bus, container->host_device,
                            container->host_function, container->host_root,
                            container->dext_conn);
    container->dext_conn = IO_OBJECT_NULL;
fail_discards:
    ram_block_uncoordinated_discard_disable(false);
fail_unref:
    g_clear_pointer(&container->host_root, g_free);
    object_unref(container);
    vfio_address_space_put(space);
    return NULL;

fail_release_conn:
    ops->disconnect(container->dext_conn);
    container->dext_conn = IO_OBJECT_NULL;
    goto fail_discards;
}

static void apple_vfio_container_disconnect(AppleVFIOContainer *container)
{
    VFIOContainer *bcontainer = VFIO_IOMMU(container);
    VFIOAddressSpace *space = bcontainer->space;
    const AppleVFIOHostBackend *backend = apple_vfio_host_backend();
    const AppleVFIOHostBackendOps *ops = backend ? backend->ops : NULL;

    ram_block_uncoordinated_discard_disable(false);
    vfio_listener_unregister(bcontainer);

    apple_vfio_dext_release(container->host_bus, container->host_device,
                            container->host_function, container->host_root,
                            container->dext_conn);
    if (ops && ops->disconnect) {
        ops->disconnect(container->dext_conn);
    }
    container->dext_conn = IO_OBJECT_NULL;
    g_clear_pointer(&container->host_root, g_free);

    object_unref(container);
    vfio_address_space_put(space);
}
