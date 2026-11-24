# Licensing of this driver

bcmwlsdio is written for this repository and is licensed under
GPL-2.0-only, the licence in the [LICENSE](../../../LICENSE) file at the
repository root.

The chip and protocol files (`bwl_regs.h`, `bwl_sdio.c`, `bwl_chip.c`,
`bwl_bringup.c`, `bwl_fw.c`, `bwl_proto.c`, `bwl_ctl.c`) follow OpenBSD's
`bwfm` driver (`sys/dev/ic/bwfm.c`, `bwfmreg.h`,
`sys/dev/sdmmc/if_bwfm_sdio.c`, `if_bwfm_sdio.h`): its register values,
structure layouts and the order of its bring-up steps. `bwfm` is
Copyright (c) 2010-2016 Broadcom Corporation and (c) 2016, 2017 Patrick
Wildt, under the ISC licence. The ISC licence permits use in a GPL-2.0
work provided the copyright and permission notice are kept. The full notice
is in `bwl_regs.h`, and each of the other files names it.

Linux's brcmfmac (`drivers/net/wireless/broadcom/brcm80211/brcmfmac`) is
also under the ISC licence (Broadcom). It was used to check `bwfm`'s values
and for the receive-abort sequence. No GPL-only Linux code is used.

Nothing from Microsoft's driver samples is used. The driver links against
the WDK's `sdbus.lib` and `ntstrsafe.lib`, as the other drivers here link
the WDK's libraries.

The firmware files the driver loads are not part of this repository; see
[docs/dev/wifi.md](../../../docs/dev/wifi.md#firmware-files).
