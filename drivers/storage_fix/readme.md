# Storage drivers for ARM64 devices without coherent DMA

The NVMe and AHCI drivers that ship with Windows assume cache-coherent DMA.
On ARM64 boards without it (Raspberry Pi 5, NXP i.MX 8M, Orange Pi 5),
NVMe or AHCI access usually hangs the device. This package holds
patched copies by Doug Cook (https://github.com/idigdoug):

- `storahci.sys` fixes hangs on AHCI (SATA) storage.
- `stornvme.sys` fixes hangs on NVMe storage.

Report issues at
[Rockchip-Windows-Drivers](https://github.com/worproject/Rockchip-Windows-Drivers/issues).

## Installation

Put Windows in test mode (test-signed drivers), then overwrite the matching
driver in `C:\Windows\System32\Drivers`, for example with a system-file
update tool such as sfpcopy.

## Known issues

### storahci.sys: corrupt SMART data or crash in disk.sys

[Issue 59](https://github.com/worproject/Rockchip-Windows-Drivers/issues/59).
AHCI drives can still return corrupt SMART data; rare on the on-chip
controller, frequent on PCIe AHCI cards. On Windows builds 27765 and later
this gives invalid SMART results; on earlier builds it often bugchecks in
disk.sys. The cause is in storport.sys. Workaround: use build 27765 or later,
or no PCIe SATA controller.

### stornvme.sys: unresponsive system or DPC Watchdog Violation

[Issue 60](https://github.com/worproject/Rockchip-Windows-Drivers/issues/60).
Fast NVMe SSDs under heavy I/O can make the system stall or bugcheck with
DPC_WATCHDOG_VIOLATION. In legacy PCIe interrupt mode one CPU processes all
completions and can be kept busy too long. Things to try (report results in
issue 60):

- Import `StorNvmeCoalescing.reg` (in this .zip) and reboot.
- Edit `InterruptCoalescingEntry` and `InterruptCoalescingTime` under
  `HKEY_LOCAL_MACHINE\SYSTEM\ControlSet001\Services\stornvme\Parameters\Device`:
  `REG_MULTI_SZ` with a value like `* 3`, numbers 0 to 255. Higher is less
  likely to crash but may cost performance.
- Change the stornvme
  [interrupt affinity](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/interrupt-affinity-and-priority)
  so interrupts are serviced on the A76 cores.

## Patches

DMA coherence:

- storahci: clean the command table before IDENTIFY and normal commands;
  invalidate the data buffer before PRDT commands; DMA buffers from uncached
  memory.
- stornvme: invalidate the data buffer before commands; clean the PrpList;
  DMA buffers from uncached memory; `STOR_FEATURE_DMA_ALLOCATION_NO_BOUNDARY`
  removed from FeatureSupport so small DMA buffers are not taken from pool.

Crash dumps: in crash-dump mode both drivers carved memory used with
interlocked or unaligned operations (storahci: channel extension; stornvme:
submission and completion queues, command id context) out of the uncached
NonCachedExtension, which raised WHEA errors. A static buffer is now used
for that first allocation.

Missed doorbell (RK3588): the device can believe the driver acknowledged
item N-1 when it acknowledged N, which gives an endless interrupt loop.
After 10 consecutive do-nothing interrupts a DPC rings the completion
doorbell of every queue. A race that could unmask interrupts is also fixed.

## Changelog

- fix8: doorbell workaround for the RK3588 DPC watchdog timeout; interrupt
  masking fix.
- fix7, fix6: broken, do not use.
- fix5: crash dumps work.
- fix4: first generally useful release; no crash dumps.
