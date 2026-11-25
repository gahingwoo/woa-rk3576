/* Stub for tools/aictest/layoutcheck.c: just enough of the kernel headers
 * for the vendor's lmac_msg.h and ipc_shared.h to compile in user space.
 * SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + (c))
#define LINUX_VERSION_CODE KERNEL_VERSION(6, 1, 0)
