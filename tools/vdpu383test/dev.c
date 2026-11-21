/*
 * dev.c - see dev.h.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: MIT
 */

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dev.h"

static int  g_null;
static char g_err[256];
static uint32_t g_fake_dma = 0x10000000;

#ifdef _WIN32
static HANDLE g_dev = INVALID_HANDLE_VALUE;

static int ioctl(DWORD code, const void *in, DWORD in_len, void *out, DWORD out_len, DWORD *got)
{
    DWORD n = 0;
    if (!DeviceIoControl(g_dev, code, (void *)in, in_len, out, out_len, &n, NULL)) {
        snprintf(g_err, sizeof(g_err), "DeviceIoControl 0x%08lx failed: error %lu", code, GetLastError());
        return -1;
    }
    if (got)
        *got = n;
    return 0;
}
#endif

const char *dev_error(void)
{
    return g_err;
}

int dev_is_null(void)
{
    return g_null;
}

int dev_open(int null_device)
{
    g_null = null_device;
    if (null_device)
        return 0;
#ifdef _WIN32
    g_dev = CreateFileW(VDPU383_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (g_dev == INVALID_HANDLE_VALUE) {
        snprintf(g_err, sizeof(g_err),
                 "cannot open \\\\.\\VDPU383: error %lu (driver not installed or not started, "
                 "or not run as administrator)", GetLastError());
        return -1;
    }
    return 0;
#else
    snprintf(g_err, sizeof(g_err), "no device on this platform; use --null");
    return -1;
#endif
}

void dev_close(void)
{
#ifdef _WIN32
    if (g_dev != INVALID_HANDLE_VALUE)
        CloseHandle(g_dev);
    g_dev = INVALID_HANDLE_VALUE;
#endif
}

int dev_info(VDPU383_INFO *info)
{
    memset(info, 0, sizeof(*info));
    if (g_null)
        return 0;
#ifdef _WIN32
    return ioctl(IOCTL_VDPU383_GET_INFO, NULL, 0, info, sizeof(*info), NULL);
#else
    return -1;
#endif
}

int dev_warmup(VDPU383_WARMUP_RESULT *r)
{
    memset(r, 0, sizeof(*r));
    if (g_null)
        return 0;
#ifdef _WIN32
    return ioctl(IOCTL_VDPU383_WARMUP, NULL, 0, r, sizeof(*r), NULL);
#else
    return -1;
#endif
}

int dev_reset(unsigned *status)
{
    *status = 0;
    if (g_null)
        return 0;
#ifdef _WIN32
    return ioctl(IOCTL_VDPU383_RESET, NULL, 0, status, sizeof(*status), NULL);
#else
    return -1;
#endif
}

int dev_read_regs(unsigned offset, unsigned count, unsigned *out)
{
    VDPU383_READ_REGS_IN in;

    if (g_null) {
        memset(out, 0, count * 4);
        return 0;
    }
    in.offset = offset;
    in.count = count;
#ifdef _WIN32
    return ioctl(IOCTL_VDPU383_READ_REGS, &in, sizeof(in), out, count * 4, NULL);
#else
    (void)in;
    return -1;
#endif
}

int dev_write_regs(const VDPU383_REG_OP *ops, unsigned count)
{
    size_t len = sizeof(VDPU383_WRITE_REGS_IN) + (count ? count - 1 : 0) * sizeof(VDPU383_REG_OP);
    VDPU383_WRITE_REGS_IN *in;
    int rc = 0;

    if (g_null)
        return 0;
    in = (VDPU383_WRITE_REGS_IN *)calloc(1, len);
    if (!in)
        return -1;
    in->count = count;
    memcpy(in->ops, ops, count * sizeof(*ops));
#ifdef _WIN32
    rc = ioctl(IOCTL_VDPU383_WRITE_REGS, in, (DWORD)len, NULL, 0, NULL);
#else
    rc = -1;
#endif
    free(in);
    return rc;
}

int dev_alloc(uint32_t size, DevBuf *b)
{
    memset(b, 0, sizeof(*b));
    size = (size + 4095) & ~4095u;
    if (g_null) {
        b->cpu = (uint8_t *)calloc(1, size);
        if (!b->cpu)
            return -1;
        b->dma = g_fake_dma;
        b->size = size;
        b->handle = 1;
        g_fake_dma += (size + 0xFFFF) & ~0xFFFFu;
        return 0;
    }
#ifdef _WIN32
    {
        VDPU383_ALLOC_IN in;
        VDPU383_ALLOC_OUT out;

        in.size = size;
        memset(&out, 0, sizeof(out));
        if (ioctl(IOCTL_VDPU383_ALLOC, &in, sizeof(in), &out, sizeof(out), NULL))
            return -1;
        b->handle = out.handle;
        b->dma = out.dma;
        b->size = out.size;
        b->cpu = (uint8_t *)(uintptr_t)out.user_va;
        return 0;
    }
#else
    return -1;
#endif
}

void dev_free(DevBuf *b)
{
    if (!b->handle)
        return;
    if (g_null) {
        free(b->cpu);
    } else {
#ifdef _WIN32
        VDPU383_FREE_IN in;
        in.handle = b->handle;
        ioctl(IOCTL_VDPU383_FREE, &in, sizeof(in), NULL, 0, NULL);
#endif
    }
    memset(b, 0, sizeof(*b));
}

int dev_run(const VDPU383_RUN_IN *in, VDPU383_RUN_OUT *out)
{
    memset(out, 0, sizeof(*out));
    if (g_null) {
        out->result = VDPU383_RUN_OK;
        out->status = V383_STA_FRAME_RDY;
        return 0;
    }
#ifdef _WIN32
    return ioctl(IOCTL_VDPU383_RUN, in, sizeof(*in), out, sizeof(*out), NULL);
#else
    (void)in;
    return -1;
#endif
}
