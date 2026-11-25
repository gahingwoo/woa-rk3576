/*
 * serialtest - loopback test for a serial port (TX wired to RX)
 *
 *   serialtest list
 *   serialtest PORT [--baud N | --sweep] [--bytes N] [--chunk N]
 *
 * PORT is a COM name (COM4), the port name a SerCx2 driver publishes on its
 * GUID_DEVINTERFACE_COMPORT interface (UART4; "list" shows them), or a full
 * interface path (\\?\ACPI#RKCP3006#...).
 *
 * The test writes a pseudo-random pattern in chunks and reads each chunk
 * back, then compares. Exit code 0 when every byte came back in order at
 * every rate tried, 1 on a mismatch or a short read, 2 on a usage or open
 * error.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <ntddser.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "cfgmgr32.lib")

/* the rates a 24 MHz DesignWare UART clock reaches within 3 % */
static const DWORD g_sweep[] = { 9600, 19200, 38400, 57600, 115200, 500000, 1500000 };

static WCHAR *
port_name_of(const WCHAR *iface)
{
    DEVPROPTYPE type;
    ULONG size = 0;
    WCHAR *buf;

    if (CM_Get_Device_Interface_PropertyW(iface, &DEVPKEY_DeviceInterface_Serial_PortName,
                                          &type, NULL, &size, 0) != CR_BUFFER_SMALL) {
        return NULL;
    }
    buf = (WCHAR *)malloc(size);
    if (buf == NULL) {
        return NULL;
    }
    if (CM_Get_Device_Interface_PropertyW(iface, &DEVPKEY_DeviceInterface_Serial_PortName,
                                          &type, (PBYTE)buf, &size, 0) != CR_SUCCESS ||
        type != DEVPROP_TYPE_STRING) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* All present COM-port interfaces, as one multi-string; NULL when none. */
static WCHAR *
interfaces(void)
{
    ULONG len = 0;
    WCHAR *list;

    if (CM_Get_Device_Interface_List_SizeW(&len, (LPGUID)&GUID_DEVINTERFACE_COMPORT, NULL,
                                           CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
        len <= 1) {
        return NULL;
    }
    list = (WCHAR *)calloc(len, sizeof(WCHAR));
    if (list == NULL) {
        return NULL;
    }
    if (CM_Get_Device_Interface_ListW((LPGUID)&GUID_DEVINTERFACE_COMPORT, NULL, list, len,
                                      CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
        free(list);
        return NULL;
    }
    return list;
}

static int
cmd_list(void)
{
    WCHAR *list = interfaces();
    WCHAR *p;
    WCHAR name[256];
    DWORD n;
    int count = 0;

    printf("COM-port interfaces (GUID_DEVINTERFACE_COMPORT):\n");
    for (p = list; p != NULL && *p; p += wcslen(p) + 1) {
        WCHAR *pn = port_name_of(p);
        printf("  %-8ls %ls\n", pn ? pn : L"?", p);
        free(pn);
        count++;
    }
    if (count == 0) {
        printf("  (none)\n");
    }
    free(list);

    printf("COM names (QueryDosDevice):\n");
    count = 0;
    for (n = 1; n <= 64; n++) {
        WCHAR dos[16];
        swprintf(dos, 16, L"COM%lu", n);
        if (QueryDosDeviceW(dos, name, ARRAYSIZE(name)) != 0) {
            printf("  %-8ls -> %ls\n", dos, name);
            count++;
        }
    }
    if (count == 0) {
        printf("  (none)\n");
    }
    return 0;
}

static HANDLE
open_port(const char *arg)
{
    WCHAR want[256];
    WCHAR path[300];
    WCHAR *list;
    WCHAR *p;
    HANDLE h;

    MultiByteToWideChar(CP_ACP, 0, arg, -1, want, ARRAYSIZE(want));

    if (want[0] == L'\\') {
        wcscpy_s(path, ARRAYSIZE(path), want);
    } else {
        /* an interface whose port name matches wins over a COM name */
        path[0] = L'\0';
        list = interfaces();
        for (p = list; p != NULL && *p; p += wcslen(p) + 1) {
            WCHAR *pn = port_name_of(p);
            if (pn != NULL && _wcsicmp(pn, want) == 0) {
                wcscpy_s(path, ARRAYSIZE(path), p);
            }
            free(pn);
            if (path[0]) {
                break;
            }
        }
        free(list);
        if (path[0] == L'\0') {
            swprintf(path, ARRAYSIZE(path), L"\\\\.\\%ls", want);
        }
    }

    printf("opening %ls\n", path);
    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "open failed: error %lu\n", GetLastError());
    }
    return h;
}

static int
setup(HANDLE h, DWORD baud)
{
    DCB dcb;
    COMMTIMEOUTS to;

    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        fprintf(stderr, "GetCommState: error %lu\n", GetLastError());
        return 0;
    }
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb)) {
        fprintf(stderr, "SetCommState %lu baud: error %lu\n", baud, GetLastError());
        return 0;
    }

    /* each read returns when its bytes are in, or 2 s after the last one */
    memset(&to, 0, sizeof(to));
    to.ReadIntervalTimeout = 0;
    to.ReadTotalTimeoutMultiplier = 1;
    to.ReadTotalTimeoutConstant = 2000;
    to.WriteTotalTimeoutMultiplier = 1;
    to.WriteTotalTimeoutConstant = 2000;
    if (!SetCommTimeouts(h, &to)) {
        fprintf(stderr, "SetCommTimeouts: error %lu\n", GetLastError());
        return 0;
    }
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return 1;
}

