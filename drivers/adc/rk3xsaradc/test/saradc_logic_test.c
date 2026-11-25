/*
 * saradc_logic_test.c - host test of the rk3xsaradc decisions
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -o t test/saradc_logic_test.c saradc_logic.c && ./t
 *
 * The board numbers below are the ones the firmware gives (edk2-rk3576,
 * Saradc.asl and each board's DSDT); the voltages are computed from the
 * schematics, not measured.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include "../saradc_logic.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static void test_scale(void)
{
    CHECK(SaradcRawToMicrovolts(0, 1800000, 12) == 0);
    CHECK(SaradcRawToMicrovolts(4095, 1800000, 12) == 1799560);
    CHECK(SaradcRawToMicrovolts(2048, 1800000, 12) == 900000);
    /* CM5 boot-config divider, the schematic's own "ADC value 416" */
    CHECK(SaradcRawToMicrovolts(416, 1800000, 12) == 182812);
    CHECK(SaradcRawToMicrovolts(100, 1800000, 0) == 0);
}

static void test_keys_tspi(void)
{
    /* TaishanPi 3M: adc-keys on channel 1, keyup 1.8 V, Recovery 18 mV */
    SARADC_KEYMAP m = { 1, 1800000, { 18000 } };

    CHECK(SaradcKeysClassify(&m, 0) == 0);
    CHECK(SaradcKeysClassify(&m, 18000) == 0);
    CHECK(SaradcKeysClassify(&m, 900000) == 0);      /* 882000 vs 900000: key */
    CHECK(SaradcKeysClassify(&m, 909000) == 0);      /* tie: Linux keeps the key */
    CHECK(SaradcKeysClassify(&m, 909001) == SARADC_NO_KEY);
    CHECK(SaradcKeysClassify(&m, 1799560) == SARADC_NO_KEY);
}

static void test_keys_ladder(void)
{
    /* A three-key ladder, the way RK3588 boards wire volume keys */
    SARADC_KEYMAP m = { 3, 1800000, { 17000, 417000, 785000 } };

    CHECK(SaradcKeysClassify(&m, 10000) == 0);
    CHECK(SaradcKeysClassify(&m, 400000) == 1);
    CHECK(SaradcKeysClassify(&m, 800000) == 2);
    CHECK(SaradcKeysClassify(&m, 1500000) == SARADC_NO_KEY);
    m.Count = 0;
    CHECK(SaradcKeysClassify(&m, 10000) == SARADC_NO_KEY);
    CHECK(SaradcKeysClassify(NULL, 10000) == SARADC_NO_KEY);
}

static void test_keys_maskrom(void)
{
    /* CM5-IO: maskrom key on channel 0 against the module's boot strap
     * (100K up, 11.3K down: 183 mV idle, about 2 mV pressed through 100R) */
    SARADC_KEYMAP cm5 = { 1, 182000, { 0 } };
    /* ROCK 4D: 10K up, 6.8K down: 729 mV idle */
    SARADC_KEYMAP r4d = { 1, 728000, { 0 } };

    CHECK(SaradcKeysClassify(&cm5, 2000) == 0);
    CHECK(SaradcKeysClassify(&cm5, 182812) == SARADC_NO_KEY);
    CHECK(SaradcKeysClassify(&cm5, 120000) == SARADC_NO_KEY);
    CHECK(SaradcKeysClassify(&r4d, 5000) == 0);
    CHECK(SaradcKeysClassify(&r4d, 728600) == SARADC_NO_KEY);
}

static void test_debounce(void)
{
    SARADC_DEBOUNCE d;

    SaradcDebounceInit(&d, SARADC_NO_KEY, 2);
    CHECK(SaradcDebounceStep(&d, SARADC_NO_KEY) == 0);
    CHECK(SaradcDebounceStep(&d, 0) == 0);           /* one sample is not enough */
    CHECK(d.Stable == SARADC_NO_KEY);
    CHECK(SaradcDebounceStep(&d, 0) == 1);           /* the second one is */
    CHECK(d.Stable == 0);
    CHECK(SaradcDebounceStep(&d, 0) == 0);
    /* a glitch does not release the key */
    CHECK(SaradcDebounceStep(&d, SARADC_NO_KEY) == 0);
    CHECK(SaradcDebounceStep(&d, 0) == 0);
    CHECK(d.Stable == 0);
    /* a bouncing release: candidate changes restart the count */
    CHECK(SaradcDebounceStep(&d, 1) == 0);
    CHECK(SaradcDebounceStep(&d, SARADC_NO_KEY) == 0);
    CHECK(SaradcDebounceStep(&d, SARADC_NO_KEY) == 1);
    CHECK(d.Stable == SARADC_NO_KEY);

    SaradcDebounceInit(&d, SARADC_NO_KEY, 1);
    CHECK(SaradcDebounceStep(&d, 2) == 1);
    SaradcDebounceInit(&d, SARADC_NO_KEY, 0);
    CHECK(SaradcDebounceStep(&d, 2) == 1);
}

