#include "process.h"
#include "kernel.h"

size_t __copy_user(void *dst, const void *src, size_t n);
size_t __clear_user(void *dst, size_t n);

bool user_range_ok(const void *p, size_t n) {
    u64 a = (u64)p;
    return a < USER_TOP && n <= USER_TOP - a;
}

int copy_from_user(void *dst, const void *src, size_t n) {
    if (!n) return 0;
    if (!user_range_ok(src, n)) return -EFAULT;
    return __copy_user(dst, src, n) ? -EFAULT : 0;
}

int copy_to_user(void *dst, const void *src, size_t n) {
    if (!n) return 0;
    if (!user_range_ok(dst, n)) return -EFAULT;
    return __copy_user(dst, src, n) ? -EFAULT : 0;
}

int clear_user(void *dst, size_t n) {
    if (!n) return 0;
    if (!user_range_ok(dst, n)) return -EFAULT;
    return __clear_user(dst, n) ? -EFAULT : 0;
}

/* Length of the user string at s (the NUL not counted), looking at no more than max bytes. The memory is read in pieces that never
 * cross a page boundary, so a string that ends just before an unmapped page is fine. */
long strnlen_user(const char *s, size_t max) {
    char buf[256];
    size_t done = 0;
    while (done < max) {
        const char *p = s + done;
        size_t chunk = MIN(MIN(sizeof buf, max - done), PAGE_SIZE - ((u64)p & (PAGE_SIZE - 1)));
        if (!user_range_ok(p, chunk)) return -EFAULT;
        if (__copy_user(buf, p, chunk)) return -EFAULT;
        for (size_t i = 0; i < chunk; i++)
            if (buf[i] == 0) return (long)(done + i);
        done += chunk;
    }
    return -ENAMETOOLONG;
}

/* Copies a NUL-terminated string (at most max bytes including the NUL). Returns its length. */
int strncpy_from_user(char *dst, const char *src, size_t max) {
    size_t done = 0;
    while (done < max) {
        const char *s = src + done;
        size_t chunk = MIN(max - done, PAGE_SIZE - ((u64)s & (PAGE_SIZE - 1)));
        if (!user_range_ok(s, chunk)) return -EFAULT;
        if (__copy_user(dst + done, s, chunk)) return -EFAULT;
        for (size_t i = 0; i < chunk; i++)
            if (dst[done + i] == 0) return (int)(done + i);
        done += chunk;
    }
    return -ENAMETOOLONG;
}
