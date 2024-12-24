#include "hw/taic.h"

enum state {
    SINT_IDLE = 0,
    REG_SEND0 = 1,
    REG_RECV0 = 2,
    REG_RECV1 = 3,
    CANCEL_SEND0 = 4,
    WAKEUP = 5,
    CLEAN = 6,
    CHECK_SEND = 7,
};

void init_softintrslots(SoftIntrSlots* softintrslots, uint64_t size) {
    softintrslots->state = 0;
    softintrslots->cap = size;
    softintrslots->sendcap = g_new0(SendCap, size);
    softintrslots->recvcap = g_new0(RecvCap, size);
}

void register_send(SoftIntrSlots* softintrslots, uint64_t data) {
    uint64_t state = 0;
    while(1) {
        state = qatomic_cmpxchg(&softintrslots->state, SINT_IDLE, REG_SEND0);
        if(state == SINT_IDLE) {
            softintrslots->os_id = data;
            return;
        } else if (state == REG_SEND0) {
            softintrslots->proc_id = data;
            int i = 0;
            uint64_t os_id = softintrslots->os_id;
            uint64_t proc_id = data;
            int idx = -1;
            for(i = 0; i < softintrslots->cap; i++) {
                if(softintrslots->sendcap[i].recv_os_id == os_id && softintrslots->sendcap[i].recv_proc_id == proc_id) {
                    qatomic_set(&softintrslots->state, SINT_IDLE);
                    return;
                } else if (softintrslots->sendcap[i].recv_os_id == 0 && softintrslots->sendcap[i].recv_proc_id == 0) {
                    idx = i;
                }
            }
            if(idx != -1) {
                softintrslots->sendcap[idx].recv_os_id = os_id;
                softintrslots->sendcap[idx].recv_proc_id = proc_id;
            } else {
                error_report("No send cap slots");
            }
            qatomic_set(&softintrslots->state, SINT_IDLE);
            return;
        }
    }
}

void cancel_send(SoftIntrSlots* softintrslots, uint64_t data) {
    uint64_t state = 0;
    while(1) {
        state = qatomic_cmpxchg(&softintrslots->state, SINT_IDLE, CANCEL_SEND0);
        if(state == SINT_IDLE) {
            softintrslots->os_id = data;
            return;
        } else if (state == CANCEL_SEND0) {
            softintrslots->proc_id = data;
            int i = 0;
            uint64_t os_id = softintrslots->os_id;
            uint64_t proc_id = data;
            for(i = 0; i < softintrslots->cap; i++) {
                if(softintrslots->sendcap[i].recv_os_id == os_id && softintrslots->sendcap[i].recv_proc_id == proc_id) {
                    softintrslots->sendcap[i].recv_os_id = 0;
                    softintrslots->sendcap[i].recv_proc_id = 0;
                    qatomic_set(&softintrslots->state, SINT_IDLE);
                    return;
                }
            }
            // The target send cap is not found.
            qatomic_set(&softintrslots->state, SINT_IDLE);
            return;
        }
    }
}

int64_t check_send(SoftIntrSlots* softintrslots, uint64_t recv_os_id, uint64_t recv_proc_id) {
    uint64_t state = 0;
    while(1) {
        state = qatomic_cmpxchg(&softintrslots->state, SINT_IDLE, CHECK_SEND);
        if(state == SINT_IDLE) {
            int64_t sendcap_idx = -1;
            for(int i = 0; i < softintrslots->cap; i++) {
                if(softintrslots->sendcap[i].recv_os_id == recv_os_id && softintrslots->sendcap[i].recv_proc_id == recv_proc_id) {
                    sendcap_idx = i;
                    break;
                }
            }
            qatomic_set(&softintrslots->state, SINT_IDLE);
            return sendcap_idx;
        }
    }
}

void register_recv(SoftIntrSlots* softintrslots, uint64_t data) {
    uint64_t state = 0;
    while(1) {
        state = qatomic_cmpxchg(&softintrslots->state, SINT_IDLE, REG_RECV0);
        if(state == SINT_IDLE) {
            softintrslots->os_id = data;
            return;
        } else if (state == REG_RECV0) {
            softintrslots->proc_id = data;
            qatomic_set(&softintrslots->state, REG_RECV1);
            return;
        } else if (state == REG_RECV1) {
            softintrslots->task_id = data;
            uint64_t os_id = softintrslots->os_id;
            uint64_t proc_id = softintrslots->proc_id;
            uint64_t task_id = data;
            int i = 0;
            int idx = -1;
            for(i = 0; i < softintrslots->cap; i++) {
                if(softintrslots->recvcap[i].send_os_id == os_id && softintrslots->recvcap[i].send_proc_id == proc_id) {
                    softintrslots->recvcap[i].handler = task_id;
                    qatomic_set(&softintrslots->state, SINT_IDLE);
                    return;
                } else if (softintrslots->recvcap[i].send_os_id == 0 && softintrslots->recvcap[i].send_proc_id == 0) {
                    idx = i;
                }
            }
            if(idx != -1) {
                softintrslots->recvcap[idx].send_os_id = os_id;
                softintrslots->recvcap[idx].send_proc_id = proc_id;
                softintrslots->recvcap[idx].handler = task_id;
            } else {
                error_report("No recv cap slots");
            }
            qatomic_set(&softintrslots->state, SINT_IDLE);
            return;
        }
    }
}

uint64_t wakeup_soft(SoftIntrSlots* softintrslots, uint64_t send_os_id, uint64_t send_proc_id) {
    uint64_t state = 0;
    while(1) {
        state = qatomic_cmpxchg(&softintrslots->state, SINT_IDLE, WAKEUP);
        if(state == SINT_IDLE) {
            int i = 0;
            for(i = 0; i < softintrslots->cap; i++) {
                if(softintrslots->recvcap[i].send_os_id == send_os_id && softintrslots->recvcap[i].send_proc_id == send_proc_id) {
                    uint64_t res = softintrslots->recvcap[i].handler;
                    softintrslots->recvcap[i].send_os_id = 0;
                    softintrslots->recvcap[i].send_proc_id = 0;
                    softintrslots->recvcap[i].handler = 0;
                    qatomic_set(&softintrslots->state, SINT_IDLE);
                    return res;
                }
            }
            error_report("Cannot wakeup the softintr task handler");
            qatomic_set(&softintrslots->state, SINT_IDLE);
            return 0;
        }
    }
}

void clean_softintrslots(SoftIntrSlots* softintrslots) {
    uint64_t state = 0;
    while(1) {
        state = qatomic_cmpxchg(&softintrslots->state, SINT_IDLE, CLEAN);
        if(state == SINT_IDLE) {
            int i = 0;
            for(i = 0; i < softintrslots->cap; i++) {
                softintrslots->sendcap[i].recv_os_id = 0;
                softintrslots->sendcap[i].recv_proc_id = 0;
                softintrslots->recvcap[i].send_os_id = 0;
                softintrslots->recvcap[i].send_proc_id = 0;
                softintrslots->recvcap[i].handler = 0;
            }
            qatomic_set(&softintrslots->state, SINT_IDLE);
            return;
        }
    }
}