#include "hw/taic.h"

void init_local_queue(LocalQueue* local_queue) {
    local_queue->is_used = false;
    local_queue->ready_queue = g_new0(Queue, 1);
    queue_init(local_queue->ready_queue);
    local_queue->count = 0;
}

void push_local_queue(LocalQueue* local_queue, uint64_t data, bool need_preempt) {
    local_queue->count++;
    if(need_preempt) {
        queue_push_head(local_queue->ready_queue, data);
    } else {
        queue_push(local_queue->ready_queue, data);
    }
}

uint64_t pop_local_queue(LocalQueue* local_queue) {
    if(local_queue->count > 0) {
        local_queue->count--;
    }
    return queue_pop(local_queue->ready_queue);
}

enum GQState {
    GQ_IDLE = 0,
    ALLOC_LQ = 1,
    FREE_LQ = 2,
    ENQ_LQ = 3,
    DEQ_LQ = 4,
    REG_EXT = 5,
    HANDLE_EXT = 6,
    HANDLE_SOFT = 7,
};

enum SintState {
    SINT_IDLE = 0,
    SINT_REG_SEND = 1,
    SINT_CANCEL_SEND = 2,
    SINT_REG_RECV0 = 3,
    SINT_REG_RECV1 = 4,
    SINT_SEND_INTR = 5,
};

void init_global_queue(GlobalQueue* global_queue) {
    int i = 0;
    global_queue->state = 0;
    global_queue->sint_state = 0;
    global_queue->os_id = 0;
    global_queue->proc_id = 0;
    global_queue->hart_id = -1;
    global_queue->ssip = false;
    global_queue->usip = false;
    global_queue->used_lq_count = 0;
    global_queue->sendcap_idx = -1;
    global_queue->recv_os = 0;
    global_queue->recv_proc = 0;
    global_queue->local_queue = g_new0(LocalQueue, LQ_NUM);
    for(i = 0; i < LQ_NUM; i++) {
        init_local_queue(&(global_queue->local_queue[i]));
    }
    init_extintrslots(&(global_queue->extintrslots), INTR_NUM);
    init_softintrslots(&(global_queue->softintrslots), INTR_NUM);
}

int64_t alloc_lq(GlobalQueue* global_queue) {
    uint64_t state = 0;
    while (1) {
        /* code */
        state = qatomic_cmpxchg(&global_queue->state, GQ_IDLE, ALLOC_LQ);
        if (state == GQ_IDLE) {
            int64_t i = 0;
            for(i = 0; i < LQ_NUM; i++) {
                if(!global_queue->local_queue[i].is_used) {
                    global_queue->local_queue[i].is_used = true;
                    global_queue->used_lq_count += 1;
                    qatomic_set(&global_queue->state, GQ_IDLE);
                    return i;
                }
            }
            // error_report("The is no local queue");
            qatomic_set(&global_queue->state, GQ_IDLE);
            return -1;
        }
    }
}

void free_lq(GlobalQueue* global_queue, uint64_t lq_idx) {
    if(lq_idx >= LQ_NUM) {
        // error_report("The lq_idx is not valid");
        return;
    }
    uint64_t state = 0;
    while (1) {
        /* code */
        state = qatomic_cmpxchg(&global_queue->state, GQ_IDLE, FREE_LQ);
        if (state == GQ_IDLE) {
            global_queue->local_queue[lq_idx].is_used = false;
            global_queue->used_lq_count -= 1;
            if(global_queue->used_lq_count == 0) {
                global_queue->os_id = 0;
                global_queue->proc_id = 0;
                global_queue->hart_id = -1;
                global_queue->ssip = false;
                global_queue->usip = false;
                for(int i = 0; i < LQ_NUM; i++) {
                    while(global_queue->local_queue[i].count != 0) {
                        pop_local_queue(&(global_queue->local_queue[i]));
                    }
                }
            }
            qatomic_set(&global_queue->state, GQ_IDLE);
            return;
        }
    }
}

void lq_enq(GlobalQueue* global_queue, uint64_t lq_idx, uint64_t data, bool need_preempt) {
    if(lq_idx >= LQ_NUM) {
        error_report("The lq_idx is not valid");
        return;
    }
    if(!global_queue->local_queue[lq_idx].is_used) {
        error_report("The lq_idx is not used");
        return;
    }
    uint64_t state = 0;
    while (1) {
        /* code */
        state = qatomic_cmpxchg(&global_queue->state, GQ_IDLE, ENQ_LQ);
        if (state == GQ_IDLE || state == HANDLE_EXT || state == HANDLE_SOFT) {
            push_local_queue(&(global_queue->local_queue[lq_idx]), data, need_preempt);
            qatomic_set(&global_queue->state, GQ_IDLE);
            return;
        }
    }
}

uint64_t lq_deq(GlobalQueue* global_queue, uint64_t lq_idx) {
    if(lq_idx >= LQ_NUM) {
        error_report("The lq_idx is not valid");
        return 0;
    }
    if(!global_queue->local_queue[lq_idx].is_used) {
        error_report("The lq_idx is not used");
        return 0;
    }
    uint64_t state = 0;
    while (1) {
        /* code */
        state = qatomic_cmpxchg(&global_queue->state, GQ_IDLE, DEQ_LQ);
        if (state == GQ_IDLE) {
            if(global_queue->ssip == true || global_queue->usip == true) {
                lq_idx = 0;
                global_queue->ssip = false;
                global_queue->usip = false;
            }
            uint64_t res = pop_local_queue(&(global_queue->local_queue[lq_idx]));
            if(res == 0) {
                // 从其他的局部队列中窃取任务
                for(int i = 0; i < LQ_NUM; i++) {
                    if(global_queue->local_queue[i].count != 0) {
                        res = pop_local_queue(&(global_queue->local_queue[i]));
                        break;
                    }
                }
            }
            qatomic_set(&global_queue->state, GQ_IDLE);
            return res;
        }
    }
}

