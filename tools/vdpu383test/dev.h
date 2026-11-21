/*
 * dev.h - vdpu383.sys from user mode, or a null device that only
 * allocates memory (for running the parser and register builder on a
 * machine without the decoder, including Linux).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef VDPU383TEST_DEV_H
#define VDPU383TEST_DEV_H

#include <stddef.h>
#include <stdint.h>

#ifndef _WIN32
#define V383_IOCTL_STANDALONE 1
#endif
#include "../../drivers/video/vdpu383/vdpu383_ioctl.h"

typedef struct DevBuf {
    unsigned handle;
    uint32_t dma;
    uint32_t size;
    uint8_t *cpu;
} DevBuf;

int  dev_open(int null_device);
void dev_close(void);
int  dev_is_null(void);
int  dev_info(VDPU383_INFO *info);
int  dev_warmup(VDPU383_WARMUP_RESULT *r);
int  dev_reset(unsigned *status);
int  dev_read_regs(unsigned offset, unsigned count, unsigned *out);
int  dev_write_regs(const VDPU383_REG_OP *ops, unsigned count);
int  dev_alloc(uint32_t size, DevBuf *b);
void dev_free(DevBuf *b);
int  dev_run(const VDPU383_RUN_IN *in, VDPU383_RUN_OUT *out);
const char *dev_error(void);

#endif
