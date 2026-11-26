/*
 * idmactest - unit tests for the dwcmshc IDMAC descriptor table builder and
 * the DMA FIFOTH value (drivers/sd/dwcmshc/idmac.h), the same header the
 * driver compiles.
 *
 *   idmactest        exit 0 if every check passes
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../drivers/sd/dwcmshc/idmac.h"

#define TABLE_MAX   600
#define CANARY      0xA5A5A5A5U
#define TABLE_BUS   0x7F000000ULL

static MSHC_IDMAC_DESCRIPTOR table[TABLE_MAX + 4];
static int failures;
static int checks;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        checks++;                                               \
        if (!(cond)) {                                          \
            failures++;                                         \
            printf("FAIL %s:%d: ", __func__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

static void
fill_canary(void)
{
    MSHC_U32 i;

    for (i = 0; i < TABLE_MAX + 4; i++) {
        table[i].Des0 = table[i].Des1 = table[i].Des2 = table[i].Des3 = CANARY;
    }
}

static int
canary_intact(MSHC_U32 from)
{
    MSHC_U32 i;

    for (i = from; i < TABLE_MAX + 4; i++) {
        if (table[i].Des0 != CANARY || table[i].Des1 != CANARY ||
            table[i].Des2 != CANARY || table[i].Des3 != CANARY) {
            return 0;
        }
    }
    return 1;
}

/*
 * The invariants of a finished table of n descriptors at TABLE_BUS that
 * describes the elements in order.
 */
static void
check_table(const char *name, MSHC_U32 n, const MSHC_U64 *addr, const MSHC_U32 *len, MSHC_U32 count)
{
    MSHC_U32 d = 0, e, i;
    MSHC_U64 a;
    MSHC_U32 left;

    for (i = 0; i < n; i++) {
        MSHC_U32 des0 = table[i].Des0;
        int first = (i == 0), last = (i == n - 1);

        CHECK(des0 & MSHC_IDMAC_DES0_OWN, "%s: desc %u not owned by the IDMAC", name, i);
        CHECK(!!(des0 & MSHC_IDMAC_DES0_FS) == first, "%s: desc %u FS wrong (0x%08x)", name, i, des0);
        CHECK(!!(des0 & MSHC_IDMAC_DES0_LD) == last, "%s: desc %u LD wrong (0x%08x)", name, i, des0);
        CHECK(!!(des0 & MSHC_IDMAC_DES0_ER) == last, "%s: desc %u ER wrong (0x%08x)", name, i, des0);
        CHECK(!!(des0 & MSHC_IDMAC_DES0_CH) == !last, "%s: desc %u CH wrong (0x%08x)", name, i, des0);
        CHECK(!!(des0 & MSHC_IDMAC_DES0_DIC) == !last, "%s: desc %u DIC wrong (0x%08x)", name, i, des0);
        CHECK((des0 & MSHC_IDMAC_DES0_CES) == 0, "%s: desc %u CES set", name, i);
        CHECK((table[i].Des1 & MSHC_IDMAC_DES1_BS2_MASK) == 0, "%s: desc %u BS2 set", name, i);
        CHECK(table[i].Des3 == (last ? (MSHC_U32)TABLE_BUS : (MSHC_U32)(TABLE_BUS + 16 * (i + 1))),
              "%s: desc %u DES3 0x%08x", name, i, table[i].Des3);
        CHECK(table[i].Des1 > 0 && table[i].Des1 <= MSHC_IDMAC_MAX_LENGTH_PER_ENTRY,
              "%s: desc %u length %u", name, i, table[i].Des1);
        CHECK((table[i].Des1 & 3) == 0 && (table[i].Des2 & 3) == 0,
              "%s: desc %u unaligned (len %u addr 0x%08x)", name, i, table[i].Des1, table[i].Des2);
    }

    /* the descriptors walk the elements byte for byte, in order */
    for (e = 0; e < count; e++) {
        a = addr[e];
        left = len[e];
        while (left > 0 && d < n) {
            CHECK(table[d].Des2 == (MSHC_U32)a, "%s: desc %u addr 0x%08x, want 0x%08llx",
                  name, d, table[d].Des2, a);
            CHECK(table[d].Des1 <= left, "%s: desc %u runs past element %u", name, d, e);
            a += table[d].Des1;
            left -= table[d].Des1 <= left ? table[d].Des1 : left;
            d++;
        }
        CHECK(left == 0, "%s: element %u not fully described", name, e);
    }
    CHECK(d == n, "%s: %u descriptors, the elements used %u", name, n, d);
    CHECK(canary_intact(n), "%s: wrote past descriptor %u", name, n);
}

