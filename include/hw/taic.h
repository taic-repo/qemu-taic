/*
 * Task-Aware Interrupt Controller
 */

#ifndef HW_TAIC_H
#define HW_TAIC_H

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qom/object.h"
#include "qemu/error-report.h"

#define TYPE_TAIC "taic"
#define INDEX_NUM 32        // assuem that the maximum number of CPUs is 32
#define TASKS_NUM 256       // the number of tasks per queue
#define TAIC_MMIO_SIZE    0x1000000
#define PAGE_SIZE          0x1000
// #define 

static QemuMutex task_lock;
static QemuMutex indices_lock;


/************ The task identifier ************/
// ready: 0
// running: 1
// blocking: 2
// blocked: 3
typedef struct {
    uint64_t ptr : 56;
    uint64_t priority: 5;
    uint64_t state: 2;
    bool preempt: 1;
} TaskId;

static inline bool tid_eq(TaskId tid1, TaskId tid2) {
    return tid1.ptr == tid2.ptr;
}

static const TaskId zero_tid = {0, 0, 0, false};

static inline uint64_t tid_val(TaskId tid) {
    return (tid.ptr << 8) | (tid.priority << 3) | (tid.state << 1) | tid.preempt;
}

static inline TaskId tid_from_val(uint64_t val) {
    TaskId tid;
    tid.ptr = val >> 8;
    tid.priority = (val >> 3) & 0x1f;
    tid.state = (val >> 1) & 0x3;
    tid.preempt = val & 0x1;
    return tid;
}

typedef struct {
    TaskId os;
    TaskId proc;
} AllocIdx;

// static AllocIdx alloc_idx = {zero_tid, zero_tid};

/************ The indices ************/
#define IS_STATE_FREE(x) qatomic_read(x) == 0
#define IS_STATE_USED(x) qatomic_read(x) == 1
#define SET_STATE_FREE(x) qatomic_set(x, 0)
#define SET_STATE_USED(x) { \
    assert(IS_STATE_FREE(x)); \
    qatomic_set(x, 1); \
}

typedef struct {
    uint64_t state;
    TaskId curr_os;
    TaskId curr_proc;
    TaskId current;
    uint64_t count;
    uint64_t ptr;
} Indices;

/************ The RunQueue ************/
typedef struct {
    TaskId* tasks;
} RunQueue;

/************ The BlockQueue ************/
typedef struct {
    TaskId* tasks;
} BlockQueue;

/************ The Resource ************/
typedef struct {
    bool res_status;
    BlockQueue bq;
    RunQueue rq;
} Resource;

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
    Indices* indices;
    Resource* resources;
}TAICState;

#define TYPE_TAIC "taic"
DECLARE_INSTANCE_CHECKER(TAICState, TAIC, TYPE_TAIC)

// init the internal configuration when create taic instance
static inline void taic_init(TAICState* taic) {
    taic->indices = g_new(Indices, INDEX_NUM);
    taic->resources = g_new(Resource, 1);
    taic->resources->bq.tasks = g_new(TaskId, TASKS_NUM);
    taic->resources->rq.tasks = g_new(TaskId, TASKS_NUM);
    qemu_mutex_init(&indices_lock);
    qemu_mutex_init(&task_lock);
}

