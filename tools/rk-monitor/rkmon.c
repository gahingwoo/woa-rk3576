/*
 * rk-monitor - a tray monitor for RK3576 boards under Windows on Arm
 *
 * A tray icon (the hottest ACPI thermal zone, in degrees) with a tooltip,
 * and a small popup with CPU, GPU and NPU state, refreshed every second.
 * Everything comes from performance counters through PDH:
 *
 *   CPU  \Processor(*)\% Processor Time, per cluster (A53 cores 0-3, A72
 *        cores 4-7, or the efficiency classes Windows reports);
 *        \Processor Information(*)\Processor Frequency;
 *        \Thermal Zone Information(*)\High Precision Temperature (tenths of
 *        a kelvin; WMI's MSAcpi_ThermalZoneTemperature is a stale cache on
 *        these boards, the counter is not)
 *   GPU  \GPU Engine(*)\Utilization Percentage, summed per engine and shown
 *        per engine type, as Task Manager does; \GPU Adapter Memory(*);
 *        \RK3576 GPU\Temperature (C) and Clock (MHz) from maliwddm
 *   NPU  \RK3576 NPU(*)\% Busy Time and Jobs/sec from rknpu.sys
 *
 * A counter set that is not there is shown as such ("not reported", "driver
 * has no counters") and looked for again every 30 seconds.
 *
 * The desktop widget (tray menu, "Desktop widget") is a small panel on the
 * desktop itself: a child of the window that holds the desktop icons, so it
 * stays below every application and stays visible on Win+D. Semi-transparent
 * (a per-pixel alpha layered window), draggable, its position remembered,
 * optionally click-through; it is redrawn only when its text or graphs change.
 *
 *   rk-monitor.exe              tray icon; left click: the popup, right click:
 *                               the menu (desktop widget, start with Windows, exit)
 *   rk-monitor.exe --autostart  the same, started at logon: exits quietly if
 *                               one is already running; shows "..." and takes
 *                               its first sample after 5 s, and runs in
 *                               background mode for its first minute
 *   rk-monitor.exe --once       print one sample as text and exit
 *
 * Tray menu, auto-switch to headphones (on by default; the item's label is
 * Chinese, "zi dong qie huan er ji", written as \u escapes in MenuShow):
 * when a plug goes into the board's headphone jack the headphones become the
 * default playback device, and the previous one comes back on unplug
 * (audioswitch.cpp; it uses an undocumented interface, see there).
 *
 * Settings: HKCU\Software\RK3576\Monitor (Widget, WidgetClickThrough,
 * WidgetX, WidgetY, AutoSwitchHeadphones); a value missing there is read from
 * HKLM\Software\RK3576\Monitor, which the installer sets (Widget = 1).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "audioswitch.h"

#define APP_NAME        L"RK3576 Monitor"
#define APP_CLASS       L"Rk3576MonitorPopup"
#define APP_MUTEX       L"Local\\Rk3576MonitorSingleInstance"
#define RUN_KEY         L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE       L"RK3576 Monitor"

#define WM_TRAY         (WM_APP + 1)
#define WM_SHOWPOPUP    (WM_APP + 2)
#define WM_AUDIOSWITCH  (WM_APP + 3)     /* an audio endpoint changed state */
#define TIMER_SAMPLE    1
#define SAMPLE_MS       1000
#define RETRY_TICKS     30
#define TRAY_ID         1

#define IDM_SHOW        100
#define IDM_AUTOSTART   101
#define IDM_EXIT        102
#define IDM_WIDGET      103
#define IDM_CLICKTHRU   104
#define IDM_AUDIOSWITCH 105

#define SET_KEY         L"Software\\RK3576\\Monitor"
#define WIDGET_CLASS    L"Rk3576MonitorWidget"
#define WIDGET_W        250         /* DIP */

#define HIST            60          /* sparkline samples */
#define MAX_CPU         64
#define MAX_TZ          8
#define MAX_ENG         64
#define MAX_ROWS        48
#define POPUP_W         360         /* DIP */

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#define RK_DWMWCP_ROUND 2

/* ------------------------------------------------------------ PDH */

typedef struct {
    const wchar_t *paths[2];        /* tried in order: "(*)" and plain forms */
    PDH_HCOUNTER h;
    BOOL ok;                        /* added to the query */
    BOOL wild;                      /* the path that worked has "(*)" */
} Ctr;

enum {
    C_CPU, C_FREQ, C_TZ, C_GPU_ENG, C_GPU_DED, C_GPU_SH,
    C_GPU_TEMP, C_GPU_CLK, C_GPU_BUSY, C_GPU_THR, C_NPU_BUSY, C_NPU_JOBS, C_COUNT
};

static Ctr g_ctr[C_COUNT] = {
    [C_CPU]      = { { L"\\Processor(*)\\% Processor Time" }, NULL, FALSE, FALSE },
    [C_FREQ]     = { { L"\\Processor Information(*)\\Processor Frequency" }, NULL, FALSE, FALSE },
    [C_TZ]       = { { L"\\Thermal Zone Information(*)\\High Precision Temperature" }, NULL, FALSE, FALSE },
    [C_GPU_ENG]  = { { L"\\GPU Engine(*)\\Utilization Percentage" }, NULL, FALSE, FALSE },
    [C_GPU_DED]  = { { L"\\GPU Adapter Memory(*)\\Dedicated Usage" }, NULL, FALSE, FALSE },
    [C_GPU_SH]   = { { L"\\GPU Adapter Memory(*)\\Shared Usage" }, NULL, FALSE, FALSE },
    /* maliwddm: one instance, "gpu0"; the plain form in case it becomes a single-instance set */
    [C_GPU_TEMP] = { { L"\\RK3576 GPU(*)\\Temperature (C)", L"\\RK3576 GPU\\Temperature (C)" }, NULL, FALSE, FALSE },
    [C_GPU_CLK]  = { { L"\\RK3576 GPU(*)\\Clock (MHz)", L"\\RK3576 GPU\\Clock (MHz)" }, NULL, FALSE, FALSE },
    [C_GPU_BUSY] = { { L"\\RK3576 GPU(*)\\% busy", L"\\RK3576 GPU\\% busy" }, NULL, FALSE, FALSE },
    [C_GPU_THR]  = { { L"\\RK3576 GPU(*)\\Throttled", L"\\RK3576 GPU\\Throttled" }, NULL, FALSE, FALSE },
    [C_NPU_BUSY] = { { L"\\RK3576 NPU(*)\\% Busy Time" }, NULL, FALSE, FALSE },
    [C_NPU_JOBS] = { { L"\\RK3576 NPU(*)\\Jobs/sec" }, NULL, FALSE, FALSE },
};

static PDH_HQUERY g_query;

static void CtrTryAdd(Ctr *c)
{
    int i;

    if (c->ok)
        return;
    for (i = 0; i < 2 && c->paths[i]; i++) {
        if (PdhAddEnglishCounterW(g_query, c->paths[i], 0, &c->h) == ERROR_SUCCESS) {
            c->ok = TRUE;
            c->wild = wcsstr(c->paths[i], L"(*)") != NULL;
            return;
        }
    }
}

static void CtrAddMissing(void)
{
    int i;

    for (i = 0; i < C_COUNT; i++)
        CtrTryAdd(&g_ctr[i]);
}

/* The instances of a wildcard counter, valid values only. Caller frees. */
static PDH_FMT_COUNTERVALUE_ITEM_W *CtrArray(Ctr *c, DWORD *n, DWORD fmt)
{
    DWORD bytes = 0, count = 0;
    PDH_FMT_COUNTERVALUE_ITEM_W *items;
    PDH_STATUS st;

    *n = 0;
    if (!c->ok)
        return NULL;
    st = PdhGetFormattedCounterArrayW(c->h, fmt, &bytes, &count, NULL);
    if (st != (PDH_STATUS)PDH_MORE_DATA || bytes == 0)
        return NULL;
    items = (PDH_FMT_COUNTERVALUE_ITEM_W *)malloc(bytes);
    if (!items)
        return NULL;
    if (PdhGetFormattedCounterArrayW(c->h, fmt, &bytes, &count, items) != ERROR_SUCCESS) {
        free(items);
        return NULL;
    }
    *n = count;
    return items;
}

static BOOL ItemValid(const PDH_FMT_COUNTERVALUE_ITEM_W *it)
{
    return it->FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || it->FmtValue.CStatus == PDH_CSTATUS_NEW_DATA;
}

