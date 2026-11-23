/*
 * pageverify: write and verify large files whose every 4 KiB page carries
 * its own index and a seed, so a page that ends up holding someone else's
 * data is found, located and saved.
 *
 * Built for Windows on ARM64 (MSVC, tools/pageverify/pageverify.vcxproj) and
 * for Linux (gcc), so the same pattern file can be made on the PC, copied to
 * the board over the network, and checked there, or the other way round.
 *
 * Page layout (little endian):
 *   0x00  "PGVF0001"         magic
 *   0x08  seed               u64
 *   0x10  page index         u64 (file offset / 4096)
 *   0x18  ~page index ^ seed u64 (header self-check)
 *   0x20  4064 bytes of splitmix64(seed, page index) stream
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PG 4096u
#define HDR 32u
#define MAGIC "PGVF0001"
#define DEFAULT_CHUNK (4u << 20)

typedef unsigned long long u64;

static FILE *g_log;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
}

static void stamp(void)
{
#ifdef _WIN32
    SYSTEMTIME t;
    GetLocalTime(&t);
    say("[%04u-%02u-%02u %02u:%02u:%02u] ", (unsigned)t.wYear,
        (unsigned)t.wMonth, (unsigned)t.wDay, (unsigned)t.wHour,
        (unsigned)t.wMinute, (unsigned)t.wSecond);
#else
    time_t now = time(NULL);
    struct tm tm;
    char b[32];
    localtime_r(&now, &tm);
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
    say("[%s] ", b);
#endif
}

static double now_s(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
#endif
}

/* ------------------------------------------------------------------ */
/* pattern                                                             */

