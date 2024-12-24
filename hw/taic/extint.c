#include "hw/taic.h"

enum ExtIntrState {
    EINT_IDLE = 0,
    REG_EXT = 1,
    WAKEUP_EXT = 2,
    CLEAN_EXT = 3,
};

void init_extintrslots(ExtIntrSlots* extintrslots, uint64_t size) {
    extintrslots->cap = size;
    extintrslots->state = 0;
    extintrslots->slots = g_new0(uint64_t, size);
}

void register_ext(ExtIntrSlots* extintrslots, uint64_t irq, uint64_t handler) {
    if(irq >= extintrslots->cap) {
        error_report("The irq is out of range");
        return;
    }
    uint64_t state = 0;
    while (1) {
        /* code */
        state = qatomic_cmpxchg(&extintrslots->state, EINT_IDLE, REG_EXT);
        if(state == EINT_IDLE) {
            extintrslots->slots[irq] = handler;
            qatomic_set(&extintrslots->state, EINT_IDLE);
            return;
        }
    }
}

uint64_t wakeup_ext(ExtIntrSlots* extintrslots, uint64_t irq) {
    if(irq >= extintrslots->cap) {
        error_report("The irq is out of range");
        return 0;
    }
    uint64_t state = 0;
    while (1) {
        /* code */
        state = qatomic_cmpxchg(&extintrslots->state, EINT_IDLE, WAKEUP_EXT);
        if(state == EINT_IDLE) {
            int64_t res = extintrslots->slots[irq];
            extintrslots->slots[irq] = 0;
            qatomic_set(&extintrslots->state, EINT_IDLE);
            return res;
        }
    }
}

void clean_extintrslots(ExtIntrSlots* extintrslots) {
    uint64_t state = 0;
    while (1) {
        /* code */
        state = qatomic_cmpxchg(&extintrslots->state, EINT_IDLE, CLEAN_EXT);
        if(state == EINT_IDLE) {
            for (uint64_t i = 0; i < extintrslots->cap; i++) {
                extintrslots->slots[i] = 0;
            }
            qatomic_set(&extintrslots->state, EINT_IDLE);
            return;
        }
    }
}