# Display

Windows draws on the framebuffer the firmware sets up. The firmware brings up
VOP2 and the HDMI PHY and hands over a UEFI GOP framebuffer, and the inbox
BasicDisplay driver scans it out. Nothing in this repo is involved, and there
is no display device in the ACPI tables.

That gives the resolution UEFI chose and nothing else: no mode changes, no
hotplug, no acceleration.

## What a real driver would take

A KMDOD (kernel-mode display-only driver) is the smallest step up: it owns a
framebuffer and programs VOP2, the DW-HDMI-QP controller and the HDPTX PHY
itself, reports modes from EDID, and presents in software. That means porting
what Linux does in `drivers/gpu/drm/rockchip/` (`rockchip_vop2.c`,
`dw_hdmi_qp`, the HDPTX PHY driver), plus adding an ACPI device for it to bind
to. A full WDDM driver with the Mali GPU is out of reach.

| Block | Base | Compatible |
|---|---|---|
| VOP2 | `0x27D00000` | `rockchip,rk3576-vop` |
| HDMI 2.1 controller (DW-HDMI-QP) | `0x27DA0000` | `rockchip,rk3576-dw-hdmi-qp` |
| HDPTX PHY | `0x2B000000` | `rockchip,rk3576-hdptx-phy` |
| HDPTX PHY GRF | `0x26032000` | `rockchip,rk3576-hdptxphy-grf` |
| MIPI DSI | `0x27D80000` | DSI host |