static u64 splitmix(u64 *s)
{
    u64 z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void put64(uint8_t *p, u64 v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static u64 get64(const uint8_t *p)
{
    u64 v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static void make_page(uint8_t *p, u64 seed, u64 idx)
{
    u64 s = seed ^ (idx * 0xD1B54A32D192ED03ULL);
    memcpy(p, MAGIC, 8);
    put64(p + 8, seed);
    put64(p + 16, idx);
    put64(p + 24, ~idx ^ seed);
    for (unsigned o = HDR; o < PG; o += 8)
        put64(p + o, splitmix(&s));
}

/* A header written by this tool: returns 1 and fills seed/idx. */
static int parse_header(const uint8_t *p, u64 *seed, u64 *idx)
{
    if (memcmp(p, MAGIC, 8) != 0)
        return 0;
    *seed = get64(p + 8);
    *idx = get64(p + 16);
    return get64(p + 24) == (~*idx ^ *seed);
}

/* ------------------------------------------------------------------ */
/* file I/O                                                            */

#ifdef _WIN32
typedef HANDLE fh_t;
#define BADF INVALID_HANDLE_VALUE
#else
typedef int fh_t;
#define BADF (-1)
#endif

/* direct: bypass the cache (FILE_FLAG_NO_BUFFERING [+ WRITE_THROUGH]). */
static fh_t f_open(const char *path, int write, int direct)
{
#ifdef _WIN32
    wchar_t w[1024];
    DWORD fl = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN;
    if (direct)
        fl |= FILE_FLAG_NO_BUFFERING | (write ? FILE_FLAG_WRITE_THROUGH : 0);
    if (!MultiByteToWideChar(CP_ACP, 0, path, -1, w, 1024))
        return BADF;
    return CreateFileW(w, write ? GENERIC_WRITE : GENERIC_READ,
                       FILE_SHARE_READ, NULL,
                       write ? CREATE_ALWAYS : OPEN_EXISTING, fl, NULL);
#else
    int fl = write ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY;
#ifdef O_DIRECT
    if (direct)
        fl |= O_DIRECT;
#endif
    (void)direct;
    return open(path, fl, 0644);
#endif
}

static void f_close(fh_t f)
{
#ifdef _WIN32
    CloseHandle(f);
#else
    close(f);
#endif
}

static int f_read(fh_t f, void *b, unsigned n, unsigned *got)
{
#ifdef _WIN32
    DWORD g = 0;
    BOOL ok = ReadFile(f, b, n, &g, NULL);
    *got = g;
    return ok ? 0 : (int)GetLastError();
#else
    ssize_t r = read(f, b, n);
    if (r < 0)
        return 1;
    *got = (unsigned)r;
    return 0;
#endif
}

static int f_write(fh_t f, const void *b, unsigned n)
{
#ifdef _WIN32
    DWORD w = 0;
    if (!WriteFile(f, b, n, &w, NULL))
        return (int)GetLastError();
    return w == n ? 0 : -1;
#else
    return write(f, b, n) == (ssize_t)n ? 0 : 1;
#endif
}

static int f_flush(fh_t f)
{
#ifdef _WIN32
    return FlushFileBuffers(f) ? 0 : (int)GetLastError();
#else
    return fsync(f);
#endif
}

static int f_size(fh_t f, u64 *sz)
{
#ifdef _WIN32
    LARGE_INTEGER li;
    if (!GetFileSizeEx(f, &li))
        return 1;
    *sz = (u64)li.QuadPart;
    return 0;
#else
    struct stat st;
    if (fstat(f, &st))
        return 1;
    *sz = (u64)st.st_size;
    return 0;
#endif
}

static int f_delete(const char *path)
{
#ifdef _WIN32
    return DeleteFileA(path) ? 0 : (int)GetLastError();
#else
    return unlink(path);
#endif
}

/* Page-aligned buffer (no-buffering I/O needs sector alignment). */
static uint8_t *abuf(size_t n)
{
#ifdef _WIN32
    return (uint8_t *)VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_READWRITE);
#else
    void *p = NULL;
    return posix_memalign(&p, PG, n) ? NULL : (uint8_t *)p;
#endif
}

/* ------------------------------------------------------------------ */
/* classification of a bad page                                        */

static unsigned arm64_score(const uint8_t *p)
{
    /* Words that are common in compiled ARM64 code and rare in random
     * data: pacibsp, autibsp, ret, nop, mov x29,sp, stp x29,x30,[sp,#-16]! */
    static const uint32_t w[] = { 0xd503237f, 0xd50323ff, 0xd65f03c0,
                                  0xd503201f, 0x910003fd, 0xa9bf7bfd };
    unsigned n = 0;
    for (unsigned o = 0; o < PG; o += 4) {
        uint32_t v = (uint32_t)p[o] | (uint32_t)p[o + 1] << 8 |
                     (uint32_t)p[o + 2] << 16 | (uint32_t)p[o + 3] << 24;
        for (unsigned i = 0; i < sizeof w / sizeof w[0]; i++)
            n += v == w[i];
    }
    return n;
}

static const char *classify(const uint8_t *got, u64 want_seed, u64 want_idx,
                            char *extra, size_t extra_n)
{
    u64 s, i;
    unsigned zeros = 0, ff = 0;
    extra[0] = 0;
    if (parse_header(got, &s, &i)) {
        uint8_t ref[PG];
        make_page(ref, s, i);
        int intact = memcmp(ref, got, PG) == 0;
        snprintf(extra, extra_n, "page %llu of seed %016llx%s", i, s,
                 intact ? " (intact)" : " (header only)");
        if (s == want_seed && i == want_idx)
            return "own header, body damaged";
        if (s == want_seed)
            return "misplaced page of THIS file (stale or misdirected I/O)";
        return "page of ANOTHER pageverify file (other seed)";
    }
    for (unsigned o = 0; o < PG; o++) {
        zeros += got[o] == 0;
        ff += got[o] == 0xff;
    }
    if (zeros == PG)
        return "all zero";
    if (ff == PG)
        return "all 0xFF";
    if (got[0] == 'M' && got[1] == 'Z')
        return "starts with MZ (PE header page)";
    unsigned a = arm64_score(got);
    snprintf(extra, extra_n, "arm64-score %u, zero bytes %u", a, zeros);
    if (a >= 8)
        return "looks like ARM64 code";
    return "unknown foreign data";
}

/* ------------------------------------------------------------------ */
/* commands                                                            */

struct opts {
    u64 seed;
    int have_seed;
    unsigned chunk;
    int direct;
    int passes;
    const char *dump;
    int keep;
    int go_on;
    int rounds;
    u64 max_report;
};

static int do_write(const char *path, u64 mib, const struct opts *o)
{
    fh_t f = f_open(path, 1, o->direct);
    if (f == BADF) {
        say("cannot create %s\n", path);
        return 2;
    }
    uint8_t *b = abuf(o->chunk);
    if (!b) {
        f_close(f);
        return 2;
    }
    u64 pages = mib * (1048576u / PG), idx = 0;
    double t0 = now_s();
    stamp();
    say("write %s: %llu MiB, seed %016llx, chunk %u KiB, %s\n", path, mib,
        o->seed, o->chunk / 1024, o->direct ? "unbuffered+write-through" :
        "cached");
    while (idx < pages) {
        unsigned n = o->chunk / PG;
        if ((u64)n > pages - idx)
            n = (unsigned)(pages - idx);
        for (unsigned k = 0; k < n; k++)
            make_page(b + (size_t)k * PG, o->seed, idx + k);
        int e = f_write(f, b, n * PG);
        if (e) {
            say("write error %d at offset 0x%llx\n", e, idx * PG);
            f_close(f);
            return 2;
        }
        idx += n;
    }
    int e = f_flush(f);
    f_close(f);
    double dt = now_s() - t0;
    stamp();
    say("write done: %.1f s, %.1f MiB/s, flush %s\n", dt,
        dt > 0 ? (double)mib / dt : 0.0, e ? "FAILED" : "ok");
    return e ? 2 : 0;
}

static void hex_line(const uint8_t *p, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        say("%02x", (unsigned)p[i]);
    say("\n");
}

static void dump_page(const char *dir, const char *path, u64 off,
                      const uint8_t *p, int pass)
{
    char name[1200];
    const char *base = strrchr(path, '\\');
    const char *b2 = strrchr(path, '/');
    if (!base || (b2 && b2 > base))
        base = b2;
    base = base ? base + 1 : path;
    snprintf(name, sizeof name, "%s/%s.off%010llx.pass%d.bin", dir, base,
             off, pass);
    FILE *d = fopen(name, "wb");
    if (!d) {
        say("  (cannot save %s)\n", name);
        return;
    }
    fwrite(p, 1, PG, d);
    fclose(d);
    say("  saved %s\n", name);
}

/* Returns the number of bad pages, or -1 on an I/O error. */
static long long verify_once(const char *path, const struct opts *o, int pass,
                             u64 *seed_io)
{
    fh_t f = f_open(path, 0, o->direct);
    if (f == BADF) {
        say("cannot open %s\n", path);
        return -1;
    }
    u64 size = 0;
    f_size(f, &size);
    uint8_t *b = abuf(o->chunk);
    if (!b) {
        f_close(f);
        return -1;
    }
    uint8_t ref[PG];
    u64 off = 0, bad = 0, seed = *seed_io;
    int have_seed = o->have_seed || pass > 1;
    double t0 = now_s();
    stamp();
    say("verify %s pass %d: %llu bytes, %s\n", path, pass, size,
        o->direct ? "unbuffered (from disk)" : "cached");
    if (size % PG)
        say("  note: size is not a multiple of 4096; the tail is ignored\n");
    for (;;) {
        unsigned got = 0;
        int e = f_read(f, b, o->chunk, &got);
        if (e) {
            say("read error %d at offset 0x%llx\n", e, off);
            f_close(f);
            return -1;
        }
        if (got < PG)
            break;
        for (unsigned k = 0; k + PG <= got; k += PG, off += PG) {
            const uint8_t *p = b + k;
            u64 idx = off / PG;
            if (!have_seed) {
                u64 s, i;
                if (!parse_header(p, &s, &i) || i != 0) {
                    say("  first page has no valid header; pass --seed\n");
                    f_close(f);
                    return -1;
                }
                seed = s;
                have_seed = 1;
                say("  seed %016llx (from page 0)\n", seed);
            }
            make_page(ref, seed, idx);
            if (memcmp(ref, p, PG) == 0)
                continue;
            bad++;
            unsigned diff = 0, first = PG, last = 0;
            for (unsigned i = 0; i < PG; i++)
                if (ref[i] != p[i]) {
                    diff++;
                    if (first == PG)
                        first = i;
                    last = i;
                }
            if (bad <= o->max_report) {
                char extra[160];
                const char *what = classify(p, seed, idx, extra, sizeof extra);
                say("BAD page %llu at offset 0x%llx: %u bytes differ "
                    "(first 0x%x, last 0x%x): %s%s%s\n", idx, off, diff,
                    first, last, what, extra[0] ? "; " : "", extra);
                say("  got  +0x000: ");
                hex_line(p, 32);
                say("  got  +0x400: ");
                hex_line(p + 0x400, 32);
                if (o->dump)
                    dump_page(o->dump, path, off, p, pass);
            }
        }
        if (got < o->chunk)
            break;
    }
    f_close(f);
    *seed_io = seed;
    double dt = now_s() - t0;
    stamp();
    say("verify pass %d: %llu pages, %llu bad, %.1f s, %.1f MiB/s\n", pass,
        off / PG, bad, dt, dt > 0 ? (double)off / 1048576.0 / dt : 0.0);
    return (long long)bad;
}

static int do_verify(const char *path, const struct opts *o)
{
    u64 seed = o->seed;
    long long worst = 0;
    for (int p = 1; p <= o->passes; p++) {
        long long r = verify_once(path, o, p, &seed);
        if (r < 0)
            return 2;
        if (r > worst)
            worst = r;
    }
    return worst ? 1 : 0;
}

/* write, then verify cached and unbuffered, delete; repeat. */
static int do_loop(const char *dir, u64 mib, const struct opts *o0)
{
    struct opts o = *o0;
    int rc = 0;
    u64 total_bad = 0;
    for (int r = 1; o.rounds == 0 || r <= o.rounds; r++) {
        char path[1100];
        o.seed = o0->have_seed ? o0->seed + (u64)r
                               : ((u64)(now_s() * 1e6) ^ ((u64)r << 48));
        o.have_seed = 1;
        snprintf(path, sizeof path, "%s/pageverify-%03d.bin", dir, r);
        stamp();
        say("=== round %d ===\n", r);
        struct opts w = o;
        w.direct = o0->direct;
        if (do_write(path, mib, &w)) {
            rc = 2;
            break;
        }
        struct opts c = o;
        c.direct = 0;
        c.passes = 1;
        u64 s = o.seed;
        long long b1 = verify_once(path, &c, 1, &s);
        struct opts u = o;
        u.direct = 1;
        u.passes = 1;
        long long b2 = verify_once(path, &u, 2, &s);
        if (b1 < 0 || b2 < 0) {
            rc = 2;
            break;
        }
        total_bad += (u64)b1 + (u64)b2;
        stamp();
        say("round %d: cached %lld bad, disk %lld bad, total so far %llu\n",
            r, b1, b2, total_bad);
        if (b1 || b2) {
            rc = 1;
            if (o.go_on && !o.keep) {
                f_delete(path);
                continue;
            }
            say("keeping %s for inspection\n", path);
            if (!o.keep)
                break;
        } else if (!o.keep) {
            f_delete(path);
        }
    }
    return rc;
}

/* Self-test for CI: pattern, header round trip, classification. */
static int selftest(void)
{
    uint8_t a[PG], b[PG];
    char x[160];
    u64 s, i;
    make_page(a, 0x1234, 7);
    make_page(b, 0x1234, 8);
    if (!parse_header(a, &s, &i) || s != 0x1234 || i != 7)
        return 1;
    if (memcmp(a + HDR, b + HDR, PG - HDR) == 0)
        return 2;
    if (strncmp(classify(b, 0x1234, 7, x, sizeof x), "misplaced", 9))
        return 3;
    memset(b, 0, PG);
    if (strcmp(classify(b, 0x1234, 7, x, sizeof x), "all zero"))
        return 4;
    for (unsigned o = 0; o < PG; o += 16) {
        put64(b + o, 0xd65f03c0d503237fULL);
        put64(b + o + 8, 0x910003fda9bf7bfdULL);
    }
    if (strcmp(classify(b, 0x1234, 7, x, sizeof x), "looks like ARM64 code"))
        return 5;
    /* deterministic stream: a known value guards against accidental
     * changes to the pattern, which would break cross-build checks */
    make_page(a, 1, 0);
    if (get64(a + HDR) != 0x910a2dec89025cc1ULL) {
        printf("pattern word %016llx\n", get64(a + HDR));
        return 6;
    }
    printf("selftest ok\n");
    return 0;
}

static void usage(void)
{
    printf(
"pageverify: find 4 KiB pages of a file that hold someone else's data\n"
"\n"
"  pageverify write  FILE MiB  [options]   create a pattern file\n"
"  pageverify verify FILE      [options]   check one (seed read from page 0)\n"
"  pageverify loop   DIR  MiB  [options]   write, verify cached, verify\n"
"                                          unbuffered, delete; repeat\n"
"  pageverify selftest\n"
"\n"
"options:\n"
"  --seed HEX       pattern seed (write: default random; loop: base)\n"
"  --chunk KiB      I/O size, multiple of 4 (default 4096)\n"
"  --direct         unbuffered I/O (writes also write-through); reads\n"
"                   then come from the disk, not the cache\n"
"  --passes N       verify N times (default 1); differing results between\n"
"                   passes mean the corruption happens on the read path\n"
"  --rounds N       loop rounds (default 1, 0 = forever)\n"
"  --keep           loop: keep every file and do not stop on the first bad\n"
"  --continue       loop: do not stop on a bad round; delete its file (the\n"
"                   bad pages are already logged and, with --dump, saved)\n"
"  --dump DIR       save every bad page as a .bin file in DIR\n"
"  --log FILE       append all output to FILE as well\n"
"  --max-report N   print at most N bad pages per pass (default 64)\n"
"\n"
"exit code: 0 clean, 1 bad pages found, 2 usage or I/O error\n");
}

int main(int argc, char **argv)
{
    struct opts o;
    memset(&o, 0, sizeof o);
    o.chunk = DEFAULT_CHUNK;
    o.passes = 1;
    o.rounds = 1;
    o.max_report = 64;
    const char *pos[3] = { 0, 0, 0 };
    int np = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--seed") && v) {
            o.seed = strtoull(v, NULL, 16);
            o.have_seed = 1;
            i++;
        } else if (!strcmp(a, "--chunk") && v) {
            unsigned long k = strtoul(v, NULL, 10);
            if (k < 4 || k % 4 || k > 65536) {
                usage();
                return 2;
            }
            o.chunk = (unsigned)k * 1024u;
            i++;
        } else if (!strcmp(a, "--direct")) {
            o.direct = 1;
        } else if (!strcmp(a, "--keep")) {
            o.keep = 1;
        } else if (!strcmp(a, "--continue")) {
            o.go_on = 1;
        } else if (!strcmp(a, "--passes") && v) {
            o.passes = atoi(v);
            i++;
        } else if (!strcmp(a, "--rounds") && v) {
            o.rounds = atoi(v);
            i++;
        } else if (!strcmp(a, "--dump") && v) {
            o.dump = v;
            i++;
        } else if (!strcmp(a, "--max-report") && v) {
            o.max_report = strtoull(v, NULL, 10);
            i++;
        } else if (!strcmp(a, "--log") && v) {
            g_log = fopen(v, "a");
            i++;
        } else if (a[0] == '-' && a[1] == '-') {
            usage();
            return 2;
        } else if (np < 3) {
            pos[np++] = a;
        }
    }
    if (np < 1) {
        usage();
        return 2;
    }
    if (!strcmp(pos[0], "selftest"))
        return selftest();
    if (o.passes < 1)
        o.passes = 1;
    if (!o.have_seed && strcmp(pos[0], "verify")) {
        u64 t = (u64)(now_s() * 1e9);
        o.seed = splitmix(&t);
    }
    int rc;
    if (!strcmp(pos[0], "write") && np == 3) {
        rc = do_write(pos[1], strtoull(pos[2], NULL, 10), &o);
        if (!rc)
            say("seed %016llx\n", o.seed);
    } else if (!strcmp(pos[0], "verify") && np == 2) {
        rc = do_verify(pos[1], &o);
    } else if (!strcmp(pos[0], "loop") && np == 3) {
        rc = do_loop(pos[1], strtoull(pos[2], NULL, 10), &o);
    } else {
        usage();
        rc = 2;
    }
    stamp();
    say("exit %d\n", rc);
    if (g_log)
        fclose(g_log);
    return rc;
}
