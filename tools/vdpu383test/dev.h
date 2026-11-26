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

/*
 * The null device as one of vdpu383.sys's sessions (vdpu383_check.h): every
 * buffer belongs to the session that allocated it, a decode goes through
 * the driver's session rules (v383_session_sanitize_run) and its address
 * check against the current session's buffers and imports, and refused
 * decodes come back as the driver returns them. For CI and `sessiontest`.
 */
void dev_null_sessions(int on);
void dev_null_select(unsigned session);          /* 1, 2, ...: the caller from now on */
void dev_null_close(unsigned session);           /* its buffers and imports go */
unsigned dev_null_refused(void);                 /* decodes the check refused */
const VDPU383_RUN_IN *dev_null_last_run(void);   /* the last decode as the driver saw it */
/* the display driver's side: an allocation of pages pages exported to a
 * session (a token for it alone), and the pages taken back */
uint64_t dev_null_export(unsigned session, unsigned pages);
void dev_null_revoke(uint64_t token);
/* IOCTL_VDPU383_IMPORT from the current session; handle != 0 refreshes */
int dev_import(uint64_t token, unsigned pages, unsigned handle, VDPU383_IMPORT_OUT *out);

#endif
