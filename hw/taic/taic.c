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
#include "target/riscv/cpu.h"

static uint64_t taic_read(void *opaque, hwaddr addr, unsigned size) {
    TAICState* taic = opaque;
    bool is_ctl = addr < PAGE_SIZE;
    uint64_t op = addr % PAGE_SIZE;
    uint64_t idx = (addr / PAGE_SIZE) - 1;
    uint64_t gq_idx = idx / LQ_NUM;
    uint64_t lq_idx = idx % LQ_NUM;
    if(is_ctl) {
        if(op == 0x0) {
            return taic_read_alloc_idx(taic);
        }
    } else {
        if(op == 0x08) { // deq
            return taic_lq_deq(taic, gq_idx, lq_idx);
        } else if(op == 0x10) { // read_error
            // TODO
        } else {
            error_report("Invalid MMIO read");
        }
    }
    return 0;
}

static void taic_write(void *opaque, hwaddr addr, uint64_t value, unsigned size) {
    TAICState* taic = opaque;
    bool is_ctl = addr < PAGE_SIZE;
    uint64_t op = addr % PAGE_SIZE;
    uint64_t idx = (addr / PAGE_SIZE) - 1;
    uint64_t gq_idx = idx / LQ_NUM;
    uint64_t lq_idx = idx % LQ_NUM;
    if(is_ctl) {
        if(op == 0x0) {
            // 申请全局队列
            taic_alloc_gq(taic, value);
        } else if(op == 0x8) {
            // 释放局部队列
            taic_free_gq(taic, value);
        } else if(op >= 0x10 && op < 0x10 + 0x08 * INTR_NUM) {
            // 模拟产生设备中断
            uint64_t irq_idx = (op - 0x10) / 0x08;
            taic_sim_extintr(taic, irq_idx);
        }
    } else {
        // operations about per queue
        if(op == 0x0) {         // enq
            taic_lq_enq(taic, gq_idx, lq_idx, value);
        } else if(op == 0x18) { // register sender
            taic_register_sender(taic, gq_idx, value);
        } else if(op == 0x20) { // cancel sender
            taic_cancel_sender(taic, gq_idx, value);
        } else if(op == 0x28) { // register receiver
            taic_register_receiver(taic, gq_idx, value);
        } else if(op == 0x30) { // send softintr
            taic_send_softintr(taic, gq_idx, value);
        } else if(op == 0x38) { // write hartid
            taic_write_hartid(taic, gq_idx, value);
        } else {                // register external intr
            if(op >= 0x40 && op < 0x40 + 0x08 * INTR_NUM) {
                uint64_t irq_idx = (op - 0x40) / 0x08;
                taic_register_ext(taic, gq_idx, irq_idx, value);
            } else {
                error_report("Invalid MMIO write");
            }
        }
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
    taic_init(taic);
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
    int i = 0;
    for(i = 0; i < hart_count; i++) {
        RISCVCPU *cpu = RISCV_CPU(qemu_get_cpu(i));
        /* Claim software interrupt bits */
        if (riscv_cpu_claim_interrupts(cpu, MIP_USIP) < 0) {
            error_report("USIP already claimed");
            exit(1);
        }
        if (riscv_cpu_claim_interrupts(cpu, MIP_SSIP) < 0) {
            error_report("SSIP already claimed");
            exit(1);
        }
    }
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
    int i = 0;
    for (i = 0; i < hart_count; i++) {
        CPUState *cpu = qemu_get_cpu(i);
        qdev_connect_gpio_out(dev, i, qdev_get_gpio_in(DEVICE(cpu), IRQ_S_SOFT));
        qdev_connect_gpio_out(dev, i + hart_count, qdev_get_gpio_in(DEVICE(cpu), IRQ_U_SOFT));
    }
    return dev;
}