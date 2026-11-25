# wdthang.sys: hang the board on purpose

A test for edk2-rk3576's **Hardware watchdog** (the WDAT). Loading this
driver stops every CPU for good with interrupts masked
(`KeIpiGenericCall` with a callback that never returns), so Windows can
neither restart the watchdog nor crash cleanly. Nothing binds it and it has
no INF; it runs only when started by hand. Do not start it on a machine you
are using for anything else.

On the board, test signing on, as administrator:

    sc create wdthang type= kernel binPath= C:\woa-tools\wdthang.sys
    sc start wdthang

Expected:

- watchdog **on**: the screen freezes and the board resets by itself within
  about 3 minutes (the hardware period is about 179 s; see edk2-rk3576
  `Wdat.aslc`). The next boot's serial log says
  "AcpiPlatform: watchdog: ... last reset WAS the watchdog".
- watchdog **off**: the board stays frozen until the power is cut.

Then `sc delete wdthang` (the service is demand-start, so it does not load at
boot by itself).

Before this, leave the board running Windows for an hour with the watchdog on,
idle and under load: it must not reset. That is the only way to see that
Windows restarts the watchdog more often than every 179 s.
