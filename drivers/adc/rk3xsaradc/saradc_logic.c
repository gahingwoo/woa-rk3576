/*
 * saradc_logic.c - the decisions rk3xsaradc makes from ADC samples
 *
 * See saradc_logic.h.  No headers, no floating point, no library calls.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "saradc_logic.h"

unsigned int SaradcRawToMicrovolts(unsigned int raw, unsigned int vrefMicrovolts, unsigned int bits)
{
    if (bits == 0 || bits > 16) {
        return 0;
    }
    return (unsigned int)(((unsigned long long)raw * vrefMicrovolts) >> bits);
}

static unsigned int Distance(unsigned int a, unsigned int b)
{
    return a > b ? a - b : b - a;
}

int SaradcKeysClassify(const SARADC_KEYMAP *map, unsigned int microvolts)
{
    unsigned int closest = 0xFFFFFFFFu;
    int key = SARADC_NO_KEY;
    unsigned int i;

    if (map == 0 || map->Count == 0) {
        return SARADC_NO_KEY;
    }
    for (i = 0; i < map->Count && i < SARADC_LOGIC_MAX_KEYS; i++) {
        unsigned int d = Distance(map->PressMicrovolts[i], microvolts);
        if (d < closest) {
            closest = d;
            key = (int)i;
        }
    }
    /* adc-keys.c: if (abs(keyup - value) < closest) no key */
    if (Distance(map->KeyupMicrovolts, microvolts) < closest) {
        key = SARADC_NO_KEY;
    }
    return key;
}

void SaradcDebounceInit(SARADC_DEBOUNCE *d, int initial, unsigned int need)
{
    d->Stable = initial;
    d->Candidate = initial;
    d->Count = 0;
    d->Need = need;
}

int SaradcDebounceStep(SARADC_DEBOUNCE *d, int sample)
{
    if (sample == d->Stable) {
        d->Candidate = sample;
        d->Count = 0;
        return 0;
    }
    if (sample != d->Candidate) {
        d->Candidate = sample;
        d->Count = 0;
    }
    d->Count++;
    if (d->Count >= d->Need) {
        d->Stable = sample;
        d->Count = 0;
        return 1;
    }
    return 0;
}

void SaradcKeyArmInit(SARADC_KEYARM *a, unsigned int need)
{
    a->Armed = 0;
    a->Released = 0;
    a->Need = need == 0 ? 1 : need;
}

int SaradcKeyArmStep(SARADC_KEYARM *a, int classified)
{
    if (a->Armed) {
        return classified;
    }
    if (classified == SARADC_NO_KEY) {
        a->Released++;
        if (a->Released >= a->Need) {
            a->Armed = 1;
        }
    } else {
        a->Released = 0;
    }
    return SARADC_NO_KEY;
}

void SaradcJackInit(SARADC_JACK *j, unsigned int pluggedBelowMicrovolts,
                    unsigned int unpluggedAboveMicrovolts, unsigned int debounceMs)
{
    j->PluggedBelowMicrovolts = pluggedBelowMicrovolts;
    /* A band that is inside out would make every sample both; close it. */
    j->UnpluggedAboveMicrovolts = unpluggedAboveMicrovolts < pluggedBelowMicrovolts ?
                                  pluggedBelowMicrovolts : unpluggedAboveMicrovolts;
    j->DebounceMs = debounceMs;
    j->Known = 0;
    j->Connected = 0;
    j->Pending = -1;
    j->PendingSinceMs = 0;
}

int SaradcJackStep(SARADC_JACK *j, unsigned int microvolts, unsigned long long nowMs)
{
    int level;

    if (microvolts < j->PluggedBelowMicrovolts) {
        level = 1;
    } else if (microvolts > j->UnpluggedAboveMicrovolts) {
        level = 0;
    } else {
        /* In the band: nothing new.  A pending change is not confirmed by
         * this sample, nor cancelled by it. */
        return 0;
    }

    if (j->Known && level == j->Connected) {
        j->Pending = -1;
        return 0;
    }
    if (j->Pending != level) {
        j->Pending = level;
        j->PendingSinceMs = nowMs;
    }
    if (nowMs - j->PendingSinceMs >= j->DebounceMs) {
        j->Known = 1;
        j->Connected = level;
        j->Pending = -1;
        return 1;
    }
    return 0;
}

unsigned int SaradcLinuxCodeToHidUsage(unsigned int code)
{
    /* include/uapi/linux/input-event-codes.h -> HID Usage Tables 1.5 */
    static const unsigned int map[][2] = {
        { 113, 0x000C00E2u },   /* KEY_MUTE          Consumer Mute */
        { 114, 0x000C00EAu },   /* KEY_VOLUMEDOWN    Volume Decrement */
        { 115, 0x000C00E9u },   /* KEY_VOLUMEUP      Volume Increment */
        { 116, 0x00010081u },   /* KEY_POWER         System Power Down */
        { 139, 0x000C0040u },   /* KEY_MENU          Menu */
        { 142, 0x00010082u },   /* KEY_SLEEP         System Sleep */
        { 143, 0x00010083u },   /* KEY_WAKEUP        System Wake Up */
        { 158, 0x000C0224u },   /* KEY_BACK          AC Back */
        { 163, 0x000C00B5u },   /* KEY_NEXTSONG      Scan Next Track */
        { 164, 0x000C00CDu },   /* KEY_PLAYPAUSE     Play/Pause */
        { 165, 0x000C00B6u },   /* KEY_PREVIOUSSONG  Scan Previous Track */
        { 166, 0x000C00B7u },   /* KEY_STOPCD        Stop */
        { 172, 0x000C0223u },   /* KEY_HOMEPAGE      AC Home */
        { 217, 0x000C0221u },   /* KEY_SEARCH        AC Search */
    };
    unsigned int i;

    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (map[i][0] == code) {
            return map[i][1];
        }
    }
    return 0;
}

int SaradcHidUsageSupported(unsigned int usage)
{
    unsigned int page = SARADC_HID_PAGE(usage);
    unsigned int id = SARADC_HID_ID(usage);

    switch (page) {
    case SARADC_HID_PAGE_CONSUMER:
        return id >= 1 && id <= 0x3FF;
    case SARADC_HID_PAGE_DESKTOP:
        return id >= 0x81 && id <= 0x83;
    case SARADC_HID_PAGE_VENDOR:
        return id >= 1 && id <= 8;
    default:
        return 0;
    }
}

void SaradcHidStateFromUsage(unsigned int usage, SARADC_HID_STATE *state)
{
    unsigned int page = SARADC_HID_PAGE(usage);
    unsigned int id = SARADC_HID_ID(usage);

    state->Consumer = 0;
    state->System = 0;
    state->Vendor = 0;
    if (usage == 0 || !SaradcHidUsageSupported(usage)) {
        return;
    }
    if (page == SARADC_HID_PAGE_CONSUMER) {
        state->Consumer = id;
    } else if (page == SARADC_HID_PAGE_DESKTOP) {
        state->System = id - 0x80;
    } else {
        state->Vendor = 1u << (id - 1);
    }
}
