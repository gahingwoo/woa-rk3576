# Display

Status on 2026-11-24.

Without a GPU driver, Windows draws on the framebuffer the firmware sets up.
The firmware brings up VOP2 and the HDMI PHY (or the TaishanPi 3M's DSI
screen) and hands over a UEFI GOP framebuffer, and the inbox BasicDisplay
driver scans it out. That gives the mode UEFI chose and nothing else: no mode
changes, no hot-plug, no acceleration.

[wddm-mali-g52](https://github.com/gahingwoo/wddm-mali-g52) is a WDDM driver
for the Mali-G52 that also owns the display: the desktop is composed on the
GPU and scanned out by VOP2, with rotation, DSI brightness, HDMI hot-plug at
the monitor's own timing, and HDMI audio. It binds the firmware's ACPI
devices `RKCP7402` (GPU) and `RKCP7403` (display). It has run on the
TaishanPi 3M. Nothing in this repository is involved.

The notes from before that driver existed are in
[history](../history/2026-10-01-display-notes.md).

## Display blocks

| Block | Base | Compatible |
|---|---|---|
| VOP2 | `0x27D00000` | `rockchip,rk3576-vop` |
| HDMI 2.1 controller (DW-HDMI-QP) | `0x27DA0000` | `rockchip,rk3576-dw-hdmi-qp` |
| HDPTX PHY | `0x2B000000` | `rockchip,rk3576-hdptx-phy` |
| HDPTX PHY GRF | `0x26032000` | `rockchip,rk3576-hdptxphy-grf` |
| MIPI DSI | `0x27D80000` | DSI host |
