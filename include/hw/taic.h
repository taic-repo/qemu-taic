/*
 * Task-Aware Interrupt Controller
 */

#ifndef HW_TAIC_H
#define HW_TAIC_H

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qom/object.h"
#include "qemu/error-report.h"
#include "qemu/queue.h"
#include "qapi/error.h"
#include "qemu/error-report.h"

#define TYPE_TAIC "taic"
#define TAIC_MMIO_BASE      0x1000000
#define TAIC_MMIO_SIZE      0x1000000
#define PAGE_SIZE           0x1000
#define GQ_NUM              4
#define LQ_NUM              2
#define INTR_NUM            6

typedef QSIMPLEQ_HEAD(, QueueEntry) QueueHead;

struct QueueEntry {
    uint64_t data;
    QSIMPLEQ_ENTRY(QueueEntry) next;
};

typedef struct {
    QueueHead head;
} Queue;

static inline void queue_init(Queue* queue) {
    QSIMPLEQ_INIT(&queue->head);
}

static inline void queue_push(Queue* queue, uint64_t data) {
    struct QueueEntry *entry = g_new0(struct QueueEntry, 1);
    entry->data = data;
    QSIMPLEQ_INSERT_TAIL(&queue->head, entry, next);
}

static inline void queue_push_head(Queue* queue, uint64_t data) {
    struct QueueEntry *entry = g_new0(struct QueueEntry, 1);
    entry->data = data;
    QSIMPLEQ_INSERT_HEAD(&queue->head, entry, next);
}

static inline uint64_t queue_pop(Queue* queue) {
    uint64_t res = 0;
    QueueHead *head = &queue->head;
    if (head->sqh_first != NULL) {
        struct QueueEntry *entry = head->sqh_first;
        res = entry->data;
        QSIMPLEQ_REMOVE_HEAD(head, next);
        g_free(entry);
    }
    return res;
}

/************ The External Interrupt Slots ************/

// 数组的每个元素表示一个 CPU 的外部中断槽
typedef struct {
    uint64_t cap;
    uint64_t state;
    uint64_t* slots;
} ExtIntrSlots;

void init_extintrslots(ExtIntrSlots* extintrslots, uint64_t size);
void register_ext(ExtIntrSlots* extintrslots, uint64_t irq, uint64_t handler);
uint64_t wakeup_ext(ExtIntrSlots* extintrslots, uint64_t irq);
void clean_extintrslots(ExtIntrSlots* extintrslots);

/************ The Soft Interrupt Slots ************/
typedef struct {
    uint64_t recv_os_id;
    uint64_t recv_proc_id;
} SendCap;

typedef struct {
    uint64_t send_os_id;
    uint64_t send_proc_id;
    uint64_t handler;
} RecvCap;

typedef struct {
    uint64_t cap;
    uint64_t state;
    uint64_t os_id;
    uint64_t proc_id;
    uint64_t task_id;
    SendCap* sendcap;
    RecvCap* recvcap;
} SoftIntrSlots;

void init_softintrslots(SoftIntrSlots* softintrslots, uint64_t size);
void register_send(SoftIntrSlots* softintrslots, uint64_t data);
void cancel_send(SoftIntrSlots* softintrslots, uint64_t data);
int64_t check_send(SoftIntrSlots* softintrslots, uint64_t recv_os_id, uint64_t recv_proc_id);
void register_recv(SoftIntrSlots* softintrslots, uint64_t data);
uint64_t wakeup_soft(SoftIntrSlots* softintrslots, uint64_t send_os_id, uint64_t send_proc_id);
void clean_softintrslots(SoftIntrSlots* softintrslots);

/************ The Global Queue ************/

typedef struct {
    bool is_used;
    Queue* ready_queue;
    uint64_t count;
} LocalQueue;

void init_local_queue(LocalQueue* local_queue);
void push_local_queue(LocalQueue* local_queue, uint64_t data, bool need_preempt);
uint64_t pop_local_queue(LocalQueue* local_queue);

typedef struct {
    uint64_t state;
    uint64_t sint_state;
    uint64_t os_id;
    uint64_t proc_id;
    int64_t hart_id;
    bool ssip;
    bool usip;
    LocalQueue* local_queue;
    ExtIntrSlots extintrslots;
    SoftIntrSlots softintrslots;
    uint64_t used_lq_count;
    int64_t sendcap_idx;
    uint64_t recv_os;
    uint64_t recv_proc;
} GlobalQueue;

