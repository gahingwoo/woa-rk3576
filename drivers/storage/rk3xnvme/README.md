# rk3xnvme: NVMe on Windows 10

Storport miniport for NVMe drives on the RK3576 boards (ROCK 4D, CM5-IO),
for Windows 10 build 19041 to 19045 only. Windows 11 keeps the inbox
`stornvme.sys`. Not run on hardware yet.

## Why

The RK3576 PCIe controller does not snoop the CPU caches, so the firmware
declares it `_CCA 0`. For such a device the Windows 10 HAL allocates DMA
common buffers with `PAGE_NOCACHE`, and Windows 10 maps `PAGE_NOCACHE` with
MAIR index 1, a Device memory type. On Device memory every unaligned access
faults, whatever `SCTLR_EL1.A` says. `stornvme.sys` 10.0.19041.1566 copies the
serial number out of its identify buffer with `ldp x8, x9, [x10]` at buffer
+ 4 (`NVMeHwAdapterControl+0x3c8`) and bugchecks 0x7E,
`STATUS_DATATYPE_MISALIGNMENT`. Windows 11 maps `PAGE_NOCACHE` with index 2,
Normal non-cacheable, where unaligned access is allowed.

rk3xnvme keeps its queues, PRP lists and identify buffer in the storport
uncached extension and reads and writes them only with aligned 32 and 64-bit
accesses. Data buffers come from storport's scatter/gather lists.

What it does: one admin and one I/O queue pair (up to 32 commands), the
first active namespace as LUN 0, 512 B to 4 KiB blocks, transfers up to
128 KiB (less if the drive's MDTS says so), flush, line or MSI interrupts plus
a 10 ms poll while commands are outstanding. Not done: TRIM, more queues,
NVMe pass-through, power states.

## Boot from NVMe

The service is boot start (`StartType 0`, group `SCSI Miniport`), so Windows
can live on the NVMe drive (ROCK 4D has no eMMC).

`stornvme.inf` matches the same ID, `PCI\CC_010802`, and an inbox driver
always outranks a test-signed one, whatever the ID. So the package does not
rely on ranking: on Windows 10 the value `stornvme.inf` under
`HKLM\DRIVERS\DriverDatabase\DeviceIds\PCI\CC_010802` is deleted, offline
by the package's `win10-nvme.cmd W:\` (which `install.cmd W:\` runs; a tool
that runs DISM itself, such as Maskrom Studio, has to run it after DISM) and
online by `install.cmd /online`'s SYSTEM task. That leaves rk3xnvme
the only driver Plug and Play finds for an NVMe controller. `stornvme`'s
service is left as it is. A Windows update that reinstalls `stornvme.inf`
can bring the value back: run `install.cmd /online <board>` again after one.

## Test plan (board)

1. CM5-IO, Windows 10 on eMMC with an NVMe drive: `install.cmd /online cm5io`,
   restart. Device Manager: "NVMe controller (RK3576, Windows 10)" started,
   the disk listed. `reg query "HKLM\DRIVERS\DriverDatabase\DeviceIds\PCI\CC_010802"`
   lists no `stornvme.inf`.
2. Read the MAIR (no driver needed): `reg query
   HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0 /v "CP 4510"`. Byte 1
   (bits 15:8) is index 1; Device memory is `0x00` or `0x04`.
3. Data: format the drive NTFS, copy a 4 GiB file there and back, compare
   hashes (`certutil -hashfile`). Repeat after a cold boot.
4. Load: `diskspd -c2G -b4K -r -o32 -t1 -d60 -w30` and `-b128K -si`; no
   errors in the System log (`Get-WinEvent -LogName System | ? ProviderName -match 'rk3xnvme|disk|storport'`).
5. ROCK 4D, boot from NVMe: apply Windows 10 to the NVMe from WinPE,
   `install.cmd W:\ rock4d`, check it printed the `stornvme.inf` removal,
   boot. Setup's first boot must reach OOBE without 0x7B
   (INACCESSIBLE_BOOT_DEVICE) or 0x7E. Then repeat steps 3 and 4, restart
   and shut down five times each.
