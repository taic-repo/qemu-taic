#include "hw/taic.h"
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "qemu/timer.h"
#include "exec/cpu-common.h"
#include "hw/irq.h"

static uint64_t taic_read(void *opaque, hwaddr addr, unsigned size) {
    info_report(" taic read %ld", addr);
    return 0;
}

static void taic_write(void *opaque, hwaddr addr, uint64_t value, unsigned size) {
    // TAICState* taic = opaque;
    // uint64_t op = addr % PAGE_SIZE;
    if(addr < PAGE_SIZE) {
        // alloc indices
        // switch(op) {
        // case IF_IDE:
        // case IF_SCSI:
        // case IF_XEN:
        // case IF_NONE:
        //     dinfo->media_cd = media == MEDIA_CDROM;
        //     break;
        // default:
        //     break;
        // }
    } else {
        // operations about per queue

    }


}

static void taic_irq_request(void *opaque, int irq, int level) {
}

static const MemoryRegionOps taic_ops = {
    .read = taic_read,
    .write = taic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 8,
        .max_access_size = 8
    },
    .impl = {
        .min_access_size = 8,
        .max_access_size = 8
    }
};

static void taic_realize(DeviceState *dev, Error **errp)
{
    TAICState *taic = TAIC(dev);
    info_report(" taic realize");
    memory_region_init_io(&taic->mmio, OBJECT(dev), &taic_ops, taic,
                          TYPE_TAIC, TAIC_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &taic->mmio);
    info_report("low 0x%x high 0x%x", (uint32_t)taic->mmio.addr, (uint32_t)taic->mmio.size);
    // init external_irqs
    uint32_t external_irq_count = taic->external_irq_count;
    taic->external_irqs = g_malloc(sizeof(qemu_irq) * external_irq_count);
    qdev_init_gpio_in(dev, taic_irq_request, external_irq_count);
    // init taic_hart
    uint32_t hart_count = taic->hart_count;
    // create output irqs
    taic->ssoft_irqs = g_malloc(sizeof(qemu_irq) * hart_count);
    qdev_init_gpio_out(dev, taic->ssoft_irqs, hart_count);
    taic->usoft_irqs = g_malloc(sizeof(qemu_irq) * hart_count);
    qdev_init_gpio_out(dev, taic->usoft_irqs, hart_count);
}

static Property taic_properties[] = {
    DEFINE_PROP_UINT32("hart_count", TAICState, hart_count, 0),
    DEFINE_PROP_UINT32("external_irq_count", TAICState, external_irq_count, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void taic_class_init(ObjectClass *obj, void *data) {
    DeviceClass *dc = DEVICE_CLASS(obj);
    device_class_set_props(dc, taic_properties);
    dc->realize = taic_realize;
}

static const TypeInfo taic_info = {
    .name          = TYPE_TAIC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TAICState),
    .class_init    = taic_class_init,
};

static void taic_register_types(void) {
    type_register_static(&taic_info);
}

type_init(taic_register_types)

DeviceState *taic_create(hwaddr addr, uint32_t hart_count, uint32_t external_irq_count) {
    qemu_log("create taic\n");
    DeviceState *dev = qdev_new(TYPE_TAIC);
    qdev_prop_set_uint32(dev, "hart_count", hart_count);
    qdev_prop_set_uint32(dev, "external_irq_count", external_irq_count);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, addr);
    return dev;
}