/* util.inc -- allocation, formatting, byte buffers, hash maps, whole-file reading. */

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) fatal("out of memory (Yuyuko ate everything)");
    return p;
}

void *xcalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) fatal("out of memory (Yuyuko ate everything)");
    return p;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) fatal("out of memory (Yuyuko ate everything)");
    return p;
}

char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *xstrdup(const char *s) {
    return xstrndup(s, strlen(s));
}

char *xvformat(const char *fmt, va_list ap) {
    char small[512];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    if (n < 0) n = 0;
    char *out;
    if ((size_t)n < sizeof small) {
        out = xstrndup(small, (size_t)n);
    } else {
        out = xmalloc((size_t)n + 1);
        vsnprintf(out, (size_t)n + 1, fmt, ap2);
    }
    va_end(ap2);
    return out;
}

char *xformat(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = xvformat(fmt, ap);
    va_end(ap);
    return s;
}

void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "mcc: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

/* ---- byte buffers */

static void buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra) cap *= 2;
    b->p = xrealloc(b->p, cap);
    b->cap = cap;
}

static void buf_put(Buf *b, const void *d, size_t n) {
    if (!n) return;
    buf_reserve(b, n);
    memcpy(b->p + b->len, d, n);
    b->len += n;
}

static void buf_u8(Buf *b, int v) {
    buf_reserve(b, 1);
    b->p[b->len++] = (u8)v;
}

static void buf_u16(Buf *b, int v) {
    buf_u8(b, v & 0xff);
    buf_u8(b, (v >> 8) & 0xff);
}

static void buf_u32(Buf *b, u32 v) {
    buf_u8(b, (int)(v & 0xff));
    buf_u8(b, (int)((v >> 8) & 0xff));
    buf_u8(b, (int)((v >> 16) & 0xff));
    buf_u8(b, (int)((v >> 24) & 0xff));
}

static void buf_u64(Buf *b, u64 v) {
    buf_u32(b, (u32)(v & 0xffffffffu));
    buf_u32(b, (u32)(v >> 32));
}

static void buf_zero(Buf *b, size_t n) {
    if (!n) return;
    buf_reserve(b, n);
    memset(b->p + b->len, 0, n);
    b->len += n;
}

static void buf_align(Buf *b, size_t a) {
    if (a > 1 && b->len % a) buf_zero(b, a - b->len % a);
}

static void buf_str(Buf *b, const char *s) {
    buf_put(b, s, strlen(s));
}

