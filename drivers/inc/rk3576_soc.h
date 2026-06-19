/*++

Module Name:

    rk3576_soc.h

Abstract:

    Shared Rockchip RK3576 SoC constants for the Windows-on-ARM driver set.

    These values are *reference* data taken from:
      - Rockchip RK3576 TRM v1.2 (Part 1/2)
      - Mainline Linux device tree: arch/arm64/boot/dts/rockchip/rk3576.dtsi
      - The EDK2 RK3576 ACPI tables (Silicon/Rockchip/RK3576/AcpiTables)

    Drivers must obtain their actual MMIO ranges and interrupts from the ACPI
    _CRS resources passed to them at PrepareController time. The literals here
    exist for documentation, sanity-checking, and reuse by future drivers
    (I2C / SPI / SDHC / GMAC ...). Do not hard-code them into device logic.

Environment:

    Kernel mode.

--*/

#pragma once

//
// GIC: the SoC uses a GICv3 at 0x2a701000 (distributor). ACPI GSIV values are
// the GIC interrupt IDs as seen by the OS; for a peripheral wired to "GIC_SPI n"
// in the device tree, GSIV = n + 32.
//
#define RK3576_SPI_TO_GSIV(_Spi)        ((_Spi) + 32)

//
// Pin controller / IOMUX (GRF based). The GPIO function muxing lives here and
// in the SYS_GRF / PMU_GRF blocks; it is *not* part of the per-bank GPIO
// register window below.
//
#define RK3576_PINCTRL_BASE             0x26040000
#define RK3576_PINCTRL_SIZE             0x0000C000

//
// GPIO banks. RK3576 has 5 banks of 32 pins each (160 GPIOs total).
// Bank 0 lives in the PMU power domain (separate base); banks 1..4 are in the
// main bus power domain. All five use the same "v2" register layout.
//
#define RK3576_GPIO_BANK_COUNT          5
#define RK3576_GPIO_PINS_PER_BANK       32

#define RK3576_GPIO0_BASE               0x27320000  // PMU domain
#define RK3576_GPIO1_BASE               0x2AE10000
#define RK3576_GPIO2_BASE               0x2AE20000
#define RK3576_GPIO3_BASE               0x2AE30000
#define RK3576_GPIO4_BASE               0x2AE40000
#define RK3576_GPIO_BANK_SIZE           0x00000200

//
// Per-bank GIC SPI numbers (device-tree view) and the corresponding ACPI GSIVs.
//
#define RK3576_GPIO0_SPI                153
#define RK3576_GPIO1_SPI                157
#define RK3576_GPIO2_SPI                161
#define RK3576_GPIO3_SPI                165
#define RK3576_GPIO4_SPI                169

//
// ACPI hardware id that the GPIO banks are published under in the EDK2 DSDT
// (Silicon/Rockchip/RK3576/AcpiTables/Gpio.asl). The Windows GPIO miniport INF
// binds to ACPI\RKCP3002.
//
#define RK3576_GPIO_ACPI_HID            "RKCP3002"