void init_global_queue(GlobalQueue* global_queue);
int64_t alloc_lq(GlobalQueue* global_queue);
void free_lq(GlobalQueue* global_queue, uint64_t lq_idx);
void lq_enq(GlobalQueue* global_queue, uint64_t lq_idx, uint64_t data, bool need_preempt);
uint64_t lq_deq(GlobalQueue* global_queue, uint64_t lq_idx);
void register_ext_handler(GlobalQueue* global_queue, uint64_t irq_idx, uint64_t data);
void handle_extintr(GlobalQueue* global_queue, uint64_t irq_idx);
void register_sender(GlobalQueue* global_queue, uint64_t data);
void cancel_sender(GlobalQueue* global_queue, uint64_t data);
void register_receiver(GlobalQueue* global_queue, uint64_t data);
void check_sendcap(GlobalQueue* global_queue, uint64_t data);
void handle_softintr(GlobalQueue* global_queue, uint64_t send_os, uint64_t send_proc);
void write_hartid(GlobalQueue* global_queue, uint64_t data);

/************ The TAIC Controller ************/
enum TaicState {
    IDLE = 0,
    WOS = 1,
    RIDX = 2,
    FREE_QUEUE = 3,
    PASS_SOFT_INTR = 4,
};

typedef struct {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion mmio;
    /* the properties related to cpu and other peripherals */
    qemu_irq *external_irqs;
    uint32_t hart_count;
    uint32_t external_irq_count;
    qemu_irq* usoft_irqs;
    qemu_irq* ssoft_irqs;
    /* internal config */
    uint64_t state;
    uint64_t os_id;
    uint64_t proc_id;
    uint64_t send_os_id;
    uint64_t send_proc_id;
    int64_t alloc_idx;
    GlobalQueue* gqs;
}TAICState;

#define TYPE_TAIC "taic"
DECLARE_INSTANCE_CHECKER(TAICState, TAIC, TYPE_TAIC)

// init the internal configuration when create taic instance
static inline void taic_init(TAICState* taic) {
    taic->state = IDLE;
    taic->os_id = 0;
    taic->proc_id = 0;
    taic->send_os_id = 0;
    taic->send_proc_id = 0;
    taic->alloc_idx = 0;
    int i = 0;
    taic->gqs = g_new0(GlobalQueue, GQ_NUM);
    for(i = 0; i < GQ_NUM; i++) {
        init_global_queue(&(taic->gqs[i]));
    }
}

static inline int64_t taic_read_alloc_idx(TAICState* taic) {
    uint64_t state = 0;
    while(1) {
        state = qatomic_cmpxchg(&taic->state, RIDX, IDLE);
        if(state == RIDX) {
            int64_t res = taic->alloc_idx;
            qatomic_set(&taic->state, IDLE);
            return res;
        }
    }
}

static inline void taic_alloc_gq(TAICState* taic, uint64_t data) {
    uint64_t state = 0;
    while(1) {
        state = qatomic_cmpxchg(&taic->state, IDLE, WOS);
        if (state == IDLE) {
            taic->os_id = data;
            return;
        } else if (state == WOS) {
            taic->proc_id = data;
            int i = 0;
            uint64_t os_id = taic->os_id;
            uint64_t proc_id = data;
            int64_t idx = -1;
            for(i = GQ_NUM - 1; i >= 0; i--) {
                if(taic->gqs[i].os_id == os_id && taic->gqs[i].proc_id == proc_id) {
                    idx = i;
                    break;
                } else if (taic->gqs[i].os_id == 0 && taic->gqs[i].proc_id == 0) {
                    idx = i;
                }
            }
            if(idx != -1) {
                taic->gqs[idx].os_id = os_id;
                taic->gqs[idx].proc_id = proc_id;
            } else {
                error_report("No global queue slots");
                taic->alloc_idx = -1;
                taic->alloc_idx = -1;
                qatomic_set(&taic->state, RIDX);
                return;
            }
            // 分配好全局队列，分配局部队列
            int64_t lq_idx = alloc_lq(&(taic->gqs[idx]));
            if(lq_idx == -1) {
                error_report("No local queue slots");
                taic->alloc_idx = -1;
                qatomic_set(&taic->state, RIDX);
                return;
            }
            taic->alloc_idx = ((idx & 0xffffffff) << 32) | (lq_idx & 0xffffffff);
            qatomic_set(&taic->state, RIDX);
            return;
        }
    }
}

static inline void taic_free_gq(TAICState* taic, uint64_t idx) {
    uint64_t gq_idx = (idx >> 32) & 0xffffffff;
    uint64_t lq_idx = idx & 0xffffffff;
    uint64_t state = 0;
    while(1) {
        state = qatomic_cmpxchg(&taic->state, IDLE, FREE_QUEUE);
        if(state == IDLE) {
            free_lq(&(taic->gqs[gq_idx]), lq_idx);
            qatomic_set(&taic->state, IDLE);
            return;
        }
    }
}

static inline void taic_lq_enq(TAICState* taic, uint64_t gq_idx, uint64_t lq_idx, uint64_t data) {
    if(gq_idx >= GQ_NUM) {
        error_report("Invalid gq_idx");
        return;
    }
    if(taic->gqs[gq_idx].os_id == 0 && taic->gqs[gq_idx].proc_id == 0) {
        error_report("Not used GQ");
        return;
    }
    lq_enq(&(taic->gqs[gq_idx]), lq_idx, data, false);
}

