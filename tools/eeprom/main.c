/*
 * eeprom.exe - read the board's I2C EEPROM through rkeeprom.sys
 *
 *   eeprom info              size, and the board information if the contents
 *                            are in a format this knows (the default command)
 *   eeprom dump [FILE]       hex dump of the whole part, or the raw bytes
 *                            written to FILE
 *   eeprom read OFF LEN      hex dump of LEN bytes at OFF (0x.. accepted)
 *
 * Read-only: there is no write command, and the driver has no write path.
 * Run it as administrator (the device is SYSTEM and Administrators only).
 *
 * Known formats, looked for at every 256-byte block:
 *   "RADX"  Radxa's ROCK EEPROM, the Raspberry Pi HAT layout (12-byte
 *           header, then atoms of type/count/dlen/data/CRC-16); atoms 1
 *           (vendor: UUID, product id and version, vendor and product
 *           strings), 4 (BOM revision) and 5 (PCB revision, MAC addresses),
 *           as Radxa's U-Boot radxa-i2c-eeprom.c reads them;
 *   "R-Pi"  a Raspberry Pi HAT EEPROM, same layout, atom 1 only.
 * Anything else is shown as a hex dump only.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <windows.h>
#include <winioctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "..\..\drivers\misc\rkeeprom\rkeeprom_ioctl.h"

static HANDLE g_dev = INVALID_HANDLE_VALUE;

static int Open(RKEEPROM_INFO *info)
{
    DWORD got = 0;

    g_dev = CreateFileW(RKEEPROM_USER_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
    if (g_dev == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        fprintf(stderr, "cannot open %ls (error %lu)%s\n", RKEEPROM_USER_PATH, e,
                e == ERROR_ACCESS_DENIED ? ": run as administrator" :
                e == ERROR_FILE_NOT_FOUND ? ": is rkeeprom installed, and does the firmware declare the EEPROM?" : "");
        return 0;
    }
    memset(info, 0, sizeof(*info));
    if (!DeviceIoControl(g_dev, IOCTL_RKEEPROM_GET_INFO, NULL, 0, info, sizeof(*info), &got, NULL) ||
        got < sizeof(*info) || info->Version != RKEEPROM_INTERFACE_VERSION) {
        fprintf(stderr, "GET_INFO failed (error %lu, version %u)\n", GetLastError(), info->Version);
        return 0;
    }
    return 1;
}

static int ReadBytes(unsigned int off, unsigned int len, unsigned char *buf)
{
    while (len != 0) {
        RKEEPROM_READ req;
        DWORD got = 0;
        unsigned int n = len > RKEEPROM_MAX_READ ? RKEEPROM_MAX_READ : len;

        req.Offset = off;
        req.Length = n;
        if (!DeviceIoControl(g_dev, IOCTL_RKEEPROM_READ, &req, sizeof(req), buf, n, &got, NULL) ||
            got != n) {
            fprintf(stderr, "read of %u bytes at 0x%X failed (error %lu)\n", n, off, GetLastError());
            return 0;
        }
        off += n;
        buf += n;
        len -= n;
    }
    return 1;
}

static void Hex(unsigned int base, const unsigned char *p, unsigned int len)
{
    unsigned int i, j;

    for (i = 0; i < len; i += 16) {
        printf("%04X: ", base + i);
        for (j = 0; j < 16; j++) {
            if (i + j < len) {
                printf("%02X ", p[i + j]);
            } else {
                printf("   ");
            }
        }
        printf(" ");
        for (j = 0; j < 16 && i + j < len; j++) {
            unsigned char c = p[i + j];
            putchar(c >= 0x20 && c < 0x7F ? c : '.');
        }
        putchar('\n');
    }
}

/* The Raspberry Pi HAT CRC-16 (polynomial 0x8005, reflected), as eepmake. */
static unsigned short Crc16(const unsigned char *data, unsigned int size)
{
    unsigned short out = 0, crc = 0;
    int bitsRead = 0, bitFlag, i;
    unsigned int j;

    while (size > 0) {
        bitFlag = out >> 15;
        out = (unsigned short)(out << 1);
        out = (unsigned short)(out | ((*data >> bitsRead) & 1));
        bitsRead++;
        if (bitsRead > 7) {
            bitsRead = 0;
            data++;
            size--;
        }
        if (bitFlag) {
            out = (unsigned short)(out ^ 0x8005);
        }
    }
    for (i = 0; i < 16; i++) {
        bitFlag = out >> 15;
        out = (unsigned short)(out << 1);
        if (bitFlag) {
            out = (unsigned short)(out ^ 0x8005);
        }
    }
    for (i = 0x8000, j = 1; i != 0; i >>= 1, j <<= 1) {
        if (i & out) {
            crc = (unsigned short)(crc | j);
        }
    }
    return crc;
}

