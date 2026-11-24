/*
 * rk-monitor: switch the default playback device to the headphones when a
 * plug goes into the board's 3.5 mm jack, and back when it comes out.
 * See audioswitch.cpp.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef RKMON_AUDIOSWITCH_H
#define RKMON_AUDIOSWITCH_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start watching the audio endpoints. COM is initialised on the calling
 * (UI) thread. A change is posted to hwnd as msg; the window procedure then
 * calls AudioSwitchPoll. FALSE: nothing to watch (no COM, no enumerator). */
BOOL AudioSwitchStart(HWND hwnd, UINT msg, BOOL enabled);
void AudioSwitchStop(void);

/* Re-read the headphone endpoint and act on a plug or unplug. */
void AudioSwitchPoll(void);

void AudioSwitchSetEnabled(BOOL on);

/* Whether switching can work here: a Windows build the undocumented
 * IPolicyConfig is known on, and the interface can be created. */
BOOL AudioSwitchAvailable(void);

/* One line for --once, with the setting as given (no default device is
 * changed). */
void AudioSwitchDescribe(BOOL enabled, wchar_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