static int
run(HANDLE h, DWORD baud, DWORD total, DWORD chunk)
{
    unsigned char *tx = (unsigned char *)malloc(chunk);
    unsigned char *rx = (unsigned char *)malloc(chunk);
    unsigned int seed = 0x40C0FFEEu ^ baud;
    DWORD done = 0;
    DWORD bad = 0;
    DWORD i;
    LARGE_INTEGER f, t0, t1;
    DWORD errors;
    COMSTAT cs;
    int ok = 1;

    if (tx == NULL || rx == NULL || !setup(h, baud)) {
        free(tx);
        free(rx);
        return 0;
    }

    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    while (done < total) {
        DWORD n = (total - done < chunk) ? total - done : chunk;
        DWORD w = 0;
        DWORD r = 0;
        DWORD got;

        for (i = 0; i < n; i++) {
            seed = seed * 1103515245u + 12345u;
            tx[i] = (unsigned char)(seed >> 16);
        }
        if (!WriteFile(h, tx, n, &w, NULL) || w != n) {
            fprintf(stderr, "  write: %lu of %lu bytes, error %lu\n", w, n, GetLastError());
            ok = 0;
            break;
        }
        while (r < n) {
            got = 0;
            if (!ReadFile(h, rx + r, n - r, &got, NULL)) {
                fprintf(stderr, "  read: error %lu\n", GetLastError());
                break;
            }
            if (got == 0) {
                break;
            }
            r += got;
        }
        if (r != n) {
            fprintf(stderr, "  short read at byte %lu: %lu of %lu (is TX wired to RX?)\n",
                    done, r, n);
            ok = 0;
            break;
        }
        for (i = 0; i < n; i++) {
            if (rx[i] != tx[i]) {
                if (bad < 8) {
                    fprintf(stderr, "  byte %lu: sent 0x%02x, got 0x%02x\n",
                            done + i, tx[i], rx[i]);
                }
                bad++;
            }
        }
        done += n;
    }
    QueryPerformanceCounter(&t1);

    errors = 0;
    memset(&cs, 0, sizeof(cs));
    ClearCommError(h, &errors, &cs);

    {
        double s = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
        printf("  %7lu baud: %lu bytes, %lu wrong, %.0f bytes/s (line limit %lu), comm errors 0x%lx\n",
               baud, done, bad, s > 0 ? done / s : 0.0, baud / 10, errors);
    }
    if (bad != 0 || errors != 0) {
        ok = 0;
    }
    free(tx);
    free(rx);
    return ok;
}

static void
usage(void)
{
    printf("usage: serialtest list\n"
           "       serialtest PORT [--baud N | --sweep] [--bytes N] [--chunk N]\n"
           "PORT: COM4, UART4 (an interface port name) or an interface path.\n"
           "Wire the port's TX to its RX first.\n");
}

int
main(int argc, char **argv)
{
    DWORD baud = 115200;
    DWORD bytes = 16384;
    DWORD chunk = 256;
    int sweep = 0;
    int ok = 1;
    int i;
    HANDLE h;

    if (argc < 2) {
        usage();
        return 2;
    }
    if (_stricmp(argv[1], "list") == 0) {
        return cmd_list();
    }
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--baud") == 0 && i + 1 < argc) {
            baud = strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--bytes") == 0 && i + 1 < argc) {
            bytes = strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--chunk") == 0 && i + 1 < argc) {
            chunk = strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--sweep") == 0) {
            sweep = 1;
        } else {
            usage();
            return 2;
        }
    }
    if (chunk == 0 || chunk > 65536 || bytes == 0) {
        usage();
        return 2;
    }

    h = open_port(argv[1]);
    if (h == INVALID_HANDLE_VALUE) {
        return 2;
    }
    if (sweep) {
        size_t k;
        for (k = 0; k < ARRAYSIZE(g_sweep); k++) {
            ok &= run(h, g_sweep[k], bytes, chunk);
        }
    } else {
        ok = run(h, baud, bytes, chunk);
    }
    CloseHandle(h);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