static void test_jack_rock4d(void)
{
    /*
     * ROCK 4D J18: the microphone contact biased from VCCA3V3_CODEC through
     * 100R + 2.2K, halved by 100K/100K into SARADC_VIN3_HP_HOOK:
     *   nothing plugged      ~1.63 V
     *   4-pole headset       mic bias across the capsule, ~0.5-1.2 V
     *   3-pole plug          contact on the sleeve, ~0 V
     * Thresholds from the firmware: plugged below 1.45 V, unplugged above
     * 1.52 V, 150 ms.
     */
    SARADC_JACK j;
    unsigned long long t = 1000;

    SaradcJackInit(&j, 1450000, 1520000, 150);
    CHECK(j.Known == 0);
    CHECK(SaradcJackStep(&j, 1630000, t) == 0);      /* first sample starts the wait */
    CHECK(SaradcJackStep(&j, 1630000, t + 100) == 0);
    CHECK(SaradcJackStep(&j, 1630000, t + 150) == 1);
    CHECK(j.Known == 1 && j.Connected == 0);
    t += 1000;
    /* headset goes in */
    CHECK(SaradcJackStep(&j, 900000, t) == 0);
    CHECK(SaradcJackStep(&j, 900000, t + 149) == 0);
    CHECK(SaradcJackStep(&j, 900000, t + 150) == 1);
    CHECK(j.Connected == 1);
    t += 1000;
    /* hook button: a short trip to 0 V is still plugged, no change */
    CHECK(SaradcJackStep(&j, 0, t) == 0);
    CHECK(j.Connected == 1);
    /* the band does not unplug it */
    CHECK(SaradcJackStep(&j, 1480000, t + 500) == 0);
    CHECK(j.Connected == 1);
    /* a bounce up and back during removal restarts the wait */
    CHECK(SaradcJackStep(&j, 1630000, t + 1000) == 0);
    CHECK(SaradcJackStep(&j, 900000, t + 1050) == 0);
    CHECK(j.Pending == -1);
    CHECK(SaradcJackStep(&j, 1630000, t + 1100) == 0);
    CHECK(SaradcJackStep(&j, 1630000, t + 1200) == 0);
    CHECK(SaradcJackStep(&j, 1630000, t + 1250) == 1);
    CHECK(j.Connected == 0);
}

static void test_jack_band_first(void)
{
    SARADC_JACK j;

    SaradcJackInit(&j, 1450000, 1520000, 0);
    CHECK(SaradcJackStep(&j, 1500000, 0) == 0);       /* band, nothing known */
    CHECK(j.Known == 0);
    CHECK(SaradcJackStep(&j, 100000, 10) == 1);       /* debounce 0: at once */
    CHECK(j.Connected == 1);

    /* inside-out thresholds are closed up, not inverted */
    SaradcJackInit(&j, 1500000, 1000000, 0);
    CHECK(j.UnpluggedAboveMicrovolts == 1500000);
    CHECK(SaradcJackStep(&j, 1500000, 0) == 0);
    CHECK(SaradcJackStep(&j, 1500001, 0) == 1);
    CHECK(j.Connected == 0);
}

static void test_hid(void)
{
    SARADC_HID_STATE s;

    CHECK(SaradcLinuxCodeToHidUsage(115) == 0x000C00E9u);   /* KEY_VOLUMEUP */
    CHECK(SaradcLinuxCodeToHidUsage(116) == 0x00010081u);   /* KEY_POWER */
    CHECK(SaradcLinuxCodeToHidUsage(0x168) == 0);           /* KEY_VENDOR */

    SaradcHidStateFromUsage(0x000C00E9u, &s);
    CHECK(s.Consumer == 0xE9 && s.System == 0 && s.Vendor == 0);
    SaradcHidStateFromUsage(0x00010081u, &s);
    CHECK(s.Consumer == 0 && s.System == 1 && s.Vendor == 0);
    SaradcHidStateFromUsage(0x00010083u, &s);
    CHECK(s.System == 3);
    SaradcHidStateFromUsage(0xFF000001u, &s);
    CHECK(s.Vendor == 0x01);
    SaradcHidStateFromUsage(0xFF000008u, &s);
    CHECK(s.Vendor == 0x80);
    SaradcHidStateFromUsage(0, &s);
    CHECK(s.Consumer == 0 && s.System == 0 && s.Vendor == 0);
    /* out of the descriptor's range: nothing */
    SaradcHidStateFromUsage(0xFF000009u, &s);
    CHECK(s.Vendor == 0);
    SaradcHidStateFromUsage(0x000C0400u, &s);
    CHECK(s.Consumer == 0);
    SaradcHidStateFromUsage(0x00010080u, &s);
    CHECK(s.System == 0);
    CHECK(!SaradcHidUsageSupported(0x00070004u));          /* keyboard page */
    CHECK(SaradcHidUsageSupported(0x000C0224u));
}

int main(void)
{
    test_scale();
    test_keys_tspi();
    test_keys_ladder();
    test_keys_maskrom();
    test_debounce();
    test_jack_rock4d();
    test_jack_band_first();
    test_hid();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("saradc_logic: all tests passed\n");
    return 0;
}