static unsigned int Le16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned int Le32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24); }

static void PrintString(const char *label, const unsigned char *p, unsigned int max)
{
    unsigned int i;

    printf("  %-14s ", label);
    for (i = 0; i < max && p[i] != 0; i++) {
        putchar(p[i] >= 0x20 && p[i] < 0x7F ? p[i] : '?');
    }
    putchar('\n');
}

/* The HAT-style layout at p (len bytes available).  Returns 1 if parsed. */
static int ParseHat(const unsigned char *p, unsigned int len, unsigned int base)
{
    unsigned int version = p[4], atoms = Le16(p + 6), eeplen = Le32(p + 8);
    unsigned int pos = 12, a;

    printf("%.4s EEPROM at 0x%03X: format version %u, %u atoms, %u bytes\n",
           (const char *)p, base, version, atoms, eeplen);
    if (eeplen > len) {
        printf("  (declared length runs past the part; reading what is there)\n");
        eeplen = len;
    }
    for (a = 0; a < atoms; a++) {
        unsigned int type, count, dlen;
        const unsigned char *d;
        unsigned short crc;

        if (pos + 8 > eeplen) {
            printf("  atom %u: header runs past the data\n", a);
            return 1;
        }
        type = Le16(p + pos);
        count = Le16(p + pos + 2);
        dlen = Le32(p + pos + 4);
        if (dlen < 2 || pos + 8 + dlen > eeplen) {
            printf("  atom %u (type %u): length %u runs past the data\n", a, type, dlen);
            return 1;
        }
        d = p + pos + 8;
        crc = Crc16(p + pos, 8 + dlen - 2);
        printf(" atom %u: type 0x%04X, %u data bytes, CRC %s\n", count, type, dlen - 2,
               crc == Le16(d + dlen - 2) ? "ok" : "BAD");

        if (type == 1 && dlen >= 22) {
            unsigned int vslen = d[20], pslen = d[21], i;

            printf("  %-14s ", "uuid bytes");
            for (i = 0; i < 16; i++) {
                printf("%02x", d[i]);
            }
            printf("\n  %-14s 0x%04X\n  %-14s 0x%04X\n", "product id", Le16(d + 16),
                   "product ver", Le16(d + 18));
            if (memcmp(p, "RADX", 4) == 0) {
                /* Radxa: fixed 32-byte vendor and product fields. */
                if (dlen >= 22 + 64) {
                    PrintString("vendor", d + 22, 32);
                    PrintString("product / SN", d + 22 + 32, 32);
                }
            } else if (22 + vslen + pslen <= dlen) {
                PrintString("vendor", d + 22, vslen);
                PrintString("product", d + 22 + vslen, pslen);
            }
        } else if (type == 4 && memcmp(p, "RADX", 4) == 0 && dlen >= 2 + 2 + 8) {
            printf("  %-14s %u\n", "data version", Le16(d));
            PrintString("BOM revision", d + 2, 8);
        } else if (type == 5 && memcmp(p, "RADX", 4) == 0 && dlen >= 2 + 2 + 20) {
            unsigned int i, n = (dlen - 2 - 2 - 20) / 6;

            printf("  %-14s %u\n", "data version", Le16(d));
            PrintString("PCB revision", d + 2, 20);
            for (i = 0; i < n && i < 10; i++) {
                const unsigned char *m = d + 2 + 20 + i * 6;

                if ((m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) == 0) {
                    continue;
                }
                printf("  ethernet%u MAC   %02x:%02x:%02x:%02x:%02x:%02x\n", i,
                       m[0], m[1], m[2], m[3], m[4], m[5]);
            }
        }
        pos += 8 + dlen;
    }
    return 1;
}

