/*
 * hcdcheck - walk a Broadcom .hcd patchram file with the driver's own
 * walker (drivers/bluetooth/bcm4345bt/hcd.c) and print what the driver
 * would send. CI runs it against the pinned BCM4345C5.hcd files.
 *
 *   hcdcheck FILE [--records N]   exit 0 if the driver would accept FILE
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../drivers/bluetooth/bcm4345bt/hcd.h"

static void
print_strings(const unsigned char *b, unsigned long n)
{
    unsigned long i, start = 0, run = 0;

    for (i = 0; i <= n; i++) {
        if (i < n && b[i] >= 0x20 && b[i] < 0x7f) {
            if (run++ == 0) {
                start = i;
            }
            continue;
        }
        if (run >= 16) {
            printf("  string: %.*s\n", (int)run, (const char *)b + start);
        }
        run = 0;
    }
}

int
main(int argc, char **argv)
{
    FILE *f;
    unsigned char *buf;
    long size;
    HCD_SUMMARY s;
    HCD_RECORD rec;
    unsigned long off = 0, expect = 0, n = 0;
    int r;

    if (argc != 2 && !(argc == 4 && strcmp(argv[2], "--records") == 0)) {
        fprintf(stderr, "usage: %s FILE [--records N]\n", argv[0]);
        return 2;
    }
    if (argc == 4) {
        expect = strtoul(argv[3], NULL, 0);
    }
    f = fopen(argv[1], "rb");
    if (f == NULL) {
        perror(argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 256 * 1024) {
        fprintf(stderr, "%s: size %ld outside 1..262144 (driver limit)\n", argv[1], size);
        return 1;
    }
    buf = malloc((size_t)size);
    if (buf == NULL || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "%s: read failed\n", argv[1]);
        return 2;
    }
    fclose(f);

    r = HcdValidate(buf, (unsigned long)size, &s);
    printf("%s: %ld bytes, %lu records (%lu Write RAM, %lu bytes; %lu other), %s\n",
           argv[1], size, s.Records, s.WriteRam, s.Bytes, s.Other,
           s.EndsWithLaunch ? "ends with Launch RAM" : "NO Launch RAM at the end");
    while (HcdNext(buf, (unsigned long)size, &off, &rec) == 1) {
        if (n == 0 || rec.Opcode != HCD_OP_WRITE_RAM) {
            printf("  record %lu: opcode 0x%04x, %u bytes\n", n, rec.Opcode, rec.Length);
        }
        n++;
    }
    print_strings(buf, (unsigned long)size);
    if (r != 0) {
        printf("REJECTED (%d)\n", r);
        return 1;
    }
    if (expect != 0 && expect != s.Records) {
        printf("REJECTED: expected %lu records\n", expect);
        return 1;
    }
    printf("ok\n");
    return 0;
}