/* A single value: the plain counter, or the largest instance of a "(*)" one. */
static BOOL CtrOne(Ctr *c, double *v)
{
    PDH_FMT_COUNTERVALUE fv;
    PDH_FMT_COUNTERVALUE_ITEM_W *it;
    DWORD n, i;
    BOOL got = FALSE;

    if (!c->ok)
        return FALSE;
    if (!c->wild) {
        if (PdhGetFormattedCounterValue(c->h, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, NULL, &fv) != ERROR_SUCCESS ||
            (fv.CStatus != PDH_CSTATUS_VALID_DATA && fv.CStatus != PDH_CSTATUS_NEW_DATA))
            return FALSE;
        *v = fv.doubleValue;
        return TRUE;
    }
    it = CtrArray(c, &n, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100);
    for (i = 0; i < n; i++) {
        if (ItemValid(&it[i]) && (!got || it[i].FmtValue.doubleValue > *v)) {
            *v = it[i].FmtValue.doubleValue;
            got = TRUE;
        }
    }
    free(it);
    return got;
}

/* ------------------------------------------------------------ model */

typedef struct {
    double v[HIST];
    int n, head;
} Series;

static void SeriesPush(Series *s, double v)
{
    s->v[s->head] = v;
    s->head = (s->head + 1) % HIST;
    if (s->n < HIST)
        s->n++;
}

typedef struct {
    wchar_t name[32];
    double celsius;
} Zone;

typedef struct {
    wchar_t label[40];
    ULONG64 mask;                   /* logical processors 0..63 of group 0 */
    double usage;
    double mhz;
    BOOL usageOk, mhzOk;
    Series hist;
} Cluster;

typedef struct {
    wchar_t name[16];
    double busy, jobs;
    BOOL busyOk, jobsOk;
    Series hist;
} NpuCore;

static struct {
    Cluster cl[2];
    int ncl;
    Zone tz[MAX_TZ];
    int ntz;
    BOOL tzOk;

    BOOL gpuOk;                     /* GPU Engine counters exist */
    double gpu;                     /* busiest engine */
    wchar_t gpuTypes[96];
    Series gpuHist;
    BOOL memOk;
    double gpuDed, gpuShared;
    BOOL gpuTempOk, gpuClkOk, gpuSetOk, gpuBusyOk, gpuThrOk;
    double gpuTemp, gpuClk, gpuBusy, gpuThr;

    BOOL npuSet;                    /* the counter set exists */
    NpuCore npu[2];
    int nnpu;

    Series cpuAll, npuAll;          /* the widget's graphs */
} M;

typedef enum { R_HEAD, R_ROW, R_NOTE } RowKind;

typedef struct {
    RowKind kind;
    wchar_t label[48];
    wchar_t value[112];
    const Series *spark;
    BOOL warn;
} Row;

static Row g_rows[MAX_ROWS];
static int g_nrows;
static wchar_t g_tip[128];
static double g_hottest;
static BOOL g_hottestOk;

/* Clusters: by efficiency class when Windows reports more than one, else
 * the RK3576 layout (cores 0-3 A53, 4-7 A72) when there are 8, else one. */
static void ClustersInit(void)
{
    BYTE buf[16384];
    DWORD len = sizeof(buf), off = 0;
    ULONG64 bycls[2] = { 0, 0 };
    BYTE lo = 255, hi = 0;
    SYSTEM_INFO si;
    int i, cpus;

    if (GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
        while (off < len) {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX p = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
            BYTE c = p->Processor.EfficiencyClass;
            if (c < lo) lo = c;
            if (c > hi) hi = c;
            off += p->Size;
        }
        off = 0;
        while (lo != hi && off < len) {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX p = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
            if (p->Processor.GroupMask[0].Group == 0)
                bycls[p->Processor.EfficiencyClass == lo ? 0 : 1] |= (ULONG64)p->Processor.GroupMask[0].Mask;
            off += p->Size;
        }
    }
    GetSystemInfo(&si);
    cpus = (int)si.dwNumberOfProcessors;
    if (lo != hi && bycls[0] && bycls[1]) {
        M.cl[0].mask = bycls[0];
        M.cl[1].mask = bycls[1];
        M.ncl = 2;
    } else if (cpus == 8) {
        M.cl[0].mask = 0x0F;
        M.cl[1].mask = 0xF0;
        M.ncl = 2;
    } else {
        M.cl[0].mask = cpus >= 64 ? ~0ull : ((1ull << cpus) - 1);
        M.ncl = 1;
    }
    for (i = 0; i < M.ncl; i++) {
        int first = -1, last = -1, b;
        for (b = 0; b < 64; b++)
            if (M.cl[i].mask & (1ull << b)) {
                if (first < 0) first = b;
                last = b;
            }
        if (M.ncl == 1)
            swprintf(M.cl[i].label, 40, L"Cores %d-%d", first, last);
        else
            swprintf(M.cl[i].label, 40, L"%ls, cores %d-%d", i == 0 ? L"A53" : L"A72", first, last);
    }
}

static int ClusterOf(int cpu)
{
    int i;

    if (cpu < 0 || cpu >= 64)
        return -1;
    for (i = 0; i < M.ncl; i++)
        if (M.cl[i].mask & (1ull << cpu))
            return i;
    return -1;
}

static void SampleCpu(void)
{
    PDH_FMT_COUNTERVALUE_ITEM_W *it;
    DWORD n, i;
    double sum[2] = { 0, 0 }, fmax[2] = { 0, 0 };
    int cnt[2] = { 0, 0 }, fcnt[2] = { 0, 0 }, k;

    it = CtrArray(&g_ctr[C_CPU], &n, PDH_FMT_DOUBLE);
    for (i = 0; i < n; i++) {
        wchar_t *end;
        long cpu;
        if (!ItemValid(&it[i]) || !iswdigit(it[i].szName[0]))
            continue;               /* "_Total" */
        cpu = wcstol(it[i].szName, &end, 10);
        k = ClusterOf((int)cpu);
        if (k >= 0) {
            sum[k] += it[i].FmtValue.doubleValue;
            cnt[k]++;
        }
    }
    free(it);

    /* "\Processor Information(G,N)": group 0 only, as above */
    it = CtrArray(&g_ctr[C_FREQ], &n, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100);
    for (i = 0; i < n; i++) {
        const wchar_t *comma = wcschr(it[i].szName, L',');
        if (!ItemValid(&it[i]) || !comma || it[i].szName[0] != L'0' || !iswdigit(comma[1]))
            continue;
        k = ClusterOf((int)wcstol(comma + 1, NULL, 10));
        if (k >= 0) {
            if (it[i].FmtValue.doubleValue > fmax[k])
                fmax[k] = it[i].FmtValue.doubleValue;
            fcnt[k]++;
        }
    }
    free(it);

    for (k = 0; k < M.ncl; k++) {
        Cluster *c = &M.cl[k];
        c->usageOk = cnt[k] > 0;
        c->usage = cnt[k] ? sum[k] / cnt[k] : 0;
        c->mhzOk = fcnt[k] > 0 && fmax[k] > 0;
        c->mhz = fmax[k];
        SeriesPush(&c->hist, c->usage);
    }
    {
        double tot = 0;
        int cores = 0;
        for (k = 0; k < M.ncl; k++) {
            tot += sum[k];
            cores += cnt[k];
        }
        SeriesPush(&M.cpuAll, cores ? tot / cores : 0);
    }
}

static void SampleThermal(void)
{
    PDH_FMT_COUNTERVALUE_ITEM_W *it;
    DWORD n, i;

    M.ntz = 0;
    M.tzOk = g_ctr[C_TZ].ok;
    it = CtrArray(&g_ctr[C_TZ], &n, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100);
    for (i = 0; i < n && M.ntz < MAX_TZ; i++) {
        const wchar_t *nm = it[i].szName, *dot;
        double tk = it[i].FmtValue.doubleValue;
        if (!ItemValid(&it[i]) || tk <= 0)
            continue;
        dot = wcsrchr(nm, L'.');    /* "\_TZ.TZ00" -> "TZ00" */
        wcsncpy_s(M.tz[M.ntz].name, 32, dot ? dot + 1 : nm, _TRUNCATE);
        M.tz[M.ntz].celsius = tk / 10.0 - 273.15;
        M.ntz++;
    }
    free(it);
}

typedef struct {
    wchar_t key[96];                /* "luid_..._phys_0_eng_3" */
    wchar_t type[32];
    double sum;
} Engine;

