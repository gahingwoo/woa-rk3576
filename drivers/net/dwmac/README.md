# dwmac — RK3576 Gigabit Ethernet (Synopsys DWMAC-4.20a, Windows on ARM)

A NetAdapterCx driver for the RK3576 GMAC, which is a genuine **Synopsys
DesignWare MAC v4.20a** (`snps,dwmac-4.20a`) — published as `ACPI\RKCP6543`
(GMAC0 @ 0x2A220000, GIC SPI 325/330). Unlike the I²C/SPI "rk3x" blocks, this
one really is the Synopsys IP, so the register engine matches stmmac directly.

## Layering (confidence)

| File | Confidence | Purpose |
|------|-----------|---------|
| [hw.c](hw.c) | **kernel-verified** | DWMAC-4.20a engine: reset, MAC/DMA init, MAC address, MDIO, PHY link resolution, descriptor rings, TX/RX, RK SDGMAC_GRF clock select. Checked against the stmmac driver. |
| [netadapter.c](netadapter.c) | **template — verify on build** | NetAdapterCx + WDF integration and the TX/RX datapath. |
| [acpi_dsd.c](acpi_dsd.c) | **template — verify on hardware** | Reads the DMA tuning firmware publishes in `_DSD` and the `AXIC` package it names. |
| [dwmac_regs.h](dwmac_regs.h) | kernel-verified | register map + descriptor bits + GRF |

## Design (v1)

- **Single TX + single RX DMA channel**, fixed descriptor rings.
- **Bounce buffers**: frame data is copied to/from fixed DMA buffers (no
  per-packet scatter-gather mapping yet). Simple and verifiable; SG DMA is a
  later optimization.
- **DMA tuning comes from firmware, not from the driver.** The EDK2 port copies
  mainline's device-tree description of this MAC into `_DSD` verbatim — same
  property names, same values — and names an `AXIC` package for the AXI master
  settings. `acpi_dsd.c` reads both and programs `DMA_SYS_BUS_MODE` and the
  channel PBLs from them, falling back to exactly the values stmmac would use
  for a device tree that says nothing. Three things were previously wrong
  because the driver hardcoded instead:
  - `wr_osr_lmt` / `rd_osr_lmt` were never programmed at all, and the blind
    write of `DMA_SYS_BUS_MODE` cleared them to 0 — one outstanding AXI read and
    one outstanding write. Firmware asks for 8 and 4.
  - `PBL` was never programmed, so the burst length stayed at whatever reset
    left. stmmac uses 8 with the ×8 multiplier, i.e. 64 beats.
  - `EAME` was never set, though the driver uses `WdfDmaProfilePacket64` and
    writes the `_HI` descriptor registers. Without it the core ignores the upper
    address bits — and RK3576 DRAM runs from `0x40000000` to `0x140000000`, so a
    buffer really can land above 4 GB.

  One deliberate behaviour change: `AAL` is now off, because firmware does not
  publish `snps,aal` and mainline therefore runs this MAC with it clear. The
  driver used to set it unconditionally.
- **PHY-agnostic link resolution** via the standard clause-22 MII registers
  (BMSR / STAT1000 / LPA), so it works with the RTL8211F (ROCK 4D) or YT8531C
  (CM5-IO) without per-PHY code.
- **Link-speed clock** is switched through the RK3576 `SDGMAC_GRF` CON0 register
  (125 MHz / 25 MHz / 2.5 MHz for 1000/100/10), the same path the EDK2 `_DSM`
  uses. That GRF window is a fixed SoC address (not in `_CRS`) and is mapped
  directly. RGMII pin delays are left as the firmware programmed them
  (`rgmii-rxid`; TX delay set by UEFI).

## Status / limitations — read before building

- **The DWMAC engine (`hw.c`) is verified against the kernel stmmac driver.**
- **The NetAdapterCx layer (`netadapter.c`) compiles clean against NetAdapter
  2.1** (WDK 10.0.26100) but has not run on silicon. Getting it to build
  corrected four real defects, not just names: there is no
  `NetTxQueueGetAdapter`/`NetRxQueueGetAdapter` (the adapter and each queue now
  carry a context pointing back at the device context, which is where the
  adapter state actually lives); fragment data is reached through the
  `ms_fragment_virtualaddress` extension, not a `NetFragmentGetVirtualAddress`
  call; datapath capabilities were never published, so `NetAdapterStart` could
  not have succeeded; and that start call had to move out of `EvtDeviceAdd`.
- **Interrupt-driven datapath**: a WDFINTERRUPT ISR latches the DMA channel
  status and the DPC wakes the TX/RX queues (`NetTxQueueNotifyMoreCompleted...` /
  `NetRxQueueNotifyMoreReceived...`).
- **No checksum/LSO/RSS offloads, no multicast filter** (accepts its unicast +
  broadcast). MTU 1500.
- **`acpi_dsd.c` has two things only hardware can settle.** Neither can do worse
  than fall back to the old hardcoded values, and both log which way they went:
  1. Whether `acpi.sys` flattens a `Name` holding a package into the top-level
     argument list or hands it back as one nested-package argument. The parser
     accepts both rather than guessing.
  2. Whether evaluating `AXIC` — a `Name`, not a `Method` — works through
     `IOCTL_ACPI_EVAL_METHOD`. It should, since that is how every driver reads a
     `Name(_CRS, ...)`. If it turns out not to, the fix is one word in the
     firmware's ASL (`Name` → `Method`), not a driver change.
- **Builds for ARM64 in CI; not yet run on silicon.**

## Bring-up dependency

GMAC0 only (single 1GbE on ROCK 4D / CM5-IO). The NanoPi M5 has a second GMAC1
(`ethernet@2a230000`) — same driver, different ACPI instance, once its DSDT
node exists.
