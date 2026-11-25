/*
 * rkeeprom_ioctl.h - the interface of rkeeprom.sys, for tools\eeprom
 *
 * MIT, unlike the driver (GPL-2.0-only): it is an interface, constants and
 * structures only.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef RKEEPROM_IOCTL_H
#define RKEEPROM_IOCTL_H

/* \\.\RKEEPROM0 */
#define RKEEPROM_DOS_NAME        L"\\DosDevices\\RKEEPROM0"
#define RKEEPROM_USER_PATH       L"\\\\.\\RKEEPROM0"

#define RKEEPROM_INTERFACE_VERSION  1

#ifndef CTL_CODE
#define CTL_CODE(t, f, m, a) (((t) << 16) | ((a) << 14) | ((f) << 2) | (m))
#define METHOD_BUFFERED   0
#define FILE_ANY_ACCESS   0
#define FILE_READ_ACCESS  1
#endif

#define RKEEPROM_DEVICE_TYPE     0x8E2Eu

/* out RKEEPROM_INFO */
#define IOCTL_RKEEPROM_GET_INFO  CTL_CODE(RKEEPROM_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* in RKEEPROM_READ, out the bytes */
#define IOCTL_RKEEPROM_READ      CTL_CODE(RKEEPROM_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)

/*
 * A 24C16 answers on eight addresses, 256 bytes each (the block number is
 * the low three address bits); the firmware lists them in _CRS in order.
 */
typedef struct _RKEEPROM_INFO {
    unsigned int Size;              /* sizeof(RKEEPROM_INFO) */
    unsigned int Version;           /* RKEEPROM_INTERFACE_VERSION */
    unsigned int Bytes;             /* Blocks * BlockBytes */
    unsigned int Blocks;            /* I2C addresses in _CRS */
    unsigned int BlockBytes;        /* 256 */
    unsigned int Reads;
    unsigned int Errors;
} RKEEPROM_INFO;

typedef struct _RKEEPROM_READ {
    unsigned int Offset;
    unsigned int Length;            /* at most 4096 */
} RKEEPROM_READ;

#define RKEEPROM_MAX_READ        4096

#endif /* RKEEPROM_IOCTL_H */