static void SampleGpu(void)
{
    static Engine eng[MAX_ENG];
    PDH_FMT_COUNTERVALUE_ITEM_W *it;
    DWORD n, i;
    int ne = 0, e, t, nt = 0;
    wchar_t types[8][32];
    double tmax[8];

    M.gpuOk = g_ctr[C_GPU_ENG].ok;
    M.gpu = 0;
    M.gpuTypes[0] = 0;
    it = CtrArray(&g_ctr[C_GPU_ENG], &n, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100);
    for (i = 0; i < n; i++) {
        const wchar_t *luid = wcsstr(it[i].szName, L"luid_");
        const wchar_t *et = wcsstr(it[i].szName, L"_engtype_");
        size_t klen;
        if (!ItemValid(&it[i]) || !luid || !et || et < luid)
            continue;
        klen = (size_t)(et - luid);
        if (klen >= 96)
            klen = 95;
        for (e = 0; e < ne; e++)
            if (wcsncmp(eng[e].key, luid, klen) == 0 && eng[e].key[klen] == 0)
                break;
        if (e == ne) {
            if (ne == MAX_ENG)
                continue;
            wcsncpy_s(eng[e].key, 96, luid, klen);
            wcsncpy_s(eng[e].type, 32, et + 9, _TRUNCATE);
            eng[e].sum = 0;
            ne++;
        }
        eng[e].sum += it[i].FmtValue.doubleValue;
    }
    free(it);

    for (e = 0; e < ne; e++) {
        double v = eng[e].sum > 100 ? 100 : eng[e].sum;
        if (v > M.gpu)
            M.gpu = v;
        for (t = 0; t < nt; t++)
            if (!wcscmp(types[t], eng[e].type))
                break;
        if (t == nt) {
            if (nt == 8)
                continue;
            wcscpy_s(types[t], 32, eng[e].type);
            tmax[t] = 0;
            nt++;
        }
        if (v > tmax[t])
            tmax[t] = v;
    }
    for (t = 0; t < nt && t < 4; t++) {
        wchar_t part[48];
        swprintf(part, 48, L"%ls%ls %.0f%%", t ? L", " : L"", types[t], tmax[t]);
        wcscat_s(M.gpuTypes, 96, part);
    }
    SeriesPush(&M.gpuHist, M.gpu);

    /* adapter memory, all adapters together */
    M.memOk = FALSE;
    M.gpuDed = M.gpuShared = 0;
    {
        int which;
        for (which = 0; which < 2; which++) {
            it = CtrArray(&g_ctr[which ? C_GPU_SH : C_GPU_DED], &n, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100);
            for (i = 0; i < n; i++) {
                if (!ItemValid(&it[i]))
                    continue;
                if (which)
                    M.gpuShared += it[i].FmtValue.doubleValue;
                else
                    M.gpuDed += it[i].FmtValue.doubleValue;
                M.memOk = TRUE;
            }
            free(it);
        }
    }

    M.gpuSetOk = g_ctr[C_GPU_TEMP].ok || g_ctr[C_GPU_CLK].ok;
    M.gpuTempOk = CtrOne(&g_ctr[C_GPU_TEMP], &M.gpuTemp);
    M.gpuClkOk = CtrOne(&g_ctr[C_GPU_CLK], &M.gpuClk);
    M.gpuBusyOk = CtrOne(&g_ctr[C_GPU_BUSY], &M.gpuBusy);
    M.gpuThrOk = CtrOne(&g_ctr[C_GPU_THR], &M.gpuThr);
}

static void SampleNpu(void)
{
    PDH_FMT_COUNTERVALUE_ITEM_W *it;
    DWORD n, i;
    int k, which;

    M.npuSet = g_ctr[C_NPU_BUSY].ok;
    for (k = 0; k < M.nnpu; k++)
        M.npu[k].busyOk = M.npu[k].jobsOk = FALSE;
    for (which = 0; which < 2; which++) {
        it = CtrArray(&g_ctr[which ? C_NPU_JOBS : C_NPU_BUSY], &n, PDH_FMT_DOUBLE);
        for (i = 0; i < n; i++) {
            if (!ItemValid(&it[i]))
                continue;
            for (k = 0; k < M.nnpu; k++)
                if (!wcscmp(M.npu[k].name, it[i].szName))
                    break;
            if (k == M.nnpu) {
                if (M.nnpu == 2)
                    continue;
                wcsncpy_s(M.npu[k].name, 16, it[i].szName, _TRUNCATE);
                M.nnpu++;
            }
            if (which) {
                M.npu[k].jobs = it[i].FmtValue.doubleValue;
                M.npu[k].jobsOk = TRUE;
            } else {
                M.npu[k].busy = it[i].FmtValue.doubleValue;
                M.npu[k].busyOk = TRUE;
            }
        }
        free(it);
    }
    {
        double mx = 0;
        for (k = 0; k < M.nnpu; k++) {
            SeriesPush(&M.npu[k].hist, M.npu[k].busyOk ? M.npu[k].busy : 0);
            if (M.npu[k].busyOk && M.npu[k].busy > mx)
                mx = M.npu[k].busy;
        }
        SeriesPush(&M.npuAll, mx);
    }
}

static Row *AddRow(RowKind kind, const wchar_t *label, const Series *spark)
{
    Row *r;

    if (g_nrows == MAX_ROWS)
        return &g_rows[MAX_ROWS - 1];
    r = &g_rows[g_nrows++];
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    wcsncpy_s(r->label, 48, label, _TRUNCATE);
    r->spark = spark;
    return r;
}

static void FmtMB(wchar_t *out, size_t cap, double bytes)
{
    if (bytes >= 1024.0 * 1024 * 1024)
        swprintf(out, cap, L"%.2f GB", bytes / (1024.0 * 1024 * 1024));
    else
        swprintf(out, cap, L"%.0f MB", bytes / (1024.0 * 1024));
}

/* The widget: four lines, label, value and (for the use lines) a graph. */
static Row g_wrows[4];
static int g_nwrows;

static void BuildWidgetRows(void)
{
    Row *r;
    wchar_t part[32];
    int i;

    memset(g_wrows, 0, sizeof(g_wrows));
    g_nwrows = 4;

    r = &g_wrows[0];
    wcscpy_s(r->label, 48, L"CPU");
    r->spark = &M.cpuAll;
    for (i = 0; i < M.ncl; i++) {
        if (M.ncl == 2)
            swprintf(part, 32, L"%ls%ls %.0f%%", i ? L"   " : L"", i ? L"A72" : L"A53", M.cl[i].usage);
        else
            swprintf(part, 32, L"%.0f%%", M.cl[i].usage);
        wcscat_s(r->value, 112, part);
    }

    r = &g_wrows[1];
    wcscpy_s(r->label, 48, L"GPU");
    r->spark = M.gpuOk ? &M.gpuHist : NULL;
    if (M.gpuOk)
        swprintf(r->value, 112, L"%.0f%%", M.gpu);
    else
        wcscpy_s(r->value, 112, L"no counters");
    if (M.gpuTempOk) {
        swprintf(part, 32, L"   %.0f \x00B0" L"C", M.gpuTemp);
        wcscat_s(r->value, 112, part);
        r->warn = M.gpuTemp >= 85.0;
    }
    if (M.gpuClkOk) {
        swprintf(part, 32, L"   %.0f MHz", M.gpuClk);
        wcscat_s(r->value, 112, part);
    }
    if (M.gpuThrOk && M.gpuThr >= 0.5) {
        wcscat_s(r->value, 112, L" (throttled)");
        r->warn = TRUE;
    }

    r = &g_wrows[2];
    wcscpy_s(r->label, 48, L"NPU");
    if (!M.npuSet || M.nnpu == 0) {
        wcscpy_s(r->value, 112, M.npuSet ? L"driver not running" : L"no counters");
    } else {
        r->spark = &M.npuAll;
        for (i = 0; i < M.nnpu; i++) {
            swprintf(part, 32, L"%ls%.0f%%", i ? L" / " : L"", M.npu[i].busy);
            wcscat_s(r->value, 112, part);
        }
    }

    r = &g_wrows[3];
    wcscpy_s(r->label, 48, L"Temp");
    if (M.ntz == 0)
        wcscpy_s(r->value, 112, L"not reported");
    for (i = 0; i < M.ntz; i++) {
        swprintf(part, 32, L"%ls%.0f", i ? L" / " : L"", M.tz[i].celsius);
        wcscat_s(r->value, 112, part);
        if (M.tz[i].celsius >= 85.0)
            r->warn = TRUE;
    }
    if (M.ntz)
        wcscat_s(r->value, 112, L" \x00B0" L"C");
}

