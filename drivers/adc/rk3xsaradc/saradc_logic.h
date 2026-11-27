/*
 * saradc_logic.h - the decisions rk3xsaradc makes from ADC samples
 *
 * Plain C with no headers, so the same file builds into the driver and into
 * the host unit test (test/saradc_logic_test.c, run by CI with gcc).  No
 * floating point: the driver runs at PASSIVE_LEVEL but has no reason to
 * save the FP state for a division.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef SARADC_LOGIC_H
#define SARADC_LOGIC_H

#define SARADC_LOGIC_MAX_KEYS   8
#define SARADC_NO_KEY           (-1)

/* HID usages, page << 16 | usage */
#define SARADC_HID_PAGE(u)      ((unsigned int)(u) >> 16)
#define SARADC_HID_ID(u)        ((unsigned int)(u) & 0xFFFFu)
#define SARADC_HID_PAGE_DESKTOP 0x0001u
#define SARADC_HID_PAGE_CONSUMER 0x000Cu
#define SARADC_HID_PAGE_VENDOR  0xFF00u

/* Raw sample to microvolts, as Linux scales it: raw * vref / 2^bits. */
unsigned int SaradcRawToMicrovolts(unsigned int raw, unsigned int vrefMicrovolts, unsigned int bits);

/*
 * adc-keys, with the semantics of Linux drivers/input/keyboard/adc-keys.c:
 * the key whose press voltage is closest to the sample wins, unless the
 * key-up voltage is strictly closer, in which case no key is pressed.
 */
typedef struct _SARADC_KEYMAP {
    unsigned int Count;
    unsigned int KeyupMicrovolts;
    unsigned int PressMicrovolts[SARADC_LOGIC_MAX_KEYS];
} SARADC_KEYMAP;

/* Index of the pressed key, or SARADC_NO_KEY. */
int SaradcKeysClassify(const SARADC_KEYMAP *map, unsigned int microvolts);

/*
 * A value reported only once the same sample has been seen Need times in a
 * row (Need 0 or 1: every sample is taken as it comes).
 */
typedef struct _SARADC_DEBOUNCE {
    int Stable;
    int Candidate;
    unsigned int Count;
    unsigned int Need;
} SARADC_DEBOUNCE;

void SaradcDebounceInit(SARADC_DEBOUNCE *d, int initial, unsigned int need);
/* 1 when Stable changed with this sample, 0 otherwise. */
int SaradcDebounceStep(SARADC_DEBOUNCE *d, int sample);

/*
 * Keys are armed only once they have been seen released: Need key-up
 * samples in a row (at least 1) after start or resume.  Until then every
 * sample is taken as "no key", so a key held through boot, or a converter
 * that reads 0 V (which is where most of these buttons sit when pressed),
 * never reports a press.  That matters most for a key that is the power
 * button.  Once armed it stays armed.
 */
typedef struct _SARADC_KEYARM {
    int Armed;
    unsigned int Released;
    unsigned int Need;
} SARADC_KEYARM;

void SaradcKeyArmInit(SARADC_KEYARM *a, unsigned int need);
/* The classification to debounce: key, or SARADC_NO_KEY while not armed. */
int SaradcKeyArmStep(SARADC_KEYARM *a, int classified);

/*
 * Headphone jack on an ADC input: plugged below one voltage, unplugged
 * above another, and in between (the hysteresis band) whatever it was.  A
 * new level must hold for DebounceMs before it is reported; the first
 * decision is debounced the same way.  A sample inside the band before any
 * decision decides nothing.
 */
typedef struct _SARADC_JACK {
    unsigned int PluggedBelowMicrovolts;
    unsigned int UnpluggedAboveMicrovolts;
    unsigned int DebounceMs;
    int Known;
    int Connected;
    int Pending;                    /* -1 none, else the level waiting out the debounce */
    unsigned long long PendingSinceMs;
} SARADC_JACK;

void SaradcJackInit(SARADC_JACK *j, unsigned int pluggedBelowMicrovolts,
                    unsigned int unpluggedAboveMicrovolts, unsigned int debounceMs);
/* 1 when the reported state changed (including the first decision). */
int SaradcJackStep(SARADC_JACK *j, unsigned int microvolts, unsigned long long nowMs);

/*
 * The HID usage for a Linux input event code (the linux,code of an
 * adc-keys button), or 0 when there is no sensible one (KEY_VENDOR, ...).
 */
unsigned int SaradcLinuxCodeToHidUsage(unsigned int code);

/*
 * The three input reports of the fixed descriptor (hid.c), from the usage
 * of the key held now (0 for none):
 *   report 1, consumer: two 16-bit usage slots, the first one used
 *   report 2, system control: 2-bit index, 1..3 = usages 0x81..0x83
 *   report 3, vendor page 0xFF00: 8 buttons, usage 1..8 = bit 0..7
 * A usage the descriptor cannot carry yields all zeros.
 */
typedef struct _SARADC_HID_STATE {
    unsigned int Consumer;          /* usage id on page 0x0C, 0 = none */
    unsigned int System;            /* 0, or 1..3 */
    unsigned int Vendor;            /* bitmask */
} SARADC_HID_STATE;

void SaradcHidStateFromUsage(unsigned int usage, SARADC_HID_STATE *state);
/* 1 when the usage fits one of the three reports. */
int SaradcHidUsageSupported(unsigned int usage);

#endif /* SARADC_LOGIC_H */
