# Licensing of this driver

aic8800usb is written for this repository and is licensed under
GPL-2.0-only, the licence in the [LICENSE](../../../LICENSE) file at the
repository root.

The protocol files (`aic_proto.h`, `aic_core.c`, `aic_fw.c`, `aic_wlan.c`,
`aic_data.c`) follow AICSemi's Linux driver for the AIC8800 family as
Radxa packages it in [radxa-pkg/aic8800](https://github.com/radxa-pkg/aic8800)
(commit `5562c254b0231be34588dc608ec28c4f1d613285`): the USB framing, the
LMAC message IDs and parameter layouts, the firmware download sequence and
its addresses, the patch table format and the values written into it, and
the TX and RX descriptors. No code was copied; the values and the order of
the steps were. Those sources are
`src/USB/driver_fw/drivers/aic8800/aic_load_fw` and `aic8800_fdrv`,
"Copyright (C) RivieraWaves 2011-2019" and "Copyright (C) AICSemi
2018-2020", built as kernel modules with `MODULE_LICENSE("GPL")`, which in
the Linux kernel means GPL version 2. The package's `debian/copyright`
states `Files: src/*  License: GPL-2`. (The repository's top-level
`LICENSE` file is GPL-3.0; per `debian/copyright` it covers Radxa's
packaging, not `src/`.) GPL-2.0 is the licence of this repository, so the
derived files are GPL-2.0-only like everything else here.

The supplicant and the cryptography (`aic_wpa.c`, `aic_crypto.c`) are
written from the standards (IEEE 802.11-2016 clause 12, FIPS 180-4,
FIPS 197, RFC 2104, RFC 3394, RFC 8018); no other implementation was used.

The WiFiCx front end (`wificx.cpp`) and the 802.11 frame conversions are
written from Microsoft's WiFiCx and WDI documentation and the WDK headers.
Its structure follows Microsoft's WiFiCx sample (MS-PL) and what rtw89-win
([Meari-Prototype/rtw89-win](https://github.com/Meari-Prototype/rtw89-win),
BSD-3-Clause) recorded of WiFiCx's behaviour on hardware; no code was
copied from either, and the findings it relies on are cited by their IDs in
the source and in [docs/dev/wificx.md](../../../docs/dev/wificx.md).

Nothing from Mario Bălănică's `aicwlan` Windows driver is used: it is
published as a binary without a licence. No code from Microsoft's driver
samples is used. The driver links against the WDK's `ntstrsafe.lib`,
`ksecdd.lib`, `libcntpr.lib` and `WificxTLVGenParse.lib` (Microsoft's WDI
TLV generator and parser, part of the WDK).

The firmware files the driver loads are not part of this repository; see
[docs/dev/aic8800.md](../../../docs/dev/aic8800.md#firmware-files).