static void BuildRows(void)
{
    wchar_t a[48], b[48], tz[48];
    Row *r;
    int i;

    g_nrows = 0;
    g_hottestOk = FALSE;
    for (i = 0; i < M.ntz; i++)
        if (!g_hottestOk || M.tz[i].celsius > g_hottest) {
            g_hottest = M.tz[i].celsius;
            g_hottestOk = TRUE;
        }

    AddRow(R_HEAD, L"CPU", NULL);
    for (i = 0; i < M.ncl; i++) {
        Cluster *c = &M.cl[i];
        r = AddRow(R_ROW, c->label, &c->hist);
        if (!c->usageOk)
            wcscpy_s(r->value, 112, L"not reported");
        else if (c->mhzOk)
            swprintf(r->value, 112, L"%.0f%%   %.0f MHz", c->usage, c->mhz);
        else
            swprintf(r->value, 112, L"%.0f%%", c->usage);
    }
    if (!M.tzOk) {
        r = AddRow(R_ROW, L"Temperature", NULL);
        wcscpy_s(r->value, 112, L"no thermal zone counters");
    } else if (M.ntz == 0) {
        r = AddRow(R_ROW, L"Temperature", NULL);
        wcscpy_s(r->value, 112, L"no thermal zones reported");
    }
    for (i = 0; i < M.ntz; i++) {
        swprintf(a, 48, L"Zone %ls", M.tz[i].name);
        r = AddRow(R_ROW, a, NULL);
        swprintf(r->value, 112, L"%.1f \x00B0" L"C", M.tz[i].celsius);
        r->warn = M.tz[i].celsius >= 85.0;
    }

    AddRow(R_HEAD, L"GPU", NULL);
    r = AddRow(R_ROW, L"Utilization", M.gpuOk ? &M.gpuHist : NULL);
    if (!M.gpuOk)
        wcscpy_s(r->value, 112, L"no GPU engine counters");
    else if (M.gpuTypes[0])
        swprintf(r->value, 112, L"%.0f%%   %ls", M.gpu, M.gpuTypes);
    else
        swprintf(r->value, 112, L"%.0f%%", M.gpu);
    r = AddRow(R_ROW, L"Memory", NULL);
    if (M.memOk) {
        FmtMB(a, 48, M.gpuShared);
        FmtMB(b, 48, M.gpuDed);
        swprintf(r->value, 112, L"%ls shared, %ls dedicated", a, b);
    } else {
        wcscpy_s(r->value, 112, L"not reported");
    }
    if (M.gpuBusyOk) {
        r = AddRow(R_ROW, L"Busy (driver)", NULL);
        swprintf(r->value, 112, L"%.0f%%", M.gpuBusy);
    }
    r = AddRow(R_ROW, L"Temperature", NULL);
    if (M.gpuTempOk) {
        swprintf(r->value, 112, L"%.0f \x00B0" L"C", M.gpuTemp);
        r->warn = M.gpuTemp >= 85.0;
    } else {
        wcscpy_s(r->value, 112, M.gpuSetOk ? L"not reported (driver not running)" : L"not reported");
    }
    r = AddRow(R_ROW, L"Clock", NULL);
    if (M.gpuClkOk)
        swprintf(r->value, 112, L"%.0f MHz%ls", M.gpuClk,
                 M.gpuThrOk && M.gpuThr >= 0.5 ? L", throttled (thermal)" : L"");
    else
        wcscpy_s(r->value, 112, L"not reported");
    r->warn = M.gpuThrOk && M.gpuThr >= 0.5;
    if (!M.gpuSetOk) {
        r = AddRow(R_NOTE, L"", NULL);
        wcscpy_s(r->value, 112, L"RK3576 GPU counters not registered: run maliwddm-counters.cmd install");
    }

    AddRow(R_HEAD, L"NPU", NULL);
    if (!M.npuSet) {
        r = AddRow(R_ROW, L"Status", NULL);
        wcscpy_s(r->value, 112, L"driver has no counters");
        r = AddRow(R_NOTE, L"", NULL);
        wcscpy_s(r->value, 112, L"Not registered, or an older rknpu: run rknpu-perfcounters.ps1");
    } else if (M.nnpu == 0) {
        r = AddRow(R_ROW, L"Status", NULL);
        wcscpy_s(r->value, 112, L"driver not running");
    }
    for (i = 0; i < M.nnpu; i++) {
        NpuCore *c = &M.npu[i];
        swprintf(a, 48, L"NPU %ls", c->name);
        r = AddRow(R_ROW, a, &c->hist);
        if (!c->busyOk)
            wcscpy_s(r->value, 112, L"no data");
        else if (c->jobsOk)
            swprintf(r->value, 112, L"%.0f%% busy   %.0f jobs/s", c->busy, c->jobs);
        else
            swprintf(r->value, 112, L"%.0f%% busy", c->busy);
    }

    /* the tooltip: 127 characters at most */
    if (g_hottestOk)
        swprintf(tz, 48, L"   %.0f \x00B0" L"C", g_hottest);
    else
        tz[0] = 0;
    if (M.ncl == 2)
        swprintf(a, 48, L"CPU %.0f%% / %.0f%%", M.cl[0].usage, M.cl[1].usage);
    else
        swprintf(a, 48, L"CPU %.0f%%", M.cl[0].usage);
    if (M.npuSet && M.nnpu == 2)
        swprintf(b, 48, L"NPU %.0f%% / %.0f%%", M.npu[0].busy, M.npu[1].busy);
    else if (M.npuSet && M.nnpu == 1)
        swprintf(b, 48, L"NPU %.0f%%", M.npu[0].busy);
    else
        wcscpy_s(b, 48, L"NPU no counters");
    swprintf(g_tip, 128, L"%ls\n%ls%ls\nGPU %.0f%%\n%ls", APP_NAME, a, tz, M.gpu, b);
    BuildWidgetRows();
}

static int g_tick;
static BOOL g_warm = TRUE;          /* no sample yet: every value shows as "..." */

/* Before the first sample: the rows with their labels and an ellipsis, so
 * the tray icon and the widget are there at once. */
static void BuildWarmRows(void)
{
    static const wchar_t *const sec[3] = { L"CPU", L"GPU", L"NPU" };
    static const wchar_t *const wl[4] = { L"CPU", L"GPU", L"NPU", L"Temp" };
    Row *r;
    int i;

    g_nrows = 0;
    for (i = 0; i < 3; i++) {
        AddRow(R_HEAD, sec[i], NULL);
        r = AddRow(R_ROW, L"Starting", NULL);
        wcscpy_s(r->value, 112, L"\x2026");
    }
    memset(g_wrows, 0, sizeof(g_wrows));
    g_nwrows = 4;
    for (i = 0; i < 4; i++) {
        wcscpy_s(g_wrows[i].label, 48, wl[i]);
        wcscpy_s(g_wrows[i].value, 112, L"\x2026");
    }
    swprintf(g_tip, 128, L"%ls\nstarting\x2026", APP_NAME);
}

static void Sample(void)
{
    if (!g_query && PdhOpenQueryW(NULL, 0, &g_query) != ERROR_SUCCESS) {
        g_query = NULL;
        return;
    }
    if (g_tick % RETRY_TICKS == 0)
        CtrAddMissing();
    g_tick++;
    g_warm = FALSE;
    PdhCollectQueryData(g_query);
    SampleCpu();
    SampleThermal();
    SampleGpu();
    SampleNpu();
    BuildRows();
}

/* ------------------------------------------------------------ theme */

typedef struct {
    COLORREF bg, text, dim, line, accent, warn;
} Palette;

static BOOL g_appsDark, g_taskbarDark;
static Palette g_pal;

static BOOL RegLight(const wchar_t *value, BOOL dflt)
{
    DWORD v = 0, sz = sizeof(v);

    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     value, RRF_RT_REG_DWORD, NULL, &v, &sz) != ERROR_SUCCESS)
        return dflt;
    return v != 0;
}

static void ThemeRead(void)
{
    g_appsDark = !RegLight(L"AppsUseLightTheme", TRUE);
    g_taskbarDark = !RegLight(L"SystemUsesLightTheme", FALSE);
    if (g_appsDark) {
        g_pal.bg = RGB(0x2B, 0x2B, 0x2B);
        g_pal.text = RGB(0xFF, 0xFF, 0xFF);
        g_pal.dim = RGB(0xC5, 0xC5, 0xC5);
        g_pal.line = RGB(0x45, 0x45, 0x45);
        g_pal.accent = RGB(0x60, 0xCD, 0xFF);
        g_pal.warn = RGB(0xFF, 0x99, 0xA4);
    } else {
        g_pal.bg = RGB(0xF9, 0xF9, 0xF9);
        g_pal.text = RGB(0x1A, 0x1A, 0x1A);
        g_pal.dim = RGB(0x5D, 0x5D, 0x5D);
        g_pal.line = RGB(0xE0, 0xE0, 0xE0);
        g_pal.accent = RGB(0x00, 0x5F, 0xB8);
        g_pal.warn = RGB(0xC4, 0x2B, 0x1C);
    }
}

/* ------------------------------------------------------------ tray icon */

static HWND g_hwnd;
static HICON g_icon;
static UINT g_msgTaskbarCreated;
static BOOL g_trayAdded;

/* The hottest temperature as text, drawn white on black, the brightness
 * then used as the alpha of the taskbar's text colour. */
