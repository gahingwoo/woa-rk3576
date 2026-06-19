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
| [dwmac_regs.h](dwmac_regs.h) | kernel-verified | register map + descriptor bits + GRF |

## Design (v1)

- **Single TX + single RX DMA channel**, fixed descriptor rings.
- **Bounce buffers**: frame data is copied to/from fixed DMA buffers (no
  per-packet scatter-gather mapping yet). Simple and verifiable; SG DMA is a
  later optimization.
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
- **The NetAdapterCx layer (`netadapter.c`) is a faithful template, not
  validated.** NetCx APIs (the `NET_RING` packet/fragment iteration, link/
  capability structures, queue config) are owned by the WDK `<netadaptercx.h>`
  and change across versions; spots that touch them are marked
  `VERIFY-ON-BUILD`. Aligning these to the WDK ABI (cross-checking the
  NetAdapterCx samples) is **in progress** — the control flow is right,
  identifiers are being settled.
- **Interrupt-driven datapath**: a WDFINTERRUPT ISR latches the DMA channel
  status and the DPC wakes the TX/RX queues (`NetTxQueueNotifyMoreCompleted...` /
  `NetRxQueueNotifyMoreReceived...`). The dwmac status read/ack is verified; the
  WDFINTERRUPT + NetCx notification wiring is `VERIFY-ON-BUILD`.
- **No checksum/LSO/RSS offloads, no multicast filter** (accepts its unicast +
  broadcast). MTU 1500.
- **Not yet compiled with a WDK or run on silicon.**

## Bring-up dependency

GMAC0 only (single 1GbE on ROCK 4D / CM5-IO). The NanoPi M5 has a second GMAC1
(`ethernet@2a230000`) — same driver, different ACPI instance, once its DSDT
node exists.
