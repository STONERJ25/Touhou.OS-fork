/* __atomic_* and __sync_* builtins (single-threaded semantics) on all operand sizes */
#include <stdio.h>

int main(void) {
    int i = 5;
    long l = 100;
    short s = 7;
    unsigned char c = 250;
    unsigned long ul = 1;
    int *p = &i;

    printf("fetch_add: %d %d\n", __atomic_fetch_add(&i, 3, __ATOMIC_SEQ_CST), i);
    printf("fetch_sub: %ld %ld\n", __atomic_fetch_sub(&l, 30, __ATOMIC_SEQ_CST), l);
    printf("add_fetch: %d %d\n", __atomic_add_fetch(&i, 2, __ATOMIC_SEQ_CST), i);
    printf("sub_fetch: %d\n", __atomic_sub_fetch(&i, 1, __ATOMIC_SEQ_CST));
    printf("fetch_and/or/xor: %d ", __atomic_fetch_and(&i, 0xe, __ATOMIC_SEQ_CST));
    printf("%d ", __atomic_fetch_or(&i, 0x41, __ATOMIC_SEQ_CST));
    printf("%d now %d\n", __atomic_fetch_xor(&i, 0xff, __ATOMIC_SEQ_CST), i);
    printf("and_fetch/or_fetch/xor_fetch: %d %d %d\n", __atomic_and_fetch(&i, 0xf0, __ATOMIC_SEQ_CST), __atomic_or_fetch(&i, 1, __ATOMIC_SEQ_CST), __atomic_xor_fetch(&i, 3, __ATOMIC_SEQ_CST));
    printf("nand: %d\n", __atomic_fetch_nand(&i, 0x0f, __ATOMIC_SEQ_CST));
    printf("exchange: %d now %d\n", __atomic_exchange_n(&i, 77, __ATOMIC_SEQ_CST), i);
    printf("load/store: %d ", __atomic_load_n(&i, __ATOMIC_SEQ_CST));
    __atomic_store_n(&i, 1234, __ATOMIC_SEQ_CST);
    printf("%d\n", i);

    int expected = 1234;
    int ok1 = __atomic_compare_exchange_n(&i, &expected, 4321, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    int ok2 = __atomic_compare_exchange_n(&i, &expected, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    printf("cas: %d %d i=%d expected=%d\n", ok1, ok2, i, expected);

    printf("short: %d %d\n", __atomic_fetch_add(&s, 5, __ATOMIC_SEQ_CST), s);
    printf("uchar wraps: %d %d\n", __atomic_fetch_add(&c, 10, __ATOMIC_SEQ_CST), c);
    printf("ulong: %lu %lu\n", __atomic_fetch_add(&ul, 0xffffffffffUL, __ATOMIC_SEQ_CST), ul);
    printf("sync: %d %d %d\n", __sync_fetch_and_add(&i, 1), __sync_add_and_fetch(&i, 1), __sync_val_compare_and_swap(&i, 4323, 9));
    printf("sync bool cas: %d %d i=%d\n", __sync_bool_compare_and_swap(&i, 9, 10), __sync_bool_compare_and_swap(&i, 9, 11), i);
    printf("test_and_set: %d ", __sync_lock_test_and_set(&i, 50));
    __sync_lock_release(&i);
    printf("released: %d\n", i);
    __sync_synchronize();
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    char flag = 0;
    printf("flag: %d %d ", __atomic_test_and_set(&flag, __ATOMIC_SEQ_CST), __atomic_test_and_set(&flag, __ATOMIC_SEQ_CST));
    __atomic_clear(&flag, __ATOMIC_SEQ_CST);
    printf("%d\n", flag);

    int *q = __atomic_exchange_n(&p, (int *)0, __ATOMIC_SEQ_CST);
    printf("pointer exchange: %d %d\n", q == &i, p == (int *)0);
    return 0;
}