static int CmdInfo(const RKEEPROM_INFO *info)
{
    unsigned char *buf = (unsigned char *)malloc(info->Bytes);
    unsigned int off;
    int found = 0, blank = 1;

    printf("EEPROM: %u bytes (%u blocks of %u), %u reads and %u errors so far\n",
           info->Bytes, info->Blocks, info->BlockBytes, info->Reads, info->Errors);
    if (buf == NULL || !ReadBytes(0, info->Bytes, buf)) {
        free(buf);
        return 1;
    }
    for (off = 0; off < info->Bytes; off++) {
        if (buf[off] != 0xFF) {
            blank = 0;
            break;
        }
    }
    if (blank) {
        printf("every byte is 0xFF: the part has never been written\n");
        free(buf);
        return 0;
    }
    for (off = 0; off + 12 <= info->Bytes; off += info->BlockBytes) {
        if (memcmp(buf + off, "RADX", 4) == 0 || memcmp(buf + off, "R-Pi", 4) == 0) {
            found |= ParseHat(buf + off, info->Bytes - off, off);
        }
    }
    if (!found) {
        printf("no known format (RADX, R-Pi); the first 256 bytes:\n");
        Hex(0, buf, info->Bytes < 256 ? info->Bytes : 256);
    }
    free(buf);
    return 0;
}

static int CmdDump(const RKEEPROM_INFO *info, const char *file)
{
    unsigned char *buf = (unsigned char *)malloc(info->Bytes);
    int rc = 1;

    if (buf != NULL && ReadBytes(0, info->Bytes, buf)) {
        if (file != NULL) {
            FILE *f = fopen(file, "wb");

            if (f != NULL && fwrite(buf, 1, info->Bytes, f) == info->Bytes) {
                printf("%u bytes written to %s\n", info->Bytes, file);
                rc = 0;
            } else {
                fprintf(stderr, "cannot write %s\n", file);
            }
            if (f != NULL) {
                fclose(f);
            }
        } else {
            Hex(0, buf, info->Bytes);
            rc = 0;
        }
    }
    free(buf);
    return rc;
}

static int CmdRead(const RKEEPROM_INFO *info, const char *a, const char *b)
{
    unsigned long off = strtoul(a, NULL, 0), len = strtoul(b, NULL, 0);
    unsigned char *buf;
    int rc = 1;

    if (len == 0 || off >= info->Bytes || len > info->Bytes - off) {
        fprintf(stderr, "out of range: the part has %u bytes\n", info->Bytes);
        return 1;
    }
    buf = (unsigned char *)malloc(len);
    if (buf != NULL && ReadBytes((unsigned int)off, (unsigned int)len, buf)) {
        Hex((unsigned int)off, buf, (unsigned int)len);
        rc = 0;
    }
    free(buf);
    return rc;
}

int main(int argc, char **argv)
{
    RKEEPROM_INFO info;
    const char *cmd = argc > 1 ? argv[1] : "info";
    int rc;

    if (strcmp(cmd, "info") != 0 && strcmp(cmd, "dump") != 0 &&
        !(strcmp(cmd, "read") == 0 && argc == 4)) {
        fprintf(stderr, "usage: eeprom info | dump [FILE] | read OFFSET LENGTH\n");
        return 2;
    }
    if (!Open(&info)) {
        return 1;
    }
    if (strcmp(cmd, "dump") == 0) {
        rc = CmdDump(&info, argc > 2 ? argv[2] : NULL);
    } else if (strcmp(cmd, "read") == 0) {
        rc = CmdRead(&info, argv[2], argv[3]);
    } else {
        rc = CmdInfo(&info);
    }
    CloseHandle(g_dev);
    return rc;
}
