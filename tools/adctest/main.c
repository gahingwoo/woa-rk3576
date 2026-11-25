/*
 * adctest.exe - read the RK3576 SARADC through rk3xsaradc.sys
 *
 *   adctest info                  what the firmware wired to the ADC, counters
 *   adctest scan                  one conversion on each of the 8 channels
 *   adctest read CH [N [MS]]      N conversions of channel CH, MS apart
 *   adctest watch [MS]            all channels every MS (default 500) until Ctrl+C
 *   adctest jack [--watch]        the debounced jack state (and its changes)
 *
 * The numbers to put into a board's DSDT come from here: hold each button,
 * plug and unplug each kind of plug, and read the channel.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <windows.h>
#include <winioctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "..\..\drivers\adc\rk3xsaradc\rk3xsaradc_ioctl.h"

static HANDLE g_dev = INVALID_HANDLE_VALUE;
static volatile LONG g_stop;

static BOOL WINAPI OnCtrlC(DWORD type)
{
    (void)type;
    InterlockedExchange(&g_stop, 1);
    return TRUE;
}

static int Open(void)
{
    g_dev = CreateFileW(RK3X_SARADC_USER_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
    if (g_dev == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "cannot open \\\\.\\RK3XSARADC (error %lu): is rk3xsaradc.sys installed, "
                        "and does the firmware declare \\_SB.ADC0 (RKCP0ADC)?\n", GetLastError());
        return 0;
    }
    return 1;
}

static int Ioctl(DWORD code, void *in, DWORD inLen, void *out, DWORD outLen)
{
    DWORD got = 0;

    if (!DeviceIoControl(g_dev, code, in, inLen, out, outLen, &got, NULL)) {
        fprintf(stderr, "DeviceIoControl 0x%08lX failed (error %lu)\n", code, GetLastError());
        return 0;
    }
    if (got != outLen) {
        fprintf(stderr, "DeviceIoControl 0x%08lX returned %lu bytes, expected %lu\n", code, got, outLen);
        return 0;
    }
    return 1;
}

static int ReadChannel(unsigned int ch, RK3X_SARADC_READ_OUT *out)
{
    RK3X_SARADC_READ_IN in;

    in.Channel = ch;
    return Ioctl(IOCTL_RK3X_SARADC_READ_RAW, &in, sizeof(in), out, sizeof(*out));
}

static void PrintUsage(unsigned int usage)
{
    unsigned int page = usage >> 16, id = usage & 0xFFFF;

    if (page == 0x000C) {
        printf("consumer 0x%03X", id);
    } else if (page == 0x0001) {
        printf("system control 0x%02X", id);
    } else if (page == 0xFF00) {
        printf("vendor button %u", id);
    } else {
        printf("0x%08X", usage);
    }
}

static int CmdInfo(void)
{
    RK3X_SARADC_INFO info;
    unsigned int i;

    if (!Ioctl(IOCTL_RK3X_SARADC_GET_INFO, NULL, 0, &info, sizeof(info))) {
        return 1;
    }
    printf("interface %u, %u channels, %u bits, vref %u uV, poll %u ms%s%s\n",
           info.Version, info.Channels, info.Bits, info.VrefMicrovolts, info.PollIntervalMs,
           (info.Flags & RK3X_SARADC_FLAG_POLLING) ? ", polling" : "",
           (info.Flags & RK3X_SARADC_FLAG_STOPPED) ? ", STOPPED after timeouts" : "");
    if (info.KeysChannel == RK3X_SARADC_NO_CHANNEL) {
        printf("keys: none described\n");
    } else {
        printf("keys: channel %u, key-up %u uV, %u key(s)%s\n", info.KeysChannel,
               info.KeyupMicrovolts, info.KeyCount,
               (info.Flags & RK3X_SARADC_FLAG_KEYS) ? "" : " (HID not started)");
        for (i = 0; i < info.KeyCount && i < RK3X_SARADC_MAX_KEYS; i++) {
            printf("  key %u: press %u uV, linux code %u, ", i, info.KeyPressMicrovolts[i], info.KeyCode[i]);
            PrintUsage(info.KeyHidUsage[i]);
            printf("%s\n", info.KeyPressed == (int)i ? "  [held]" : "");
        }
    }
    if (info.JackChannel == RK3X_SARADC_NO_CHANNEL) {
        printf("jack: none described\n");
    } else {
        printf("jack: channel %u, plugged below %u uV, unplugged above %u uV, %u ms: %s\n",
               info.JackChannel, info.JackPluggedBelowMicrovolts, info.JackUnpluggedAboveMicrovolts,
               info.JackDebounceMs,
               info.JackConnected == 0xFFFFFFFFu ? "not decided yet" :
               info.JackConnected ? "plugged" : "unplugged");
    }
    printf("last raw:");
    for (i = 0; i < info.Channels && i < RK3X_SARADC_MAX_CHANNELS; i++) {
        if (info.LastRaw[i] == 0xFFFFFFFFu) {
            printf("  ch%u -", i);
        } else {
            printf("  ch%u %u", i, info.LastRaw[i]);
        }
    }
    printf("\nconversions %u, timeouts %u, jack notifications %u, HID reports %u\n",
           info.Conversions, info.Timeouts, info.Notifications, info.HidReports);
    printf("controller reset (\\_SB.ADC0.RSTC): %s, %u done\n",
           info.ResetAvailable ? "available" : "NOT available, channel changes go unreset", info.Resets);
    return 0;
}

static int CmdScan(void)
{
    RK3X_SARADC_READ_OUT out;
    unsigned int ch;

    for (ch = 0; ch < RK3X_SARADC_MAX_CHANNELS; ch++) {
        if (!ReadChannel(ch, &out)) {
            return 1;
        }
        printf("ch%u  raw %4u  %7u uV\n", ch, out.Raw, out.Microvolts);
    }
    return 0;
}

static int CmdRead(unsigned int ch, unsigned int n, unsigned int ms)
{
    RK3X_SARADC_READ_OUT out;
    unsigned int i, lo = 0xFFFFFFFFu, hi = 0;
    unsigned long long sum = 0;

    if (n == 0) {
        n = 1;
    }
    for (i = 0; i < n && !g_stop; i++) {
        if (!ReadChannel(ch, &out)) {
            return 1;
        }
        printf("ch%u  raw %4u  %7u uV\n", ch, out.Raw, out.Microvolts);
        if (out.Raw < lo) lo = out.Raw;
        if (out.Raw > hi) hi = out.Raw;
        sum += out.Raw;
        if (i + 1 < n && ms) {
            Sleep(ms);
        }
    }
    if (i > 1) {
        printf("%u samples: min %u, max %u, mean %llu\n", i, lo, hi, sum / i);
    }
    return 0;
}

static int CmdWatch(unsigned int ms)
{
    RK3X_SARADC_READ_OUT out;
    unsigned int ch;

    if (ms == 0) {
        ms = 500;
    }
    while (!g_stop) {
        for (ch = 0; ch < RK3X_SARADC_MAX_CHANNELS; ch++) {
            if (!ReadChannel(ch, &out)) {
                return 1;
            }
            printf("%5u", out.Raw);
        }
        printf("   (raw, ch0..ch7)\n");
        Sleep(ms);
    }
    return 0;
}

static int CmdJack(int watch)
{
    RK3X_SARADC_JACK_INFO j;
    unsigned int last = 0xFFFFFFFFu;

    do {
        if (!Ioctl(IOCTL_RK3X_SARADC_GET_JACK, NULL, 0, &j, sizeof(j))) {
            return 1;
        }
        if (j.Sequence != last) {
            if (!j.Known) {
                printf("jack: not decided yet (no jack channel, or still debouncing)\n");
            } else {
                printf("jack: %s at %u uV (change %u)\n", j.Connected ? "plugged" : "unplugged",
                       j.Microvolts, j.Sequence);
            }
            last = j.Sequence;
        }
        if (watch) {
            Sleep(200);
        }
    } while (watch && !g_stop);
    return 0;
}

static void Usage(void)
{
    printf("adctest info | scan | read CH [N [MS]] | watch [MS] | jack [--watch]\n");
}

int main(int argc, char **argv)
{
    int rc;

    if (argc < 2) {
        Usage();
        return 2;
    }
    SetConsoleCtrlHandler(OnCtrlC, TRUE);
    if (!Open()) {
        return 1;
    }
    if (!strcmp(argv[1], "info")) {
        rc = CmdInfo();
    } else if (!strcmp(argv[1], "scan")) {
        rc = CmdScan();
    } else if (!strcmp(argv[1], "read") && argc >= 3) {
        rc = CmdRead((unsigned int)strtoul(argv[2], NULL, 0),
                     argc >= 4 ? (unsigned int)strtoul(argv[3], NULL, 0) : 1,
                     argc >= 5 ? (unsigned int)strtoul(argv[4], NULL, 0) : 100);
    } else if (!strcmp(argv[1], "watch")) {
        rc = CmdWatch(argc >= 3 ? (unsigned int)strtoul(argv[2], NULL, 0) : 500);
    } else if (!strcmp(argv[1], "jack")) {
        rc = CmdJack(argc >= 3 && !strcmp(argv[2], "--watch"));
    } else {
        Usage();
        rc = 2;
    }
    CloseHandle(g_dev);
    return rc;
}