static inline uint64_t taic_lq_deq(TAICState* taic, uint64_t gq_idx, uint64_t lq_idx) {
    if(gq_idx >= GQ_NUM) {
        error_report("Invalid gq_idx");
        return 0;
    }
    if(taic->gqs[gq_idx].os_id == 0 && taic->gqs[gq_idx].proc_id == 0) {
        error_report("Not used GQ");
        return 0;
    }
    return lq_deq(&(taic->gqs[gq_idx]), lq_idx);
}

static inline void taic_register_ext(TAICState* taic, uint64_t gq_idx, uint64_t irq_idx, uint64_t data) {
    if(gq_idx >= GQ_NUM) {
        error_report("Invalid gq_idx");
        return;
    }
    if(taic->gqs[gq_idx].os_id == 0 && taic->gqs[gq_idx].proc_id == 0) {
        error_report("Not used GQ");
        return;
    }
    register_ext_handler(&(taic->gqs[gq_idx]), irq_idx, data);
}

static inline void taic_sim_extintr(TAICState* taic, uint64_t irq_idx) {
    for(int i = 0; i < GQ_NUM; i++) {
        handle_extintr(&(taic->gqs[i]), irq_idx);
    }
}

static inline void taic_register_sender(TAICState* taic, uint64_t gq_idx, uint64_t data) {
    if(gq_idx >= GQ_NUM) {
        error_report("Invalid gq_idx");
        return;
    }
    if(taic->gqs[gq_idx].os_id == 0 && taic->gqs[gq_idx].proc_id == 0) {
        error_report("Not used GQ");
        return;
    }
    register_sender(&(taic->gqs[gq_idx]), data);
}

static inline void taic_cancel_sender(TAICState* taic, uint64_t gq_idx, uint64_t data) {
    if(gq_idx >= GQ_NUM) {
        error_report("Invalid gq_idx");
        return;
    }
    if(taic->gqs[gq_idx].os_id == 0 && taic->gqs[gq_idx].proc_id == 0) {
        error_report("Not used GQ");
        return;
    }
    cancel_sender(&(taic->gqs[gq_idx]), data);
}

static inline void taic_register_receiver(TAICState* taic, uint64_t gq_idx, uint64_t data) {
    if(gq_idx >= GQ_NUM) {
        error_report("Invalid gq_idx");
        return;
    }
    if(taic->gqs[gq_idx].os_id == 0 && taic->gqs[gq_idx].proc_id == 0) {
        error_report("Not used GQ");
        return;
    }
    register_receiver(&(taic->gqs[gq_idx]), data);
}

static inline void taic_send_softintr(TAICState* taic, uint64_t gq_idx, uint64_t data) {
    if(gq_idx >= GQ_NUM) {
        error_report("Invalid gq_idx");
        return;
    }
    if(taic->gqs[gq_idx].os_id == 0 && taic->gqs[gq_idx].proc_id == 0) {
        error_report("Not used GQ");
        return;
    }
    uint64_t state = 0;
    while (1) {
        state = qatomic_cmpxchg(&taic->state, IDLE, PASS_SOFT_INTR);
        if(state == IDLE) {
            check_sendcap(&(taic->gqs[gq_idx]), data);
            return;
        } else if(state == PASS_SOFT_INTR) {
            check_sendcap(&(taic->gqs[gq_idx]), data);
            if(taic->gqs[gq_idx].sendcap_idx != -1) {   // 有发送能力，检查接收方的能力
                uint64_t recv_os = taic->gqs[gq_idx].recv_os;
                uint64_t recv_proc = taic->gqs[gq_idx].recv_proc;
                uint64_t send_os = taic->gqs[gq_idx].os_id;
                uint64_t send_proc = taic->gqs[gq_idx].proc_id;
                // 找到对应的接收方的全局队列
                int idx = -1;
                for(int i = 0; i < GQ_NUM; i++) {
                    if(taic->gqs[i].os_id == recv_os && taic->gqs[i].proc_id == recv_proc) {
                        idx = i;
                        break;
                    }
                }
                if(idx != -1) {     // 找到了对应的接收方的全局队列，处理中断
                    handle_softintr(&(taic->gqs[idx]), send_os, send_proc);
                }
            }
            qatomic_set(&taic->state, IDLE);
            return;
        }
    }
}

static inline void taic_write_hartid(TAICState* taic, uint64_t gq_idx, uint64_t data) {
    if(gq_idx >= GQ_NUM) {
        error_report("Invalid gq_idx");
        return;
    }
    if(taic->gqs[gq_idx].os_id == 0 && taic->gqs[gq_idx].proc_id == 0) {
        error_report("Not used GQ");
        return;
    }
    write_hartid(&(taic->gqs[gq_idx]), data);
}

DeviceState *taic_create(hwaddr addr, uint32_t hart_count, uint32_t external_irq_count);


#endif