static HICON MakeIcon(void)
{
    int sz = GetSystemMetrics(SM_CXSMICON), h, x;
    BITMAPINFO bi;
    void *bits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP dib, mask, old;
    HFONT font = NULL, oldf;
    wchar_t txt[8];
    SIZE ext;
    RECT rc;
    ICONINFO ii;
    HICON icon;
    COLORREF col;
    DWORD *px;

    if (g_hottestOk)
        swprintf(txt, 8, L"%d", (int)(g_hottest + 0.5));
    else
        wcscpy_s(txt, 8, L"--");
    col = g_taskbarDark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x1A, 0x1A, 0x1A);
    if (g_hottestOk && g_hottest >= 85.0)
        col = g_taskbarDark ? RGB(0xFF, 0x99, 0xA4) : RGB(0xC4, 0x2B, 0x1C);

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = sz;
    bi.bmiHeader.biHeight = -sz;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dib || !bits) {
        DeleteDC(dc);
        return LoadIconW(NULL, IDI_APPLICATION);
    }
    old = (HBITMAP)SelectObject(dc, dib);
    memset(bits, 0, (size_t)sz * sz * 4);

    /* the largest bold font whose text fits the icon */
    for (h = sz + 2; h > 6; h--) {
        if (font)
            DeleteObject(font);
        font = CreateFontW(-h, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        oldf = (HFONT)SelectObject(dc, font);
        GetTextExtentPoint32W(dc, txt, (int)wcslen(txt), &ext);
        SelectObject(dc, oldf);
        if (ext.cx <= sz + 1)
            break;
    }
    oldf = (HFONT)SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    SetRect(&rc, 0, 0, sz, sz);
    DrawTextW(dc, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldf);
    DeleteObject(font);
    GdiFlush();

    px = (DWORD *)bits;
    for (x = 0; x < sz * sz; x++) {
        DWORD p = px[x];
        DWORD a = ((p & 0xFF) + ((p >> 8) & 0xFF) + ((p >> 16) & 0xFF)) / 3;
        DWORD r = GetRValue(col) * a / 255, g = GetGValue(col) * a / 255, b = GetBValue(col) * a / 255;
        px[x] = (a << 24) | (r << 16) | (g << 8) | b;      /* premultiplied BGRA */
    }
    SelectObject(dc, old);

    mask = CreateBitmap(sz, sz, 1, 1, NULL);
    memset(&ii, 0, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmColor = dib;
    ii.hbmMask = mask;
    icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(dib);
    DeleteDC(dc);
    return icon ? icon : LoadIconW(NULL, IDI_APPLICATION);
}

static void TrayUpdate(BOOL add)
{
    NOTIFYICONDATAW nid;
    HICON icon = MakeIcon();

    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = icon;
    wcsncpy_s(nid.szTip, 128, g_tip[0] ? g_tip : APP_NAME, _TRUNCATE);
    if (add || !g_trayAdded) {
        if (Shell_NotifyIconW(NIM_ADD, &nid)) {
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
            g_trayAdded = TRUE;
        }
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    if (g_icon)
        DestroyIcon(g_icon);
    g_icon = icon;
}

static void TrayRemove(void)
{
    NOTIFYICONDATAW nid;

    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = FALSE;
}

/* ------------------------------------------------------------ autostart */

static BOOL AutostartGet(void)
{
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                        NULL, NULL, NULL) == ERROR_SUCCESS;
}

/* The installer starts it for every user (its logon task, which it notes
 * as AutostartTask = 1, or the HKLM Run entry of an offline install before
 * its first boot); then the per-user entry is not offered. */
static BOOL AutostartMachine(void)
{
    DWORD v = 0, sz = sizeof(v);

    if (RegGetValueW(HKEY_LOCAL_MACHINE, SET_KEY, L"AutostartTask", RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS &&
        v != 0)
        return TRUE;
    return RegGetValueW(HKEY_LOCAL_MACHINE, RUN_KEY, RUN_VALUE, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                        NULL, NULL, NULL) == ERROR_SUCCESS;
}

/* A setting: the user's, else the machine default the installer wrote, else Dflt. */
static DWORD SettingGet(const wchar_t *name, DWORD dflt)
{
    DWORD v, sz = sizeof(v);

    if (RegGetValueW(HKEY_CURRENT_USER, SET_KEY, name, RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS)
        return v;
    sz = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, SET_KEY, name, RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS)
        return v;
    return dflt;
}

static BOOL SettingHas(const wchar_t *name)
{
    return RegGetValueW(HKEY_CURRENT_USER, SET_KEY, name, RRF_RT_REG_DWORD, NULL, NULL, NULL) == ERROR_SUCCESS;
}

static void SettingSet(const wchar_t *name, DWORD v)
{
    RegSetKeyValueW(HKEY_CURRENT_USER, SET_KEY, name, REG_DWORD, &v, sizeof(v));
}

static void AutostartSet(BOOL on)
{
    HKEY k;
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 16];

    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS)
        return;
    if (on) {
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        swprintf(cmd, MAX_PATH + 16, L"\"%ls\" --autostart", exe);
        RegSetValueExW(k, RUN_VALUE, 0, REG_SZ, (const BYTE *)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, RUN_VALUE);
    }
    RegCloseKey(k);
}

/* ------------------------------------------------------------ popup */

static UINT g_dpi = 96;
static HFONT g_fBody, g_fHead, g_fTitle;
static ULONGLONG g_hiddenAt;

static int S(int v)
{
    return MulDiv(v, (int)g_dpi, 96);
}

