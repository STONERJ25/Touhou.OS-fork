#include "process.h"
#include "kernel.h"
#include "irq.h"

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

/* A relative timeout, struct gtimespec in user memory, as an absolute uptime_ms() deadline. Anything too large to count in milliseconds
 * is "never" (~0), so a huge tv_sec cannot wrap around into the past. */
long deadline_from_timespec(const void *uts, u64 *deadline) {
    struct gtimespec ts;
    if (copy_from_user(&ts, uts, sizeof ts)) return -EFAULT;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) return -EINVAL;
    u64 now = uptime_ms();
    if ((u64)ts.tv_sec > (~0ULL >> 2) / 1000) { *deadline = ~0ULL; return 0; }
    u64 ms = (u64)ts.tv_sec * 1000 + (u64)ts.tv_nsec / 1000000 + ((ts.tv_nsec % 1000000) ? 1 : 0);
    *deadline = ms >= ~0ULL - now ? ~0ULL : now + ms;
    return 0;
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
