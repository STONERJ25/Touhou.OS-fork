/* Tasks (kernel threads and the kernel half of user processes), scheduler, wait queues, locks. */
#ifndef K_TASK_H
#define K_TASK_H

#include "types.h"
#include "arch.h"
#include "abi.h"

#define KSTACK_SIZE  (16 * 1024)
#define SCHED_SLICE  10         /* ticks (ms) */

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_SLEEPING,
    TASK_ZOMBIE,
} TaskState;

struct Process;
struct WaitQueue;

typedef struct Task {
    u64  rsp;                   /* saved kernel stack pointer; first member, used by switch.asm */
    struct Task *next;          /* circular list of all tasks */
    struct Task *wq_next;       /* wait queue link */
    struct WaitQueue *wq;       /* queue this task is blocked on, if any */
    int  tid;
    char name[24];
    TaskState state;
    u8  *kstack;                /* low address of the kernel stack */
    u64  kstack_top;
    u64  wake_ms;               /* for SLEEPING, and for BLOCKED with a timeout */
    bool timed;
    bool timed_out;
    bool intr;                  /* blocked in an interruptible wait: signals may wake it */
    int  exit_code;
    u64  cpu_ticks;
    int  slice;
    bool detached;              /* kernel thread that frees itself on exit */
    paddr_t pml4;
    struct Process *proc;       /* NULL for pure kernel threads */
    u8  *fpu;                   /* 512-byte FXSAVE area for tasks that run user code */
    void (*entry)(void *);
    void *arg;
} Task;

typedef struct WaitQueue {
    Task *head;
    Task *tail;
} WaitQueue;

extern Task *g_current;
#define current g_current

void  sched_init(void);
void  sched_start_idle(void);                       /* turns the boot context into the idle task */
void  sched_tick(void);                             /* from the timer interrupt */
void  schedule(void);
void  task_on_trap_return(TrapFrame *tf);
void  task_print_current(void);

Task *task_create(const char *name, void (*entry)(void *), void *arg);   /* kernel thread; starts READY */
Task *task_create_detached(const char *name, void (*entry)(void *), void *arg);
NORETURN void task_exit(int code);
void  task_yield(void);
void  task_sleep_ms(u64 ms);
void  task_for_each(void (*fn)(Task *t, void *ctx), void *ctx);
void  task_interrupt(Task *t);                      /* wake a task blocked in an interruptible wait */

/* Set by the process layer: true if the current task has a deliverable signal. */
extern bool (*g_signal_pending_hook)(void);
static inline bool signal_pending(void) { return g_signal_pending_hook && g_signal_pending_hook(); }
int   task_count(void);
Task *task_by_tid(int tid);

void  wq_init(WaitQueue *wq);
void  wq_block(WaitQueue *wq);                      /* call with IRQs disabled; returns when woken */
bool  wq_block_timeout(WaitQueue *wq, u64 ms);      /* false if the timeout expired */
void  wq_wake_one(WaitQueue *wq);
void  wq_wake_all(WaitQueue *wq);

/* Sleep until cond is true. cond is evaluated with IRQs disabled. */
#define wait_event(wq, cond)                                      \
    do {                                                          \
        u64 _f = irq_save();                                      \
        while (!(cond)) wq_block(&(wq));                          \
        irq_restore(_f);                                          \
    } while (0)

typedef struct { volatile int locked; WaitQueue wq; } Mutex;
void mutex_init(Mutex *m);
void mutex_lock(Mutex *m);
void mutex_unlock(Mutex *m);
bool mutex_trylock(Mutex *m);

typedef struct { volatile int count; WaitQueue wq; } Semaphore;
void sem_init(Semaphore *s, int count);
void sem_wait(Semaphore *s);
bool sem_wait_timeout(Semaphore *s, u64 ms);
void sem_post(Semaphore *s);

void context_switch(u64 *old_rsp, u64 new_rsp);
void task_entry_trampoline(void);

#endif