// alloc a indices for a cpu
static inline void alloc_indices(TAICState* taic, TaskId os_id, TaskId proc_id) {
    if(IS_STATE_USED(&taic->indices[INDEX_NUM - 1].state)) {
        info_report("There is no free indices for this os and proc");
        return;
    }
    qemu_mutex_lock(&indices_lock);
    uint64_t i = 0, idx = 0;
    bool has_same = false;
    // find the proper indices for the os or proc
    for(; i < INDEX_NUM; i++) {
        uint64_t *state_ptr = &taic->indices[i].state;
        if(IS_STATE_FREE(state_ptr)) {
            idx = i;
            break;
        } else if(IS_STATE_USED(state_ptr) && tid_eq(taic->indices[i].curr_os, os_id) && tid_eq(taic->indices[i].curr_proc, proc_id)) {
            idx = i;
            has_same = true;
        } else {
            if(has_same) break;
        }
    }
    uint64_t *state_ptr = &taic->indices[idx].state;
    // If the indices is free, the subsequent indices are free.
    // If the indices is used, the subsequent indices must shift backward.
    if(IS_STATE_USED(state_ptr)) {
        for(i = INDEX_NUM - 1; i > idx; i--) {
            memcpy(&(taic->indices[i]), &(taic->indices[i - 1]), sizeof(Indices));
        }
    }
    SET_STATE_USED(state_ptr);
    taic->indices[idx].curr_os = os_id;
    taic->indices[idx].curr_proc = proc_id;
    taic->indices[idx].current = zero_tid;
    taic->indices[idx].count = 0;
    uint64_t ptr = 0;
    if(idx != 0) {
        ptr = taic->indices[idx - 1].ptr + taic->indices[idx - 1].count;
    }
    taic->indices[idx].ptr = ptr;
    info_report("Alloc the indices %ld", idx);
    qemu_mutex_unlock(&indices_lock);
}

// recycle the target indices
static inline void recycle_indices(TAICState* taic, uint64_t idx) {
    qemu_mutex_lock(&indices_lock);
    if(idx < INDEX_NUM - 1) {
        memcpy(&(taic->indices[idx]), &(taic->indices[idx + 1]), sizeof(Indices) * (INDEX_NUM - idx - 1));
    }
    SET_STATE_FREE(&(taic->indices[INDEX_NUM - 1].state));
    taic->indices[INDEX_NUM - 1].curr_os = zero_tid;
    taic->indices[INDEX_NUM - 1].curr_proc = zero_tid;
    taic->indices[INDEX_NUM - 1].current = zero_tid;
    taic->indices[INDEX_NUM - 1].count = 0;
    taic->indices[INDEX_NUM - 1].ptr = 0;
    info_report("Recycle the indices %ld", idx);
    qemu_mutex_unlock(&indices_lock);
}

// add task to a local queue according to the priority
static inline void add_task(TAICState* taic, uint64_t idx, TaskId task_id) {
    qemu_mutex_lock(&task_lock);
    // check whether the queue is full
    uint64_t ptr = taic->indices[INDEX_NUM - 1].ptr;
    uint64_t count = taic->indices[INDEX_NUM - 1].count;
    if(ptr + count >= TASKS_NUM) {
        info_report("The queue is full");
        qemu_mutex_unlock(&task_lock);
        return;
    }
    ptr = taic->indices[idx].ptr;
    count = taic->indices[idx].count;
    uint64_t i = ptr + count;
    for(i = TASKS_NUM - 1; i > ptr + count; i--) {
        taic->resources->rq.tasks[i] = taic->resources->rq.tasks[i - 1];
    }
    for(i = idx + 1; i < INDEX_NUM; i++) {
        if(IS_STATE_USED(&taic->indices[i].state)) {
            taic->indices[i].ptr++;
        } else {
            break;
        }
    }
    // insert into the proper position
    uint64_t priority = task_id.priority;
    bool preempt = task_id.preempt;
    // the task state is ready
    assert(task_id.state == 0);
    uint64_t pos = ptr;
    if(preempt) {
        for(i = ptr + count - 1; i >= ptr; i--) {
            taic->resources->rq.tasks[i + 1] = taic->resources->rq.tasks[i];
        }
    } else {
        for(i = ptr + count - 1; i >= ptr; i--) {
            TaskId curr = taic->resources->rq.tasks[i];
            if(curr.priority <= priority) {
                pos = i + 1;
                break;
            }
            taic->resources->rq.tasks[i + 1] = curr;
        }
    }
    taic->resources->rq.tasks[pos] = task_id;
    taic->indices[idx].count++;
    info_report("Insert the task %ld to the indices %ld, pos %ld", tid_val(task_id), idx, pos);
    qemu_mutex_unlock(&task_lock);
}