static int
build(const MSHC_U64 *addr, const MSHC_U32 *len, MSHC_U32 count, MSHC_U32 capacity, MSHC_IDMAC_BUILDER *b)
{
    MSHC_U32 i;

    fill_canary();
    MshcIdmacBegin(b, table, TABLE_BUS, capacity);
    for (i = 0; i < count; i++) {
        (void)MshcIdmacAppend(b, addr[i], len[i]);
    }
    return MshcIdmacFinish(b);
}

static void
test_single_block(void)
{
    MSHC_U64 a[] = { 0x40001000ULL };
    MSHC_U32 l[] = { 512 };
    MSHC_IDMAC_BUILDER b;

    CHECK(build(a, l, 1, MshcIdmacCapacityFor(512), &b) == MSHC_IDMAC_OK, "single block refused");
    CHECK(b.Count == 1 && b.Bytes == 512, "count %u bytes %u", b.Count, b.Bytes);
    CHECK(table[0].Des0 == (MSHC_IDMAC_DES0_OWN | MSHC_IDMAC_DES0_FS | MSHC_IDMAC_DES0_LD | MSHC_IDMAC_DES0_ER),
          "des0 0x%08x", table[0].Des0);
    check_table("single", b.Count, a, l, 1);
}

static void
test_sizes(void)
{
    /* one contiguous element of each size: one descriptor per 4 KiB */
    static const MSHC_U32 sizes[] = { 4, 8, 64, 508, 512, 4092, 4096, 4100, 8192, 12288, 65536, 131072, 1048576 };
    MSHC_U32 i;

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        MSHC_U64 a[] = { 0x80000000ULL };
        MSHC_U32 l[] = { sizes[i] };
        MSHC_U32 want = (sizes[i] + 4095) / 4096;
        MSHC_IDMAC_BUILDER b;
        char name[32];

        if (want + 1 > TABLE_MAX) {
            continue;
        }
        snprintf(name, sizeof(name), "size %u", sizes[i]);
        CHECK(build(a, l, 1, MshcIdmacCapacityFor(sizes[i]), &b) == MSHC_IDMAC_OK, "%s refused (%d)", name, b.Error);
        CHECK(b.Count == want, "%s: %u descriptors, want %u", name, b.Count, want);
        CHECK(b.Bytes == sizes[i], "%s: %u bytes", name, b.Bytes);
        check_table(name, b.Count, a, l, 1);
    }
}

static void
test_chaining(void)
{
    /* page-split list, first element not on a page */
    MSHC_U64 a[] = { 0x40000E00ULL, 0x52341000ULL, 0x6000F000ULL, 0x41000000ULL };
    MSHC_U32 l[] = { 0x200, 0x3000, 0x1000, 0xE00 };
    MSHC_U32 total = 0x200 + 0x3000 + 0x1000 + 0xE00;
    MSHC_IDMAC_BUILDER b;

    CHECK(build(a, l, 4, MshcIdmacCapacityFor(total), &b) == MSHC_IDMAC_OK, "chain refused (%d)", b.Error);
    CHECK(b.Count == 6, "chain: %u descriptors", b.Count);
    CHECK(b.Count <= MshcIdmacCapacityFor(total), "chain over capacity");
    check_table("chain", b.Count, a, l, 4);
}