static void put32(u8 *p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static void put64(u8 *p, u64 v) {
    put32(p, (u32)(v & 0xffffffffu));
    put32(p + 4, (u32)(v >> 32));
}

static u32 get32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u64 get64(const u8 *p) {
    return (u64)get32(p) | ((u64)get32(p + 4) << 32);
}

/* ---- hash map keyed by (pointer, length) strings; open addressing, FNV-1a */

#define MAP_TOMBSTONE ((void *)-1)

static u64 fnv_hash(const char *s, int len) {
    u64 hash = 0xcbf29ce484222325ULL;
    for (int i = 0; i < len; i++) {
        hash *= 0x100000001b3ULL;
        hash ^= (u8)s[i];
    }
    return hash;
}

static bool map_match(MapEntry *ent, const char *key, int keylen) {
    return ent->key && ent->key != MAP_TOMBSTONE && ent->keylen == keylen && !memcmp(ent->key, key, (size_t)keylen);
}

static MapEntry *map_find(Map *map, const char *key, int keylen) {
    if (!map->buckets) return NULL;
    u64 hash = fnv_hash(key, keylen);
    for (int i = 0; i < map->capacity; i++) {
        MapEntry *ent = &map->buckets[(hash + (u64)i) % (u64)map->capacity];
        if (map_match(ent, key, keylen)) return ent;
        if (ent->key == NULL) return NULL;
    }
    return NULL;
}

/* The first free slot (empty or tombstone) on the probe path of a key that is known to be absent. */
static MapEntry *map_slot(Map *map, const char *key, int keylen) {
    u64 hash = fnv_hash(key, keylen);
    for (int i = 0; i < map->capacity; i++) {
        MapEntry *ent = &map->buckets[(hash + (u64)i) % (u64)map->capacity];
        if (ent->key == MAP_TOMBSTONE || ent->key == NULL) return ent;
    }
    fatal("internal error: hash map full");
    return NULL;
}

static void map_rehash(Map *map) {
    int nkeys = 0;
    for (int i = 0; i < map->capacity; i++)
        if (map->buckets[i].key && map->buckets[i].key != MAP_TOMBSTONE) nkeys++;
    int cap = map->capacity;
    while ((nkeys * 100) / cap >= 50) cap *= 2;
    Map map2;
    map2.buckets = xcalloc((size_t)cap * sizeof(MapEntry));
    map2.capacity = cap;
    map2.used = 0;
    for (int i = 0; i < map->capacity; i++) {
        MapEntry *ent = &map->buckets[i];
        if (ent->key && ent->key != MAP_TOMBSTONE) {
            MapEntry *e2 = map_slot(&map2, ent->key, ent->keylen);
            *e2 = *ent;
            map2.used++;
        }
    }
    free(map->buckets);
    *map = map2;
}

static void *map_get2(Map *map, const char *key, int keylen) {
    MapEntry *ent = map_find(map, key, keylen);
    return ent ? ent->val : NULL;
}

static void *map_get(Map *map, const char *key) {
    return map_get2(map, key, (int)strlen(key));
}

static void map_put2(Map *map, const char *key, int keylen, void *val) {
    if (!map->buckets) {
        map->buckets = xcalloc(16 * sizeof(MapEntry));
        map->capacity = 16;
    } else if ((map->used * 100) / map->capacity >= 70) {
        map_rehash(map);
    }
    MapEntry *ent = map_find(map, key, keylen);
    if (ent) {
        ent->val = val;
        return;
    }
    ent = map_slot(map, key, keylen);
    if (ent->key == NULL) map->used++;
    ent->key = (char *)key;
    ent->keylen = keylen;
    ent->val = val;
}

static void map_put(Map *map, const char *key, void *val) {
    map_put2(map, key, (int)strlen(key), val);
}

static void map_delete2(Map *map, const char *key, int keylen) {
    MapEntry *ent = map_find(map, key, keylen);
    if (ent) ent->key = MAP_TOMBSTONE;
}

/* ---- files */

static u8 *read_whole_file(const char *path, size_t *len_out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    size_t cap = 65536, len = 0;
    u8 *buf = xmalloc(cap + 1);
    for (;;) {
        size_t n = fread(buf + len, 1, cap - len, fp);
        len += n;
        if (len < cap) break;
        cap *= 2;
        buf = xrealloc(buf, cap + 1);
    }
    fclose(fp);
    buf[len] = 0;
    if (len_out) *len_out = len;
    return buf;
}

static bool file_exists(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    fclose(fp);
    return true;
}

static int write_whole_file(const char *path, const void *data, size_t len, bool executable) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    if (len && fwrite(data, 1, len, fp) != len) {
        fclose(fp);
        return -1;
    }
    if (fclose(fp) != 0) return -1;
#ifndef _WIN32
    if (executable) chmod(path, 0755);
#else
    (void)executable;
#endif
    return 0;
}

static bool str_eq(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

static bool str_starts(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool str_ends(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !memcmp(s + a - b, suffix, b);
}

static const char *base_of(const char *path) {
    const char *p = strrchr(path, '/');
#ifdef _WIN32
    const char *q = strrchr(path, '\\');
    if (q && (!p || q > p)) p = q;
#endif
    return p ? p + 1 : path;
}

static char *dir_of(const char *path) {
    const char *b = base_of(path);
    if (b == path) return xstrdup(".");
    size_t n = (size_t)(b - path);
    if (n > 1) n--;
    return xstrndup(path, n);
}

static char *join_path(const char *dir, const char *name) {
    if (!*dir || str_eq(dir, ".")) return xstrdup(name);
    size_t n = strlen(dir);
    if (dir[n - 1] == '/') return xformat("%s%s", dir, name);
    return xformat("%s/%s", dir, name);
}
