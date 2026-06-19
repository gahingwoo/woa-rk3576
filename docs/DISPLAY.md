# Display bring-up — RK3576 (VOP2 + HDMI/HDPTX)

Short version: **for v1, display works with no driver from us** via the Windows
inbox *BasicDisplay* driver over the UEFI GOP framebuffer. A real VOP2/HDMI
driver is a full WDDM effort and is deliberately *not* attempted here — this doc
explains why and lays the foundation for whoever takes it on.

## What the hardware is

| Block | Base | Compatible |
|-------|------|------------|
| VOP2 (Video Output Processor 2) | 0x27D00000 | `rockchip,rk3576-vop` |
| HDMI 2.1 controller (DW-HDMI-QP) | 0x27DA0000 | `rockchip,rk3576-dw-hdmi-qp` |
| HDPTX PHY (HDMI/eDP combo) | 0x2B000000 | `rockchip,rk3576-hdptx-phy` |
| HDPTX PHY GRF | 0x26032000 | `rockchip,rk3576-hdptxphy-grf` |
| MIPI DSI | 0x27D80000 | (DSI host) |

There is **no display device in the EDK2 ACPI tables** — unlike GPIO/I²C/SPI/
storage/USB/GMAC, the display path is not ACPI-enumerated. Windows only sees the
framebuffer the firmware set up.

## v1: inbox BasicDisplay over UEFI GOP (no driver)

The EDK2 firmware brings up VOP2 + HDPTX and provides a linear framebuffer
through the UEFI **Graphics Output Protocol (GOP)**. When the Windows boot loader
starts, it captures that framebuffer (address, resolution, pixel format) and the
inbox **BasicDisplay (BDD)** driver scans it out. This is the standard WOA
bring-up display path and needs:

- nothing in this repo, and
- no ACPI device.

Limitations: fixed resolution/timing (whatever UEFI set), no mode changes, no
acceleration, no hotplug, no brightness. That is expected and fine to reach the
desktop.

> Caveat from the firmware side: the EDK2 README notes HDMI output on the
> **CM5-IO is still WIP** (VOP2/HDPTX init runs, HPD detected, but the video
> timing is incorrect). On the **ROCK 4D** HDMI/GOP works at QHD. So on CM5-IO
> the GOP framebuffer itself must be made correct in firmware *before* even the
> inbox path shows a good picture — that is an EDK2 task, not a Windows one.

## A real driver: what it would take (future, large)

To get modesetting / multiple resolutions / hotplug / acceleration, you need a
**WDDM driver**. Two tiers:

1. **KMDOD (Kernel-Mode Display-Only Driver).** The smaller option — no 3D, no
   GPU scheduler. It owns a framebuffer and programs VOP2 + HDMI + HDPTX to scan
   it out, reports EDID/modes, and does software present (blt). Even this is a
   substantial driver: it must reimplement the VOP2 window/AFBC plumbing and the
   HDPTX PHY + DW-HDMI-QP link training that Linux does in
   `drivers/gpu/drm/rockchip/` (`rockchip_vop2.c`, `dw_hdmi_qp`, the Samsung
   HDPTX PHY driver). It also needs a way to be discovered — today there is no
   ACPI node, so one is being added to the EDK2 DSDT in the firmware port (WIP).

2. **Full WDDM** (GPU scheduler, DMA, 3D via the Mali GPU). Out of scope by a
   wide margin; not realistic to author offline.

### Prerequisites before any custom display driver is worth starting

1. **Firmware GOP must be correct** on the target board (the CM5-IO HDMI timing
   bug above). If the firmware can't produce a correct picture, neither can a
   KMDOD reusing the same VOP2/HDPTX programming.
2. **ACPI enumeration** for the display block(s) must be added to the EDK2 DSDT
   (a `_HID` + `_CRS` covering VOP2/HDMI/HDPTX), mirroring how the other
   peripherals are exposed.
3. A verified VOP2 + HDPTX + DW-HDMI-QP modeset sequence (port from the Linux
   DRM drivers listed above).

## Recommendation

Ship v1 on **inbox BasicDisplay + UEFI GOP**. Treat a VOP2 KMDOD as a separate,
later project gated on the two prerequisites above. The register bases here are
the starting point for that work.