static void
test_4g_boundary(void)
{
    MSHC_IDMAC_BUILDER b;

    {
        /* ends exactly at 4 GiB: allowed */
        MSHC_U64 a[] = { 0xFFFFF000ULL };
        MSHC_U32 l[] = { 0x1000 };
        CHECK(build(a, l, 1, 4, &b) == MSHC_IDMAC_OK, "buffer ending at 4 GiB refused (%d)", b.Error);
        check_table("end at 4G", b.Count, a, l, 1);
    }
    {
        /* crosses 4 GiB */
        MSHC_U64 a[] = { 0xFFFFF000ULL };
        MSHC_U32 l[] = { 0x2000 };
        CHECK(build(a, l, 1, 4, &b) == MSHC_IDMAC_E_ABOVE_4G, "crossing 4 GiB: %d", b.Error);
        CHECK(canary_intact(0), "crossing 4 GiB wrote a descriptor");
    }
    {
        /* starts at 4 GiB */
        MSHC_U64 a[] = { 0x100000000ULL };
        MSHC_U32 l[] = { 0x200 };
        CHECK(build(a, l, 1, 4, &b) == MSHC_IDMAC_E_ABOVE_4G, "at 4 GiB: %d", b.Error);
    }
    {
        /* far above, as RAM above 4 GiB on a 4 GB RK3576 board would be */
        MSHC_U64 a[] = { 0x40000000ULL, 0x13FFFF000ULL };
        MSHC_U32 l[] = { 0x1000, 0x1000 };
        CHECK(build(a, l, 2, 4, &b) == MSHC_IDMAC_E_ABOVE_4G, "second element above 4 GiB: %d", b.Error);
    }
    {
        /* the table itself must be below 4 GiB */
        MSHC_U64 a[] = { 0x40000000ULL };
        MSHC_U32 l[] = { 0x200 };
        fill_canary();
        MshcIdmacBegin(&b, table, 0xFFFFFFF0ULL, 2);
        (void)MshcIdmacAppend(&b, a[0], l[0]);
        CHECK(MshcIdmacFinish(&b) == MSHC_IDMAC_E_TABLE, "table crossing 4 GiB: %d", b.Error);
        CHECK(canary_intact(0), "table error wrote a descriptor");
        MshcIdmacBegin(&b, table, 0x100000000ULL, 2);
        CHECK(b.Error == MSHC_IDMAC_E_TABLE, "table above 4 GiB: %d", b.Error);
        MshcIdmacBegin(&b, table, 0x7F000002ULL, 2);
        CHECK(b.Error == MSHC_IDMAC_E_TABLE, "misaligned table: %d", b.Error);
    }
}

static void
test_errors(void)
{
    MSHC_IDMAC_BUILDER b;

    {
        MSHC_U64 a[] = { 0x40000000ULL };
        MSHC_U32 l[] = { 0 };
        CHECK(build(a, l, 1, 4, &b) == MSHC_IDMAC_E_LENGTH, "zero length: %d", b.Error);
    }
    {
        MSHC_U64 a[] = { 0x40000000ULL };
        MSHC_U32 l[] = { 6 };
        CHECK(build(a, l, 1, 4, &b) == MSHC_IDMAC_E_LENGTH, "length 6: %d", b.Error);
    }
    {
        MSHC_U64 a[] = { 0x40000002ULL };
        MSHC_U32 l[] = { 8 };
        CHECK(build(a, l, 1, 4, &b) == MSHC_IDMAC_E_ALIGN, "address +2: %d", b.Error);
    }
    {
        MSHC_IDMAC_BUILDER e;
        fill_canary();
        MshcIdmacBegin(&e, table, TABLE_BUS, 4);
        CHECK(MshcIdmacFinish(&e) == MSHC_IDMAC_E_EMPTY, "empty table: %d", e.Error);
    }
    {
        /* more than the table holds: refused, nothing written past it */
        MSHC_U64 a[] = { 0x40000000ULL };
        MSHC_U32 l[] = { 5 * 4096 };
        CHECK(build(a, l, 1, 3, &b) == MSHC_IDMAC_E_FULL, "overflow: %d", b.Error);
        CHECK(canary_intact(3), "overflow wrote past the table");
    }
    {
        /* an error is sticky: later elements are not described */
        MSHC_U64 a[] = { 0x40000000ULL, 0x40000003ULL, 0x40002000ULL };
        MSHC_U32 l[] = { 0x1000, 0x1000, 0x1000 };
        CHECK(build(a, l, 3, 8, &b) == MSHC_IDMAC_E_ALIGN, "sticky: %d", b.Error);
        CHECK(b.Count == 1, "sticky: %u descriptors", b.Count);
    }
}

/*
 * Random transfers as the HAL would map them: a 4-aligned buffer of up to
 * 64 pages at a random page offset, split at every page boundary, with
 * physically contiguous runs merged at random.  The table must always fit
 * MshcIdmacCapacityFor(length), the bound sdport's common buffer meets.
 */
static MSHC_U32 rng = 12345;

static MSHC_U32
rnd(void)
{
    rng = rng * 1103515245U + 12345U;
    return rng >> 8;
}