// remove the target task
static inline void remove_task(TAICState* taic, uint64_t idx, TaskId task_id) {
    qemu_mutex_lock(&task_lock);
    uint64_t ptr = taic->indices[idx].ptr;
    uint64_t count = taic->indices[idx].count;
    uint64_t i = 0;
    for(i = ptr; i < ptr + count; i++) {
        if(taic->resources->rq.tasks[i].ptr == task_id.ptr) {
            break;
        }
    }
    if(i == ptr + count) {
        info_report("The task %ld is not in the queue", tid_val(task_id));
        qemu_mutex_unlock(&task_lock);
        return;
    }
    for(; i < TASKS_NUM - 1; i++) {
        taic->resources->rq.tasks[i] = taic->resources->rq.tasks[i + 1];
    }
    taic->resources->rq.tasks[TASKS_NUM - 1] = zero_tid;
    taic->indices[idx].count--;
    for(i = idx + 1; i < INDEX_NUM; i++) {
        if(IS_STATE_USED(&taic->indices[i].state)) {
            taic->indices[i].ptr--;
        } else {
            break;
        }
    }
    info_report("Remove the task %ld from the indices %ld", tid_val(task_id), idx);
    qemu_mutex_unlock(&task_lock);
}

// pick the next ready task from the local queue, 
// if the local queue is empty, pick the task from the global queue
static inline TaskId pick_task(TAICState* taic, uint64_t idx) {
    qemu_mutex_lock(&task_lock);
    uint64_t ptr = taic->indices[idx].ptr;
    uint64_t count = taic->indices[idx].count;
    uint64_t i = ptr;
    TaskId task_id;
    if(count == 0) {
        // pick from the global queue
        TaskId curr_os = taic->indices[idx].curr_os;
        TaskId curr_proc = taic->indices[idx].curr_proc;
        uint64_t left_idx = idx, right_idx = idx;
        // lookup forward 
        for(i = idx - 1; ; i--) {
            if(tid_eq(taic->indices[i].curr_os, curr_os) && tid_eq(taic->indices[i].curr_proc, curr_proc)) {
                left_idx = i;
            } else {
                break;
            }
        }
        // lookup backward
        for(i = idx + 1; i < INDEX_NUM; i++) {
            if(tid_eq(taic->indices[i].curr_os, curr_os) && tid_eq(taic->indices[i].curr_proc, curr_proc)) {
                right_idx = i;
            } else {
                break;
            }
        }
        if(taic->indices[right_idx].ptr == taic->indices[left_idx].ptr) {
            info_report("The global queue is empty");
            qemu_mutex_unlock(&task_lock);
            return zero_tid;
        }
        ptr = taic->indices[left_idx].ptr;
        // find the first local queue which has task
        for(i = left_idx; i <= right_idx; i++) {
            if(taic->indices[i].count > 0) {
                idx = i;
                break;
            }
        }
        info_report("pick from global queue");
    }
    // pick from the local queue
    task_id = taic->resources->rq.tasks[ptr];
    for(i = ptr; i < TASKS_NUM - 1; i++) {
        taic->resources->rq.tasks[i] = taic->resources->rq.tasks[i + 1];
    }
    taic->resources->rq.tasks[TASKS_NUM - 1] = zero_tid;
    taic->indices[idx].count--;
    for(i = idx + 1; i < INDEX_NUM; i++) {
        if(IS_STATE_USED(&taic->indices[i].state)) {
            taic->indices[i].ptr--;
        } else {
            break;
        }
    }
    info_report("pick from local queue %ld", tid_val(task_id));
    qemu_mutex_unlock(&task_lock);
    return task_id;
}

// // wake_up blocked task with load balance.
// static inline void wake_up(TAICState* taic, TaskId task_id) {

// }

DeviceState *taic_create(hwaddr addr, uint32_t hart_count, uint32_t external_irq_count);


#endif
