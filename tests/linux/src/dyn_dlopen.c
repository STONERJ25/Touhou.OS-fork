/*
 * dlopen and friends under the Linux personality (L2 of docs/COMPAT.md): the dynamic linker maps a library while the program runs, which exercises everything the kernel owes it
 * after start-up: file-backed private mappings with offsets, MAP_FIXED over a reserved region, mprotect (RELRO, the guard gaps), munmap of a mapped library on dlclose, a search
 * through LD_LIBRARY_PATH and the system directories of the Linux root, and thread-local storage for a library loaded after the thread began.
 * Every line is a fact the manual pages promise, so a real Linux prints the same lines (tools/linux-oracle.py runs it with LD_LIBRARY_PATH naming the directory of libdynplug.so).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int doubled(int x) { return 2 * x; }

static void *thread_main(void *arg) {
    int (*next)(void) = arg;
    int a = next(), b = next();                 /* a thread has its own copy of the library's thread-local variable, starting from the template */
    return (void *)(long)(a * 100 + b);
}

static int count_plugin(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    const char *slash = strrchr(info->dlpi_name, '/');
    if (strstr(slash ? slash : info->dlpi_name, "libdynplug")) (*(int *)data)++;
    return 0;
}

int main(void) {
    void *m = dlopen("libm.so.6", RTLD_NOW);    /* from the system directories of the root */
    printf("libm.so.6: %s\n", m ? "opened" : "failed");
    double (*cosp)(double) = m ? (double (*)(double))dlsym(m, "cos") : NULL;
    printf("cos(0) = %.1f\n", cosp ? cosp(0.0) : -1.0);

    void *h = dlopen("libdynplug.so", RTLD_NOW | RTLD_GLOBAL);          /* through LD_LIBRARY_PATH */
    printf("libdynplug.so: %s\n", h ? "opened" : "failed");
    if (!h) { printf("%s\n", dlerror()); return 1; }
    int (*add)(int, int) = (int (*)(int, int))dlsym(h, "plug_add");
    int *counter = (int *)dlsym(h, "plug_counter");
    int (*constructed)(void) = (int (*)(void))dlsym(h, "plug_constructed");
    int (*next)(void) = (int (*)(void))dlsym(h, "plug_tls_next");
    const char *(*name)(int) = (const char *(*)(int))dlsym(h, "plug_name");
    int (*apply)(int (*)(int), int) = (int (*)(int (*)(int), int))dlsym(h, "plug_apply");
    printf("symbols found: %s\n", add && counter && constructed && next && name && apply ? "all" : "some missing");
    printf("plug_add(2, 3) = %d\n", add(2, 3));
    *counter = 110;
    printf("after plug_counter = 110: plug_add(2, 3) = %d\n", add(2, 3));
    printf("constructor ran %d time(s)\n", constructed());
    printf("plug_name: %s %s\n", name(0), name(1));
    printf("plug_apply(doubled, 20) = %d\n", apply(doubled, 20));
    printf("thread-local in the main thread: %d %d\n", next(), next());
    pthread_t t;
    pthread_create(&t, NULL, thread_main, (void *)next);
    void *r;
    pthread_join(t, &r);
    printf("thread-local in another thread: %ld\n", (long)r);
    printf("thread-local in the main thread again: %d\n", next());
    printf("dlsym of a missing symbol: %s\n", dlsym(h, "no_such_symbol") ? "found?!" : "NULL");

    int n = 0;
    dl_iterate_phdr(count_plugin, &n);
    printf("libdynplug among the loaded objects: %d\n", n);
    void *again = dlopen("libdynplug.so", RTLD_NOW);                   /* the same object again: a reference count, not a second copy */
    printf("opened again gives the same handle: %s\n", again == h ? "yes" : "no");
    dlclose(again);
    int closed = 0;
    ((void (*)(int *))dlsym(h, "plug_on_close"))(&closed);
    printf("dlclose: %d\n", dlclose(h));
    printf("destructor ran at dlclose: %s\n", closed ? "yes" : "no");
    n = 0;
    dl_iterate_phdr(count_plugin, &n);
    printf("libdynplug among the loaded objects after dlclose: %d\n", n);

    void *bad = dlopen("libdoesnotexist.so.9", RTLD_NOW);
    printf("missing library: %s\n", bad ? "opened?!" : "failed");
    printf("dlerror: %s\n", dlerror());
    void *self = dlopen(NULL, RTLD_NOW);
    printf("dlopen(NULL): %s, printf found: %s\n", self ? "ok" : "failed", self && dlsym(self, "printf") ? "yes" : "no");
    return 0;
}
