/*
 * dev.c - see dev.h.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dev.h"
#include "../../drivers/video/vdpu383/vdpu383_check.h"

static int  g_null;
static char g_err[256];
static uint32_t g_fake_dma = 0x10000000;

/* the null device's buffers and imports, for its session mode */
#define NULL_BUFS 4096
typedef struct NullBuf {
    unsigned handle, dma, size, session;
    int used, import, dead;
    uint64_t token;
} NullBuf;
typedef struct NullExport {
    uint64_t token;
    unsigned session, pages;
    int used, revoked;
    unsigned import_handle;         /* the import holding it, 0 for none */
} NullExport;
static NullBuf g_nb[NULL_BUFS];
static NullExport g_nx[256];
static int g_sessions;
static unsigned g_session = 1, g_next_handle, g_refused;
static uint64_t g_next_token = 0x7000;
static VDPU383_RUN_IN g_last_run;

static NullBuf *null_slot(void)
{
    unsigned i;
    for (i = 0; i < NULL_BUFS; i++)
        if (!g_nb[i].used) {
            memset(&g_nb[i], 0, sizeof(g_nb[i]));
            g_nb[i].used = 1;
            return &g_nb[i];
        }
    return NULL;
}

static NullBuf *null_find(unsigned handle)
{
    unsigned i;
    for (i = 0; i < NULL_BUFS; i++)
        if (g_nb[i].used && g_nb[i].handle == handle && g_nb[i].session == g_session)
            return &g_nb[i];
    return NULL;
}

/* driver.c V383AddrCheck: the current session's buffers and imports only */
static unsigned null_lookup(void *ctx, unsigned addr)
{
    unsigned i;
    (void)ctx;
    for (i = 0; i < NULL_BUFS; i++) {
        const NullBuf *b = &g_nb[i];
        if (b->used && b->session == g_session && addr >= b->dma && (uint64_t)addr < (uint64_t)b->dma + b->size)
            return b->dead ? V383_ADDR_DEAD : V383_ADDR_OK;
    }
    return V383_ADDR_BAD;
}

void dev_null_sessions(int on) { g_sessions = on; }
void dev_null_select(unsigned session) { g_session = session; }
unsigned dev_null_refused(void) { return g_refused; }
const VDPU383_RUN_IN *dev_null_last_run(void) { return &g_last_run; }

void dev_null_close(unsigned session)
{
    unsigned i;
    for (i = 0; i < NULL_BUFS; i++)
        if (g_nb[i].used && g_nb[i].session == session)
            g_nb[i].used = 0;
    for (i = 0; i < 256; i++)
        if (g_nx[i].used && g_nx[i].session == session)
            g_nx[i].import_handle = 0;
}

uint64_t dev_null_export(unsigned session, unsigned pages)
{
    unsigned i;
    for (i = 0; i < 256; i++)
        if (!g_nx[i].used) {
            memset(&g_nx[i], 0, sizeof(g_nx[i]));
            g_nx[i].used = 1;
            g_nx[i].token = ++g_next_token;
            g_nx[i].session = session;
            g_nx[i].pages = pages;
            return g_nx[i].token;
        }
    return 0;
}

/* maliwddm MwVdecGone: the importer's revoke routine, then the export ends */
void dev_null_revoke(uint64_t token)
{
    unsigned i, k;
    for (i = 0; i < 256; i++)
        if (g_nx[i].used && g_nx[i].token == token) {
            for (k = 0; k < NULL_BUFS; k++)
                if (g_nb[k].used && g_nb[k].import && g_nb[k].handle == g_nx[i].import_handle)
                    g_nb[k].dead = 1;
            g_nx[i].used = 0;
        }
}


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
        NullBuf *n = null_slot();
        b->cpu = (uint8_t *)calloc(1, size);
        if (!b->cpu || !n) {
            free(b->cpu);
            b->cpu = NULL;
            if (n)
                n->used = 0;
            return -1;
        }
        b->dma = g_fake_dma;
        b->size = size;
        b->handle = ++g_next_handle;
        n->handle = b->handle;
        n->dma = b->dma;
        n->size = size;
        n->session = g_session;
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
        NullBuf *n = null_find(b->handle);
        /* another session's handle is not this one's to free (driver.c
         * V383Free: STATUS_INVALID_HANDLE); the memory stays its owner's */
        if (!n && g_sessions) {
            snprintf(g_err, sizeof(g_err), "free: no such handle in this session");
            return;
        }
        if (n)
            n->used = 0;
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
        if (g_sessions) {
            /* driver.c V383RunGated for a session */
            unsigned bad = 0;
            g_last_run = *in;
            v383_session_sanitize_run(&g_last_run);
            out->result = v383_check_addrs(g_last_run.regs, g_last_run.write_mask, null_lookup, NULL, &bad);
            out->bad_reg = bad;
            if (out->result != VDPU383_RUN_OK) {
                out->status = 0;
                g_refused++;
            }
        }
        return 0;
    }
#ifdef _WIN32
    return ioctl(IOCTL_VDPU383_RUN, in, sizeof(*in), out, sizeof(*out), NULL);
#else
    (void)in;
    return -1;
#endif
}

/* driver.c V383Import with maliwddm's VdecCallback as the exporter */
int dev_import(uint64_t token, unsigned pages, unsigned handle, VDPU383_IMPORT_OUT *out)
{
    NullExport *x = NULL;
    NullBuf *b;
    unsigned i;

    memset(out, 0, sizeof(*out));
    if (!g_null) {
#ifdef _WIN32
        VDPU383_IMPORT_IN in;
        in.token = token;
        in.pages = pages;
        in.handle = handle;
        return ioctl(IOCTL_VDPU383_IMPORT, &in, sizeof(in), out, sizeof(*out), NULL);
#else
        return -1;
#endif
    }
    for (i = 0; i < 256; i++)
        if (g_nx[i].used && g_nx[i].token == token)
            x = &g_nx[i];
    /* no such export, another session's (process's), imported already */
    if (!x || x->session != g_session || (x->import_handle && x->import_handle != handle) || x->pages != pages) {
        snprintf(g_err, sizeof(g_err), "import refused by the exporter");
        return -1;
    }
    if (handle) {
        b = null_find(handle);
        if (!b || !b->import || b->size != pages * 4096u)
            return -1;
        b->dead = 0;
        b->token = token;
    } else {
        b = null_slot();
        if (!b)
            return -1;
        b->handle = ++g_next_handle;
        b->dma = g_fake_dma;
        b->size = pages * 4096u;
        b->session = g_session;
        b->import = 1;
        b->token = token;
        g_fake_dma += (b->size + 0xFFFF) & ~0xFFFFu;
    }
    x->import_handle = b->handle;
    out->handle = b->handle;
    out->dma = b->dma;
    out->size = b->size;
    out->flags = VDPU383_ALLOC_IOVA | VDPU383_ALLOC_IMPORTED;
    return 0;
}
