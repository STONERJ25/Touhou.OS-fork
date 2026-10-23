/* In-kernel self tests, run with "selftest" on the command line. Prints SELFTEST PASS/FAIL and exits QEMU. */
#include "kernel.h"
#include "mm.h"
#include "task.h"
#include "irq.h"
#include "cpu.h"
#include "arch.h"
#include "futex.h"
#include "fbcon.h"

static int g_failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_failures++; printk("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static void test_pmm(void) {
    klog("selftest", "pmm");
    u64 before = pmm_free_pages_count();
    paddr_t a = pmm_alloc(0), b = pmm_alloc(3), c = pmm_alloc(9);
    CHECK(a && b && c, );
    CHECK(!(b & (8 * PAGE_SIZE - 1)), );
    CHECK(!(c & (512 * PAGE_SIZE - 1)), );
    CHECK(pmm_free_pages_count() == before - 1 - 8 - 512, );
    memset(phys_to_virt(c), 0xAB, 512 * PAGE_SIZE);
    pmm_free(a, 0);
    pmm_free(b, 3);
    pmm_free(c, 9);
    CHECK(pmm_free_pages_count() == before, );
    paddr_t z = pmm_alloc_page_zeroed();
    CHECK(z && *(u64 *)phys_to_virt(z) == 0, );
    pmm_free_page(z);
}

static void test_heap(void) {
    klog("selftest", "heap");
    void *p[200];
    u64 used = heap_bytes_in_use();
    for (int i = 0; i < 200; i++) {
        size_t sz = (size_t)(i * 37 % 3000) + 1;
        p[i] = kmalloc(sz);
        CHECK(p[i], );
        memset(p[i], i, sz);
    }
    for (int i = 0; i < 200; i++) {
        size_t sz = (size_t)(i * 37 % 3000) + 1;
        u8 *q = p[i];
        CHECK(q[0] == (u8)i && q[sz - 1] == (u8)i, );
        kfree(p[i]);
    }
    CHECK(heap_bytes_in_use() == used, );
    char *s = kmalloc(10);
    strcpy(s, "gensokyo");
    s = krealloc(s, 5000);
    CHECK(!strcmp(s, "gensokyo"), );
    kfree(s);
    void *big = kzalloc(1 << 20);
    CHECK(big && ((u8 *)big)[(1 << 20) - 1] == 0, );
    kfree(big);
}

static volatile u64 g_spin_count, g_sleep_count;
static volatile bool g_stop;

static void spinner(void *arg) {
    (void)arg;
    while (!g_stop) g_spin_count++;          /* never yields: only preemption can let anyone else run */
}

static void sleeper(void *arg) {
    (void)arg;
    while (!g_stop) {
        task_sleep_ms(10);
        g_sleep_count++;
    }
}

static Semaphore g_items;
static Mutex g_mutex;
static int g_produced, g_consumed;

static void producer(void *arg) {
    (void)arg;
    for (int i = 0; i < 50; i++) {
        mutex_lock(&g_mutex);
        g_produced++;
        mutex_unlock(&g_mutex);
        sem_post(&g_items);
        if (i % 7 == 0) task_sleep_ms(1);
    }
}

static void consumer(void *arg) {
    (void)arg;
    for (int i = 0; i < 50; i++) {
        sem_wait(&g_items);
        mutex_lock(&g_mutex);
        g_consumed++;
        mutex_unlock(&g_mutex);
    }
}

static void test_sched(void) {
    klog("selftest", "scheduler: preemption, sleep, semaphore");
    g_stop = false;
    g_spin_count = g_sleep_count = 0;
    task_create_detached("spinner", spinner, NULL);
    task_create_detached("sleeper", sleeper, NULL);
    sem_init(&g_items, 0);
    mutex_init(&g_mutex);
    task_create_detached("producer", producer, NULL);
    task_create_detached("consumer", consumer, NULL);

    task_sleep_ms(400);
    g_stop = true;
    CHECK(g_spin_count > 1000, );
    CHECK(g_sleep_count >= 15, );
    klog("selftest", "spinner=%lu sleeper=%lu produced=%d consumed=%d", (unsigned long)g_spin_count,
         (unsigned long)g_sleep_count, g_produced, g_consumed);
    CHECK(g_produced == 50 && g_consumed == 50, );
    task_sleep_ms(50);
    CHECK(task_count() == 2, );              /* kmain + idle are left once detached tasks are reaped */
}

static void test_vmm(void) {
    klog("selftest", "vmm: user mappings");
    paddr_t pml4 = vmm_new_user_pml4();
    CHECK(pml4, );
    paddr_t pg = pmm_alloc_page_zeroed();
    CHECK(vmm_map(pml4, 0x400000, pg, PTE_U | PTE_W | PTE_NX) == 0, );
    u64 flags = 0;
    CHECK((vmm_translate(pml4, 0x400123, &flags) & PTE_ADDR) == pg, );
    CHECK(flags & PTE_U, );
    CHECK(vmm_unmap(pml4, 0x400000) == pg, );
    CHECK(vmm_translate(pml4, 0x400000, NULL) == 0, );
    pmm_free_page(pg);
    vmm_free_user_pml4(pml4);
}

void selftest_run(void) {
    printk("SELFTEST BEGIN\n");
    test_pmm();
    test_heap();
    test_vmm();
    g_failures += futex_selftest();             /* kernel/sys/futex.c */
    g_failures += fbcon_selftest();             /* kernel/gfx/fbcon.c: the diff renderer must leave the screen equal to the grid */
    test_sched();
    if (g_failures) {
        printk("SELFTEST FAIL (%d failures)\n", g_failures);
        qemu_exit(1);
    }
    printk("SELFTEST PASS\n");
    qemu_exit(0);
}
