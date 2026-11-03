/* Bootstrap USB pump: no thread, interrupt, network or timed-sleep dependency.
 * The caller supplies a monotonic hardware counter and bounded busy delay. */
#ifndef VERMEER_RAM_PUMP_H
#define VERMEER_RAM_PUMP_H
#ifdef VERMEER_HOST_TEST
#include <stdint.h>
#else
#include <sys/types.h>
#endif
struct vrp_ops {
    void *context;
    int (*step)(void *);       /* service one bounded batch of USB events */
    int (*state)(void *);      /* 0 pending, 1 done, negative failure */
    uint64_t (*progress)(void *);
    uint64_t (*now)(void *);
    void (*idle)(void *);      /* busy delay, never a scheduler wait */
};
enum { VRP_COMPLETE=0, VRP_FAILED=-1, VRP_TIMEOUT=-2 };
static inline int vrp_wait(const struct vrp_ops *ops,uint64_t idle_ticks)
{
    uint64_t last=ops->now(ops->context),previous=ops->progress(ops->context);
    if (!idle_ticks)return VRP_TIMEOUT;
    for (;;) {
        if (ops->step(ops->context)<0)return VRP_FAILED;
        int state=ops->state(ops->context);
        if (state<0)return VRP_FAILED;
        if (state==1)return VRP_COMPLETE;
        uint64_t now=ops->now(ops->context),progress=ops->progress(ops->context);
        if (progress!=previous) { previous=progress;last=now; }
        if (now-last>=idle_ticks)return VRP_TIMEOUT;
        ops->idle(ops->context);
    }
}
#endif