static void
test_random(void)
{
    static MSHC_U64 a[TABLE_MAX];
    static MSHC_U32 l[TABLE_MAX];
    MSHC_U32 iter;

    for (iter = 0; iter < 20000; iter++) {
        MSHC_U32 offset = (rnd() % 1024) * 4;
        MSHC_U32 length = ((rnd() % (64 * 4096)) / 4 + 1) * 4;
        MSHC_U32 count = 0, done = 0;
        MSHC_U64 page = 0x40000000ULL + (MSHC_U64)(rnd() % 0x30000) * 4096;
        MSHC_IDMAC_BUILDER b;
        char name[48];

        while (done < length) {
            MSHC_U32 inpage = (done == 0) ? offset : 0;
            MSHC_U32 piece = 4096 - inpage;

            if (piece > length - done) {
                piece = length - done;
            }
            if (count > 0 && (rnd() & 1) && a[count - 1] + l[count - 1] == page + inpage) {
                l[count - 1] += piece;
            } else {
                a[count] = page + inpage;
                l[count] = piece;
                count++;
            }
            done += piece;
            /* next page: contiguous half of the time */
            page = (rnd() & 1) ? page + 4096 : 0x40000000ULL + (MSHC_U64)(rnd() % 0x30000) * 4096;
        }

        snprintf(name, sizeof(name), "random %u (off %u len %u)", iter, offset, length);
        CHECK(build(a, l, count, MshcIdmacCapacityFor(length), &b) == MSHC_IDMAC_OK,
              "%s refused (%d)", name, b.Error);
        if (b.Error != MSHC_IDMAC_OK) {
            continue;
        }
        CHECK(b.Bytes == length, "%s: %u bytes", name, b.Bytes);
        check_table(name, b.Count, a, l, count);
        if (failures > 20) {
            return;
        }
    }
}

static void
test_fifoth(void)
{
    /* mainline dw_mci_adjust_fifoth, 256-word FIFO (RK3576 sdmmc and sdio) */
    CHECK(MshcIdmacFifoth(256, 512) == MSHC_FIFOTH_VALUE(6, 127, 128), "512: 0x%08x", MshcIdmacFifoth(256, 512));
    CHECK(MshcIdmacFifoth(256, 64) == MSHC_FIFOTH_VALUE(3, 15, 128), "64: 0x%08x", MshcIdmacFifoth(256, 64));
    CHECK(MshcIdmacFifoth(256, 2048) == MSHC_FIFOTH_VALUE(6, 127, 128), "2048: 0x%08x", MshcIdmacFifoth(256, 2048));
    CHECK(MshcIdmacFifoth(256, 8) == MSHC_FIFOTH_VALUE(0, 1, 128), "8: 0x%08x", MshcIdmacFifoth(256, 8));
    CHECK(MshcIdmacFifoth(256, 16) == MSHC_FIFOTH_VALUE(1, 3, 128), "16: 0x%08x", MshcIdmacFifoth(256, 16));
    CHECK(MshcIdmacFifoth(256, 6) == MSHC_FIFOTH_VALUE(0, 1, 128), "6: 0x%08x", MshcIdmacFifoth(256, 6));
    CHECK(MshcIdmacFifoth(256, 0) == MSHC_FIFOTH_VALUE(0, 1, 128), "0: 0x%08x", MshcIdmacFifoth(256, 0));
    CHECK(MshcIdmacFifoth(128, 512) == MSHC_FIFOTH_VALUE(5, 63, 64), "fifo 128: 0x%08x", MshcIdmacFifoth(128, 512));
}

static void
test_capacity(void)
{
    CHECK(MshcIdmacCapacityFor(1) == 2, "cap 1");
    CHECK(MshcIdmacCapacityFor(4096) == 2, "cap 4096");
    CHECK(MshcIdmacCapacityFor(4097) == 3, "cap 4097");
    CHECK(MshcIdmacCapacityFor(65536) == 17, "cap 64K");
    CHECK(sizeof(MSHC_IDMAC_DESCRIPTOR) == 16, "descriptor is %u bytes", (unsigned)sizeof(MSHC_IDMAC_DESCRIPTOR));
}

int
main(void)
{
    test_single_block();
    test_sizes();
    test_chaining();
    test_4g_boundary();
    test_errors();
    test_capacity();
    test_fifoth();
    test_random();

    printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "OK", checks, failures);
    return failures ? 1 : 0;
}
