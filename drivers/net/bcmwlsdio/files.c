/*
 * files.c - read a firmware file from disk into a buffer of its exact size.
 *
 * The directory is FirmwareDir under Device Parameters, by default
 * \SystemRoot\System32\drivers\brcm, where install-firmware.ps1 puts the
 * files (bcm4345bt's patchram lives there too).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "bcmwlsdio.h"

NTSTATUS
BwlLoadFile(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG MaxBytes,
            _Out_ PUCHAR *Data, _Out_ PULONG Length)
{
    DECLARE_UNICODE_STRING_SIZE(dir, 200);
    DECLARE_UNICODE_STRING_SIZE(path, 300);
    UNICODE_STRING valueName;
    WDFKEY key = NULL;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    FILE_STANDARD_INFORMATION info;
    HANDLE file = NULL;
    PUCHAR buf = NULL;
    NTSTATUS status;

    *Data = NULL;
    *Length = 0;

    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ,
                                            WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        RtlInitUnicodeString(&valueName, L"FirmwareDir");
        if (!NT_SUCCESS(WdfRegistryQueryUnicodeString(key, &valueName, NULL, &dir))) {
            dir.Length = 0;
        }
        WdfRegistryClose(key);
    }
    if (dir.Length == 0) {
        (VOID)RtlUnicodeStringCopyString(&dir, BWL_DEFAULT_FW_DIR);
    }
    status = RtlUnicodeStringPrintf(&path, L"%wZ\\%ws", &dir, Name);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = ZwCreateFile(&file, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
                          NULL, 0);
    if (!NT_SUCCESS(status)) {
        BWL_LOG("%wZ: open failed 0x%08X", &path, status);
        return status;
    }
    status = ZwQueryInformationFile(file, &iosb, &info, sizeof(info), FileStandardInformation);
    if (NT_SUCCESS(status) && (info.EndOfFile.QuadPart <= 0 || info.EndOfFile.QuadPart > MaxBytes)) {
        status = STATUS_INVALID_IMAGE_FORMAT;
    }
    if (NT_SUCCESS(status)) {
        /* Exactly the file's size: a write past it shows in the guard (pool.c). */
        buf = (PUCHAR)BwlBufAlloc(POOL_FLAG_PAGED, (SIZE_T)info.EndOfFile.QuadPart, "file");
        if (buf == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    if (NT_SUCCESS(status)) {
        status = ZwReadFile(file, NULL, NULL, NULL, &iosb, buf, (ULONG)info.EndOfFile.QuadPart, NULL, NULL);
        if (NT_SUCCESS(status) && iosb.Information != (ULONG_PTR)info.EndOfFile.QuadPart) {
            status = STATUS_END_OF_FILE;
        }
        if (!NT_SUCCESS(status)) {
            BwlBufFree(Ctx, buf, "file read failed");
        } else {
            *Data = buf;
            *Length = (ULONG)info.EndOfFile.QuadPart;
            BWL_LOG("%wZ: %lu bytes", &path, *Length);
        }
    }
    ZwClose(file);
    return status;
}