static void FontsMake(void)
{
    if (g_fBody) DeleteObject(g_fBody);
    if (g_fHead) DeleteObject(g_fHead);
    if (g_fTitle) DeleteObject(g_fTitle);
    g_fBody = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_fHead = CreateFontW(-S(14), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_fTitle = CreateFontW(-S(16), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

#define PAD         16
#define ROW_H       22
#define HEAD_H      30
#define TITLE_H     28
#define FOOT_H      30
#define LABEL_W     112
#define SPARK_W     64

/* Height of the content; paints it too when dc is not NULL. */
static int Layout(HDC dc, int width)
{
    int y = S(PAD), i;
    RECT rc;

    if (dc) {
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g_fTitle);
        SetTextColor(dc, g_pal.text);
        SetRect(&rc, S(PAD), y, width - S(PAD), y + S(TITLE_H));
        DrawTextW(dc, L"RK3576", -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    y += S(TITLE_H);

    for (i = 0; i < g_nrows; i++) {
        const Row *r = &g_rows[i];

        if (r->kind == R_HEAD) {
            y += S(6);
            if (dc) {
                HPEN pen = CreatePen(PS_SOLID, 1, g_pal.line), op = (HPEN)SelectObject(dc, pen);
                MoveToEx(dc, S(PAD), y, NULL);
                LineTo(dc, width - S(PAD), y);
                SelectObject(dc, op);
                DeleteObject(pen);
                SelectObject(dc, g_fHead);
                SetTextColor(dc, g_pal.text);
                SetRect(&rc, S(PAD), y, width - S(PAD), y + S(HEAD_H));
                DrawTextW(dc, r->label, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            y += S(HEAD_H);
            continue;
        }
        if (r->kind == R_NOTE) {
            if (dc) {
                SelectObject(dc, g_fBody);
                SetTextColor(dc, g_pal.dim);
                SetRect(&rc, S(PAD), y, width - S(PAD), y + S(ROW_H) * 2);
                DrawTextW(dc, r->value, -1, &rc, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            }
            y += S(ROW_H) * 2;
            continue;
        }
        if (dc) {
            int right = width - S(PAD);

            SelectObject(dc, g_fBody);
            SetTextColor(dc, g_pal.dim);
            SetRect(&rc, S(PAD), y, S(PAD) + S(LABEL_W), y + S(ROW_H));
            DrawTextW(dc, r->label, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            if (r->spark) {
                const Series *s = r->spark;
                int x0 = right - S(SPARK_W), y0 = y + S(3), y1 = y + S(ROW_H) - S(3), k;
                HPEN base = CreatePen(PS_SOLID, 1, g_pal.line), pen = CreatePen(PS_SOLID, S(1), g_pal.accent);
                HPEN op = (HPEN)SelectObject(dc, base);
                POINT pt[HIST];

                MoveToEx(dc, x0, y1, NULL);
                LineTo(dc, right, y1);
                for (k = 0; k < s->n; k++) {
                    int idx = (s->head - s->n + k + HIST) % HIST;
                    double v = s->v[idx];
                    if (v < 0) v = 0;
                    if (v > 100) v = 100;
                    pt[k].x = x0 + MulDiv(HIST - s->n + k, S(SPARK_W), HIST - 1);
                    pt[k].y = y1 - (int)((y1 - y0) * v / 100.0 + 0.5);
                }
                SelectObject(dc, pen);
                if (s->n > 1)
                    Polyline(dc, pt, s->n);
                SelectObject(dc, op);
                DeleteObject(base);
                DeleteObject(pen);
                right = x0 - S(8);
            }
            SetTextColor(dc, r->warn ? g_pal.warn : g_pal.text);
            SetRect(&rc, S(PAD) + S(LABEL_W), y, right, y + S(ROW_H));
            DrawTextW(dc, r->value, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        y += S(ROW_H);
    }

    y += S(8);
    if (dc) {
        SelectObject(dc, g_fBody);
        SetTextColor(dc, g_pal.dim);
        SetRect(&rc, S(PAD), y, width - S(PAD), y + S(FOOT_H));
        DrawTextW(dc, L"Every second. Right-click the tray icon for options.", -1, &rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    y += S(FOOT_H) + S(PAD) / 2;
    return y;
}

static void PopupApplyFrame(void)
{
    BOOL dark = g_appsDark;
    DWORD corner = RK_DWMWCP_ROUND;

    DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(g_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
}

/* Above the tray icon, inside the work area of its monitor. */
static void PopupShow(void)
{
    NOTIFYICONIDENTIFIER id;
    RECT icon, work;
    MONITORINFO mi;
    HMONITOR mon;
    POINT cur;
    int w, h, x, y, gap;

    memset(&id, 0, sizeof(id));
    id.cbSize = sizeof(id);
    id.hWnd = g_hwnd;
    id.uID = TRAY_ID;
    if (FAILED(Shell_NotifyIconGetRect(&id, &icon))) {
        GetCursorPos(&cur);
        SetRect(&icon, cur.x, cur.y, cur.x + 1, cur.y + 1);
    }
    mon = MonitorFromRect(&icon, MONITOR_DEFAULTTONEAREST);
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    work = mi.rcWork;

    /* on that monitor first, so the DPI is the monitor's */
    SetWindowPos(g_hwnd, HWND_TOPMOST, work.right - 10, work.bottom - 10, 1, 1, SWP_NOACTIVATE);
    g_dpi = GetDpiForWindow(g_hwnd);
    FontsMake();

    w = S(POPUP_W);
    h = Layout(NULL, w);
    gap = S(12);
    x = icon.right - w;
    if (x + w > work.right - gap) x = work.right - gap - w;
    if (x < work.left + gap) x = work.left + gap;
    if (icon.top >= work.bottom - 1 || icon.top > (work.top + work.bottom) / 2)
        y = work.bottom - gap - h;  /* taskbar at the bottom (or the icon is low) */
    else
        y = work.top + gap;
    if (y < work.top + gap) y = work.top + gap;

    PopupApplyFrame();
    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    SetForegroundWindow(g_hwnd);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void PopupHide(void)
{
    if (IsWindowVisible(g_hwnd)) {
        ShowWindow(g_hwnd, SW_HIDE);
        g_hiddenAt = GetTickCount64();
    }
}

/* ------------------------------------------------------------ widget */

static HWND g_widget, g_widgetHost;
static BOOL g_widgetOn, g_clickThrough, g_widgetDrag;
static BOOL g_childFailed;          /* the desktop refused a child: stay top-level */
static ULONGLONG g_startTick;       /* GetTickCount64 at start */
static BOOL g_autostart;            /* started at logon (--autostart) */
static POINT g_dragOff;
static UINT g_wdpi;
static HFONT g_wfBody, g_wfHead;
static ULONGLONG g_wsig;            /* what was last drawn */
static int g_wlastw, g_wlasth;

static void MenuShow(int x, int y);

static int WS(int v)
{
    return MulDiv(v, (int)(g_wdpi ? g_wdpi : 96), 96);
}

/* The window that holds the desktop icons (SHELLDLL_DefView): Progman, or
 * the WorkerW Explorer moves them into for some wallpaper modes. A child of
 * it sits on the desktop: under every application, and still there on
 * Win+D, which hides top-level windows but not the desktop's children. */
static HWND DesktopHost(void)
{
    HWND progman = FindWindowW(L"Progman", NULL), w = NULL;

    if (progman && FindWindowExW(progman, NULL, L"SHELLDLL_DefView", NULL))
        return progman;
    while ((w = FindWindowExW(NULL, w, L"WorkerW", NULL)) != NULL)
        if (FindWindowExW(w, NULL, L"SHELLDLL_DefView", NULL))
            return w;
    return progman;
}

static UINT DpiAt(POINT pt)
{
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    UINT dx = 96, dy = 96;

    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)))
        return 96;
    return dx;
}

static void WidgetFonts(void)
{
    if (g_wfBody) DeleteObject(g_wfBody);
    if (g_wfHead) DeleteObject(g_wfHead);
    /* grey-scale antialiasing: ClearType's coloured fringes do not survive
     * being composited at a partial alpha */
    g_wfBody = CreateFontW(-WS(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_wfHead = CreateFontW(-WS(13), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

#define WPAD        10
#define WROW_H      20
#define WLABEL_W    42
#define WSPARK_W    48

/* The default: the top right of the primary monitor's work area, 16 DIP in. */
static POINT WidgetDefaultPos(void)
{
    POINT z = { 0, 0 };
    MONITORINFO mi;
    UINT dpi;

    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(z, MONITOR_DEFAULTTOPRIMARY), &mi);
    dpi = DpiAt(z);
    z.x = mi.rcWork.right - MulDiv(16 + WIDGET_W, (int)dpi, 96);
    z.y = mi.rcWork.top + MulDiv(16, (int)dpi, 96);
    if (z.x < mi.rcWork.left)
        z.x = mi.rcWork.left;
    return z;
}

/* The saved place, if a monitor still shows it; else the default. */
static POINT WidgetSavedPos(void)
{
    POINT p;
    RECT vs;

    if (!SettingHas(L"WidgetX") || !SettingHas(L"WidgetY"))
        return WidgetDefaultPos();
    p.x = (LONG)SettingGet(L"WidgetX", 0);
    p.y = (LONG)SettingGet(L"WidgetY", 0);
    vs.left = p.x;
    vs.top = p.y;
    vs.right = p.x + 40;
    vs.bottom = p.y + 20;
    if (!MonitorFromRect(&vs, MONITOR_DEFAULTTONULL))
        return WidgetDefaultPos();
    return p;
}

static void WidgetScreenPos(POINT *p)
{
    RECT rc;

    GetWindowRect(g_widget, &rc);
    p->x = rc.left;
    p->y = rc.top;
}

/* Screen coordinates to the host's client coordinates (a child is placed in those). */
static void WidgetMoveTo(POINT scr, int w, int h, UINT flags)
{
    POINT c = scr;

    if (g_widgetHost)
        ScreenToClient(g_widgetHost, &c);
    SetWindowPos(g_widget, g_widgetHost ? HWND_TOP : HWND_BOTTOM, c.x, c.y, w, h, flags | SWP_NOACTIVATE);
}

static ULONGLONG Fnv(ULONGLONG h, const void *data, size_t n)
{
    const unsigned char *b = (const unsigned char *)data;

    while (n--) {
        h ^= *b++;
        h *= 1099511628211ull;
    }
    return h;
}

/* What the widget would show, as a number: when it is the same, nothing is drawn. */
static ULONGLONG WidgetSignature(int sparkH)
{
    ULONGLONG h = 1469598103934665603ull;
    int i, k;

    h = Fnv(h, &g_appsDark, sizeof(g_appsDark));
    h = Fnv(h, &g_wdpi, sizeof(g_wdpi));
    for (i = 0; i < g_nwrows; i++) {
        const Row *r = &g_wrows[i];
        h = Fnv(h, r->label, wcslen(r->label) * sizeof(wchar_t));
        h = Fnv(h, r->value, wcslen(r->value) * sizeof(wchar_t));
        h = Fnv(h, &r->warn, sizeof(r->warn));
        if (r->spark) {
            const Series *s = r->spark;
            for (k = 0; k < s->n; k++) {
                /* the graph at its pixel height */
                int px = (int)(s->v[(s->head - s->n + k + HIST) % HIST] * sparkH / 100.0 + 0.5);
                h = Fnv(h, &px, sizeof(px));
            }
            h = Fnv(h, &s->n, sizeof(s->n));
        }
    }
    return h;
}

static void WidgetPaint(BOOL force)
{
    int w, h, i, x, y, k, radius;
    BITMAPINFO bi;
    void *bits = NULL;
    HDC scr, dc;
    HBITMAP dib, old;
    COLORREF bgc, txt, dim, acc, warn, line;
    BYTE bgA;
    DWORD *px, bgpx;
    POINT pos, src = { 0, 0 };
    SIZE size;
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    RECT rc;
    ULONGLONG sig;
    UINT dpi;

    if (!g_widget)
        return;
    WidgetScreenPos(&pos);
    dpi = DpiAt(pos);
    if (dpi != g_wdpi) {
        g_wdpi = dpi;
        WidgetFonts();
        force = TRUE;
    }
    w = WS(WIDGET_W);
    h = WS(WPAD) * 2 + WS(WROW_H) * g_nwrows;
    sig = WidgetSignature(WS(WROW_H) - WS(6));
    if (!force && sig == g_wsig && w == g_wlastw && h == g_wlasth)
        return;                     /* nothing changed: no drawing at all */
    g_wsig = sig;
    g_wlastw = w;
    g_wlasth = h;

    if (g_appsDark) {
        bgc = RGB(0x20, 0x20, 0x20); bgA = 0xB8;
        txt = RGB(0xFF, 0xFF, 0xFF); dim = RGB(0xC8, 0xC8, 0xC8);
        acc = RGB(0x60, 0xCD, 0xFF); warn = RGB(0xFF, 0x99, 0xA4); line = RGB(0x50, 0x50, 0x50);
    } else {
        bgc = RGB(0xF3, 0xF3, 0xF3); bgA = 0xC8;
        txt = RGB(0x1A, 0x1A, 0x1A); dim = RGB(0x50, 0x50, 0x50);
        acc = RGB(0x00, 0x5F, 0xB8); warn = RGB(0xC4, 0x2B, 0x1C); line = RGB(0xC8, 0xC8, 0xC8);
    }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    scr = GetDC(NULL);
    dc = CreateCompatibleDC(scr);
    dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dib || !bits) {
        DeleteDC(dc);
        ReleaseDC(NULL, scr);
        return;
    }
    old = (HBITMAP)SelectObject(dc, dib);
    {
        HBRUSH b = CreateSolidBrush(bgc);
        SetRect(&rc, 0, 0, w, h);
        FillRect(dc, &rc, b);
        DeleteObject(b);
    }
    SetBkMode(dc, TRANSPARENT);
    y = WS(WPAD);
    for (i = 0; i < g_nwrows; i++) {
        const Row *r = &g_wrows[i];
        int right = w - WS(WPAD);

        SelectObject(dc, g_wfHead);
        SetTextColor(dc, dim);
        SetRect(&rc, WS(WPAD), y, WS(WPAD) + WS(WLABEL_W), y + WS(WROW_H));
        DrawTextW(dc, r->label, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (r->spark) {
            const Series *s = r->spark;
            int x0 = right - WS(WSPARK_W), y0 = y + WS(3), y1 = y + WS(WROW_H) - WS(3);
            HPEN base = CreatePen(PS_SOLID, 1, line), pen = CreatePen(PS_SOLID, WS(1), acc);
            HPEN op = (HPEN)SelectObject(dc, base);
            POINT pt[HIST];

            MoveToEx(dc, x0, y1, NULL);
            LineTo(dc, right, y1);
            for (k = 0; k < s->n; k++) {
                double v = s->v[(s->head - s->n + k + HIST) % HIST];
                if (v < 0) v = 0;
                if (v > 100) v = 100;
                pt[k].x = x0 + MulDiv(HIST - s->n + k, WS(WSPARK_W), HIST - 1);
                pt[k].y = y1 - (int)((y1 - y0) * v / 100.0 + 0.5);
            }
            SelectObject(dc, pen);
            if (s->n > 1)
                Polyline(dc, pt, s->n);
            SelectObject(dc, op);
            DeleteObject(base);
            DeleteObject(pen);
            right = x0 - WS(6);
        }
        SelectObject(dc, g_wfBody);
        SetTextColor(dc, r->warn ? warn : txt);
        SetRect(&rc, WS(WPAD) + WS(WLABEL_W), y, right, y + WS(WROW_H));
        DrawTextW(dc, r->value, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        y += WS(WROW_H);
    }
    GdiFlush();

    /* Alpha: the background at bgA, everything drawn on it opaque, the
     * rounded corners cut out; premultiplied, as UpdateLayeredWindow wants. */
    px = (DWORD *)bits;
    bgpx = ((DWORD)GetRValue(bgc) << 16) | ((DWORD)GetGValue(bgc) << 8) | GetBValue(bgc);
    radius = WS(8);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            DWORD p = px[y * w + x] & 0x00FFFFFF, a = p == bgpx ? bgA : 255;
            int cx = x < radius ? radius - x : (x >= w - radius ? x - (w - radius - 1) : 0);
            int cy = y < radius ? radius - y : (y >= h - radius ? y - (h - radius - 1) : 0);
            if (cx && cy && cx * cx + cy * cy > radius * radius)
                a = 0;
            px[y * w + x] = (a << 24) |
                            ((((p >> 16) & 0xFF) * a / 255) << 16) |
                            ((((p >> 8) & 0xFF) * a / 255) << 8) |
                            ((p & 0xFF) * a / 255);
        }
    }
    size.cx = w;
    size.cy = h;
    UpdateLayeredWindow(g_widget, scr, NULL, &size, dc, &src, 0, &bf, ULW_ALPHA);
    SelectObject(dc, old);
    DeleteObject(dib);
    DeleteDC(dc);
    ReleaseDC(NULL, scr);
}

static void WidgetApplyClickThrough(void)
{
    LONG_PTR ex;

    if (!g_widget)
        return;
    ex = GetWindowLongPtrW(g_widget, GWL_EXSTYLE);
    ex = g_clickThrough ? (ex | WS_EX_TRANSPARENT) : (ex & ~(LONG_PTR)WS_EX_TRANSPARENT);
    SetWindowLongPtrW(g_widget, GWL_EXSTYLE, ex);
}

static void WidgetDestroy(void)
{
    if (g_widget) {
        HWND w = g_widget;
        g_widget = NULL;
        DestroyWindow(w);
    }
    g_widgetHost = NULL;
}

static void WidgetCreate(void)
{
    POINT pos = WidgetSavedPos();
    HWND host = DesktopHost();

    if (g_widget)
        return;
    /* at logon Explorer may not have made the desktop yet: wait for it
     * (WidgetTick tries every second) rather than show the fallback */
    if (!host && GetTickCount64() - g_startTick < 60000)
        return;
    if (host && !g_childFailed) {
        /* a child of the desktop's icon window (see DesktopHost); a layered
         * child needs the Windows 8+ entry in the manifest, which it has */
        POINT c = pos;
        ScreenToClient(host, &c);
        g_widget = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, WIDGET_CLASS, APP_NAME,
                                   WS_CHILD | WS_CLIPSIBLINGS, c.x, c.y, 1, 1, host, NULL,
                                   GetModuleHandleW(NULL), NULL);
        if (g_widget)
            g_widgetHost = host;
        else
            g_childFailed = TRUE;
    }
    if (!g_widget)      /* no desktop window: a bottom-most top-level window (Win+D hides it) */
        g_widget = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WIDGET_CLASS, APP_NAME,
                                   WS_POPUP, pos.x, pos.y, 1, 1, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_widget)
        return;
    WidgetApplyClickThrough();
    g_wdpi = 0;
    g_wsig = 0;
    WidgetMoveTo(pos, 1, 1, 0);
    WidgetPaint(TRUE);
    ShowWindow(g_widget, SW_SHOWNOACTIVATE);
}

/* Each second: draw if something changed; every few seconds, check the
 * host is still the desktop's (Explorer restarted, or moved the icons into
 * another window) and that nothing of the desktop's own covers the widget. */
static void WidgetTick(void)
{
    if (!g_widgetOn)
        return;
    if (!g_widget || (g_tick % 5 == 0 && !g_widgetDrag &&
                      (g_widgetHost ? (!IsWindow(g_widgetHost) || DesktopHost() != g_widgetHost)
                                    : (!g_childFailed && DesktopHost() != NULL)))) {
        WidgetDestroy();
        WidgetCreate();
        return;
    }
    if (g_widgetHost && !g_widgetDrag && GetWindow(g_widget, GW_HWNDPREV))
        SetWindowPos(g_widget, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    WidgetPaint(FALSE);
}

static void WidgetEnable(BOOL on)
{
    g_widgetOn = on;
    SettingSet(L"Widget", on ? 1 : 0);
    if (on)
        WidgetCreate();
    else
        WidgetDestroy();
}

static LRESULT CALLBACK WidgetProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    POINT p;

    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONDOWN:
        GetCursorPos(&p);
        WidgetScreenPos(&g_dragOff);
        g_dragOff.x = p.x - g_dragOff.x;
        g_dragOff.y = p.y - g_dragOff.y;
        g_widgetDrag = TRUE;
        SetCapture(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        if (g_widgetDrag) {
            GetCursorPos(&p);
            p.x -= g_dragOff.x;
            p.y -= g_dragOff.y;
            WidgetMoveTo(p, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (g_widgetDrag) {
            g_widgetDrag = FALSE;
            if (msg == WM_LBUTTONUP)
                ReleaseCapture();
            WidgetScreenPos(&p);
            SettingSet(L"WidgetX", (DWORD)p.x);
            SettingSet(L"WidgetY", (DWORD)p.y);
            WidgetPaint(FALSE);     /* another monitor's DPI */
        }
        return 0;
    case WM_RBUTTONUP:
        GetCursorPos(&p);
        MenuShow(p.x, p.y);
        return 0;
    case WM_DESTROY:
        /* also when Explorer goes away and takes its children with it;
         * WidgetTick makes a new one */
        if (hwnd == g_widget) {
            g_widget = NULL;
            g_widgetHost = NULL;
        }
        return 0;
    default:
        (void)lp;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static BOOL g_audioSwitch;          /* AutoSwitchHeadphones */

static void MenuShow(int x, int y)
{
    HMENU m = CreatePopupMenu();
    UINT cmd;

    AppendMenuW(m, MF_STRING, IDM_SHOW, L"Show");
    AppendMenuW(m, MF_STRING | (g_widgetOn ? MF_CHECKED : 0), IDM_WIDGET, L"Desktop widget");
    AppendMenuW(m, MF_STRING | (g_clickThrough ? MF_CHECKED : 0) | (g_widgetOn ? 0 : MF_GRAYED), IDM_CLICKTHRU,
                L"Widget click-through");
    /* greyed where setting the default device cannot work (audioswitch.cpp) */
    AppendMenuW(m, MF_STRING | (g_audioSwitch ? MF_CHECKED : 0) | (AudioSwitchAvailable() ? 0 : MF_GRAYED),
                IDM_AUDIOSWITCH, L"\u81ea\u52a8\u5207\u6362\u8033\u673a");
    if (AutostartMachine())
        AppendMenuW(m, MF_STRING | MF_CHECKED | MF_GRAYED, IDM_AUTOSTART, L"Start with Windows (all users, installed)");
    else
        AppendMenuW(m, MF_STRING | (AutostartGet() ? MF_CHECKED : 0), IDM_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
    SetMenuDefaultItem(m, IDM_SHOW, FALSE);
    SetForegroundWindow(g_hwnd);
    cmd = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN,
                                 x, y, g_hwnd, NULL);
    DestroyMenu(m);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    switch (cmd) {
    case IDM_SHOW:
        PopupShow();
        break;
    case IDM_AUTOSTART:
        AutostartSet(!AutostartGet());
        break;
    case IDM_WIDGET:
        WidgetEnable(!g_widgetOn);
        break;
    case IDM_CLICKTHRU:
        g_clickThrough = !g_clickThrough;
        SettingSet(L"WidgetClickThrough", g_clickThrough ? 1 : 0);
        WidgetApplyClickThrough();
        break;
    case IDM_AUDIOSWITCH:
        g_audioSwitch = !g_audioSwitch;
        SettingSet(L"AutoSwitchHeadphones", g_audioSwitch ? 1 : 0);
        AudioSwitchSetEnabled(g_audioSwitch);
        break;
    case IDM_EXIT:
        DestroyWindow(g_hwnd);
        break;
    default:
        break;
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_msgTaskbarCreated && msg != 0) {
        g_trayAdded = FALSE;        /* explorer restarted */
        TrayUpdate(TRUE);
        if (g_widgetOn) {           /* its desktop went with it */
            WidgetDestroy();
            WidgetCreate();
        }
        return 0;
    }
    switch (msg) {
    case WM_TIMER:
        if (wp == TIMER_SAMPLE) {
            ULONGLONG up = GetTickCount64() - g_startTick;

            /* started at logon: the first minute is the busiest of the
             * session, so the first PDH open waits a few seconds, and the
             * process runs in background mode (CPU, I/O and memory
             * priority) for the minute */
            if (g_autostart && up >= 60000) {
                SetPriorityClass(GetCurrentProcess(), PROCESS_MODE_BACKGROUND_END);
                g_autostart = FALSE;
            }
            if (g_warm && g_autostart && up < 5000) {
                TrayUpdate(FALSE);
                WidgetTick();
                return 0;
            }
            Sample();
            TrayUpdate(FALSE);
            WidgetTick();
            if (IsWindowVisible(hwnd)) {
                RECT rc;
                int h = Layout(NULL, S(POPUP_W));
                GetWindowRect(hwnd, &rc);
                if (rc.bottom - rc.top != h)    /* a zone or an NPU core came or went */
                    SetWindowPos(hwnd, NULL, rc.left, rc.bottom - h, S(POPUP_W), h,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT:            /* NOTIFYICON_VERSION_4: the click, once */
        case NIN_KEYSELECT:
            /* a click on the icon while the popup was open first hid it
             * (deactivation); do not open it again at once */
            if (IsWindowVisible(hwnd))
                PopupHide();
            else if (GetTickCount64() - g_hiddenAt > 300)
                PopupShow();
            break;
        case WM_CONTEXTMENU:
            MenuShow(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
            break;
        default:
            break;
        }
        return 0;
    case WM_SHOWPOPUP:
        PopupShow();
        return 0;
    case WM_AUDIOSWITCH:
        AudioSwitchPoll();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE)
            PopupHide();
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            PopupHide();
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && !wcscmp((const wchar_t *)lp, L"ImmersiveColorSet")) {
            ThemeRead();
            PopupApplyFrame();
            TrayUpdate(FALSE);
            WidgetPaint(TRUE);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_DPICHANGED:
        g_dpi = HIWORD(wp);
        FontsMake();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps), mem;
        RECT rc;
        HBITMAP bmp, old;
        HBRUSH bg;

        GetClientRect(hwnd, &rc);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        old = (HBITMAP)SelectObject(mem, bmp);
        bg = CreateSolidBrush(g_pal.bg);
        FillRect(mem, &rc, bg);
        DeleteObject(bg);
        Layout(mem, rc.right);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        AudioSwitchStop();
        KillTimer(hwnd, TIMER_SAMPLE);
        WidgetDestroy();
        TrayRemove();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

/* ------------------------------------------------------------ --once */

static void Out(const wchar_t *s)
{
    static HANDLE h = NULL;
    static BOOL console;
    DWORD mode, n;
    char buf[1024];
    int len;

    if (!h) {
        h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!h || h == INVALID_HANDLE_VALUE) {
            if (AttachConsole(ATTACH_PARENT_PROCESS))
                h = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        }
        console = h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode);
    }
    if (!h || h == INVALID_HANDLE_VALUE)
        return;
    if (console) {
        WriteConsoleW(h, s, (DWORD)wcslen(s), &n, NULL);
    } else {
        len = WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof(buf), NULL, NULL);
        if (len > 1)
            WriteFile(h, buf, (DWORD)(len - 1), &n, NULL);
    }
}

static int RunOnce(void)
{
    wchar_t line[256];
    int i;

    Sample();
    Sleep(SAMPLE_MS);
    Sample();
    for (i = 0; i < g_nrows; i++) {
        const Row *r = &g_rows[i];
        if (r->kind == R_HEAD)
            swprintf(line, 256, L"%ls%ls\r\n", i ? L"\r\n" : L"", r->label);
        else if (r->kind == R_NOTE)
            swprintf(line, 256, L"  (%ls)\r\n", r->value);
        else
            swprintf(line, 256, L"  %-22ls %ls\r\n", r->label, r->value);
        Out(line);
    }
    Out(L"\r\ncounters:\r\n");
    for (i = 0; i < C_COUNT; i++) {
        swprintf(line, 256, L"  %-8ls %ls\r\n", g_ctr[i].ok ? L"found" : L"absent", g_ctr[i].paths[0]);
        Out(line);
    }
    {
        /* reports only: --once never changes the default device */
        wchar_t sw[512];
        AudioSwitchDescribe(SettingGet(L"AutoSwitchHeadphones", 1) != 0, sw, 512);
        Out(L"\r\n");
        Out(sw);
        Out(L"\r\n");
    }
    return 0;
}

/* ------------------------------------------------------------ main */

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    HANDLE mutex;
    MSG m;

    (void)prev;
    (void)show;

    g_startTick = GetTickCount64();
    ClustersInit();
    ThemeRead();

    if (cmd && wcsstr(cmd, L"--once"))
        return RunOnce();

    mutex = CreateMutexW(NULL, TRUE, APP_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(APP_CLASS, NULL);
        /* started again at logon (HKLM and HKCU Run both set): stay quiet */
        if (other && !(cmd && wcsstr(cmd, L"--autostart")))
            PostMessageW(other, WM_SHOWPOPUP, 0, 0);
        return 0;
    }

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = APP_CLASS;
    wc.style = CS_DROPSHADOW;
    if (!RegisterClassExW(&wc))
        return 1;
    wc.lpfnWndProc = WidgetProc;
    wc.lpszClassName = WIDGET_CLASS;
    wc.style = 0;
    if (!RegisterClassExW(&wc))
        return 1;
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, APP_CLASS, APP_NAME, WS_POPUP,
                             0, 0, 1, 1, NULL, NULL, inst, NULL);
    if (!g_hwnd)
        return 1;
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_dpi = GetDpiForWindow(g_hwnd);
    FontsMake();

    g_autostart = cmd && wcsstr(cmd, L"--autostart") != NULL;
    if (g_autostart) {
        SetPriorityClass(GetCurrentProcess(), PROCESS_MODE_BACKGROUND_BEGIN);
        BuildWarmRows();
    } else {
        Sample();
    }
    TrayUpdate(TRUE);
    g_clickThrough = SettingGet(L"WidgetClickThrough", 0) != 0;
    if (SettingGet(L"Widget", 0)) {
        g_widgetOn = TRUE;
        WidgetCreate();
    }
    SetTimer(g_hwnd, TIMER_SAMPLE, SAMPLE_MS, NULL);
    g_audioSwitch = SettingGet(L"AutoSwitchHeadphones", 1) != 0;
    AudioSwitchStart(g_hwnd, WM_AUDIOSWITCH, g_audioSwitch);

    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (g_query)
        PdhCloseQuery(g_query);
    if (mutex)
        CloseHandle(mutex);
    return 0;
}
