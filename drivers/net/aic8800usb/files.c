/*
 * files.c - read a firmware file from disk into memory.
 *
 * The directory is FirmwareDir under Device Parameters, by default
 * \SystemRoot\System32\drivers\aic8800, where install-firmware.ps1 puts the
 * files.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "aic8800usb.h"

NTSTATUS
AicLoadFile(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG MaxBytes,
            _Out_ WDFMEMORY *Memory, _Out_ aic_file *File)
{
    DECLARE_UNICODE_STRING_SIZE(dir, 200);
    DECLARE_UNICODE_STRING_SIZE(path, 300);
    UNICODE_STRING valueName;
    WDFKEY key = NULL;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    FILE_STANDARD_INFORMATION info;
    WDF_OBJECT_ATTRIBUTES attr;
    HANDLE file = NULL;
    PVOID buf = NULL;
    NTSTATUS status;

    *Memory = NULL;
    File->Data = NULL;
    File->Len = 0;

    /* FirmwareDir: the service's Parameters key, then Device Parameters */
    RtlInitUnicodeString(&valueName, L"FirmwareDir");
    dir.Length = 0;
    if (NT_SUCCESS(AicOpenParams(KEY_READ, &key))) {
        if (!NT_SUCCESS(WdfRegistryQueryUnicodeString(key, &valueName, NULL, &dir))) {
            dir.Length = 0;
        }
        WdfRegistryClose(key);
    }
    if (dir.Length == 0 &&
        NT_SUCCESS(WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ,
                                            WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        if (!NT_SUCCESS(WdfRegistryQueryUnicodeString(key, &valueName, NULL, &dir))) {
            dir.Length = 0;
        }
        WdfRegistryClose(key);
    }
    if (dir.Length == 0) {
        (VOID)RtlUnicodeStringCopyString(&dir, AIC_DEFAULT_FW_DIR);
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
        AIC_LOG("%wZ: open failed 0x%08X", &path, status);
        return status;
    }
    status = ZwQueryInformationFile(file, &iosb, &info, sizeof(info), FileStandardInformation);
    if (NT_SUCCESS(status) && (info.EndOfFile.QuadPart <= 0 || info.EndOfFile.QuadPart > MaxBytes)) {
        status = STATUS_INVALID_IMAGE_FORMAT;
    }
    if (NT_SUCCESS(status)) {
        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject = Ctx->Device;
        status = WdfMemoryCreate(&attr, NonPagedPoolNx, AIC_POOL_TAG, (size_t)info.EndOfFile.QuadPart,
                                 Memory, &buf);
    }
    if (NT_SUCCESS(status)) {
        status = ZwReadFile(file, NULL, NULL, NULL, &iosb, buf, (ULONG)info.EndOfFile.QuadPart, NULL, NULL);
        if (NT_SUCCESS(status) && iosb.Information != (ULONG_PTR)info.EndOfFile.QuadPart) {
            status = STATUS_END_OF_FILE;
        }
        if (!NT_SUCCESS(status)) {
            WdfObjectDelete(*Memory);
            *Memory = NULL;
        } else {
            File->Data = (const u8 *)buf;
            File->Len = (u32)info.EndOfFile.QuadPart;
            AIC_LOG("%wZ: %lu bytes", &path, File->Len);
        }
    }
    ZwClose(file);
    return status;
}
