#ifndef K_TYPES_H
#define K_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

typedef uint64_t paddr_t;       /* physical address */
typedef uint64_t vaddr_t;       /* virtual address */
typedef int64_t  ssize_t;
typedef int64_t  off_t;

#define PAGE_SIZE   4096UL
#define PAGE_SHIFT  12
#define PAGE_MASK   (~(PAGE_SIZE - 1))

#define ALIGN_UP(x, a)    (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define ALIGN_DOWN(x, a)  ((x) & ~((__typeof__(x))(a) - 1))
#define IS_ALIGNED(x, a)  (((x) & ((a) - 1)) == 0)
#define ARRAY_SIZE(a)     (sizeof(a) / sizeof((a)[0]))
#define BIT(n)            (1UL << (n))
#define MIN(a, b)         ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#define MAX(a, b)         ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

#define PACKED      __attribute__((packed))
#define NORETURN    __attribute__((noreturn))
#define UNUSED      __attribute__((unused))
#define ALIGNED(n)  __attribute__((aligned(n)))
#define NOINLINE    __attribute__((noinline))
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define barrier()   __asm__ volatile ("" ::: "memory")

#endif