void register_ext_handler(GlobalQueue* global_queue, uint64_t irq_idx, uint64_t data) {
    uint64_t state = 0;
    while (1) {
        /* code */
        state = qatomic_cmpxchg(&global_queue->state, GQ_IDLE, REG_EXT);
        if(state == GQ_IDLE) {
            register_ext(&(global_queue->extintrslots), irq_idx, data);
            qatomic_set(&global_queue->state, GQ_IDLE);
            return;
        }
    }
}

void handle_extintr(GlobalQueue* global_queue, uint64_t irq_idx) {
    uint64_t state = 0;
    while (1) {
        /* code */
        state = qatomic_cmpxchg(&global_queue->state, GQ_IDLE, HANDLE_EXT);
        if(state == GQ_IDLE) {
            uint64_t ext_handler = wakeup_ext(&(global_queue->extintrslots), irq_idx);
            if(ext_handler == 0) {
                qatomic_set(&global_queue->state, GQ_IDLE);
                return;
            }
            bool need_preempt = false;
            if((ext_handler & 1) == 1) {     // preempt
                need_preempt = true;
                if(global_queue->proc_id == 0) {
                    global_queue->ssip = true;
                } else {
                    global_queue->usip = true;
                }
            }
            lq_enq(global_queue, 0, ext_handler, need_preempt);
            return;
        }
    }
}

void register_sender(GlobalQueue* global_queue, uint64_t data) {
    uint64_t state = 0;
    while (1) {
        /* code */
        state = qatomic_cmpxchg(&global_queue->sint_state, SINT_IDLE, SINT_REG_SEND);
        if(state == SINT_IDLE) {
            register_send(&(global_queue->softintrslots), data);
            return;
        } else if(state == SINT_REG_SEND) {
            register_send(&(global_queue->softintrslots), data);
            qatomic_set(&global_queue->sint_state, SINT_IDLE);
            return;
        }
    }
}

void cancel_sender(GlobalQueue* global_queue, uint64_t data) {
    uint64_t state = 0;
    while (1) {
        /* code */
        state = qatomic_cmpxchg(&global_queue->sint_state, SINT_IDLE, SINT_CANCEL_SEND);
        if(state == SINT_IDLE) {
            cancel_send(&(global_queue->softintrslots), data);
            return;
        } else if(state == SINT_CANCEL_SEND) {
            cancel_send(&(global_queue->softintrslots), data);
            qatomic_set(&global_queue->sint_state, SINT_IDLE);
            return;
        }
    }
}

void register_receiver(GlobalQueue* global_queue, uint64_t data) {
    uint64_t state = 0;
    while (1) {
        /* code */
        state = qatomic_cmpxchg(&global_queue->sint_state, SINT_IDLE, SINT_REG_RECV0);
        if(state == SINT_IDLE) {
            register_recv(&(global_queue->softintrslots), data);
            return;
        } else if(state == SINT_REG_RECV0) {
            register_recv(&(global_queue->softintrslots), data);
            qatomic_set(&global_queue->sint_state, SINT_REG_RECV1);
            return;
        } else if(state == SINT_REG_RECV1) {
            register_recv(&(global_queue->softintrslots), data);
            qatomic_set(&global_queue->sint_state, SINT_IDLE);
            return;
        }
    }
}

void check_sendcap(GlobalQueue* global_queue, uint64_t data) {
    uint64_t state = 0;
    while (1) {
        /* code */
        state = qatomic_cmpxchg(&global_queue->sint_state, SINT_IDLE, SINT_SEND_INTR);
        if(state == SINT_IDLE) {
            global_queue->recv_os = data;
            return;
        } else if(state == SINT_SEND_INTR) {
            global_queue->recv_proc = data;
            uint64_t recv_os = global_queue->recv_os;
            uint64_t recv_proc = data;
            global_queue->sendcap_idx = check_send(&(global_queue->softintrslots), recv_os, recv_proc);
            qatomic_set(&global_queue->sint_state, SINT_IDLE);
            return;
        }
    }
}

void handle_softintr(GlobalQueue* global_queue, uint64_t send_os, uint64_t send_proc) {
    uint64_t state = 0;
    while (1) {
        /* code */
        state = qatomic_cmpxchg(&global_queue->state, GQ_IDLE, HANDLE_SOFT);
        if(state == GQ_IDLE) {
            uint64_t soft_handler = wakeup_soft(&(global_queue->softintrslots), send_os, send_proc);
            if(soft_handler == 0) {
                qatomic_set(&global_queue->state, GQ_IDLE);
                return;
            }
            bool need_preempt = false;
            if((soft_handler & 1) == 1) {     // preempt
                need_preempt = true;
                if(global_queue->proc_id == 0) {
                    global_queue->ssip = true;
                } else {
                    global_queue->usip = true;
                }
            }
            lq_enq(global_queue, 0, soft_handler, need_preempt);
            return;
        }
    }
}

void write_hartid(GlobalQueue* global_queue, uint64_t data) {
    global_queue->hart_id = data;
}
