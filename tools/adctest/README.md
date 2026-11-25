# adctest

Console tool for the RK3576 SARADC through
[rk3xsaradc](../../drivers/adc/rk3xsaradc) (`\\.\RK3XSARADC`). Run it as
administrator.

    adctest info                  what the firmware wired to the ADC, counters
    adctest scan                  one conversion on each of the 8 channels
    adctest read CH [N [MS]]      N conversions of channel CH, MS ms apart, with min/max/mean
    adctest watch [MS]            all channels every MS ms (default 500) until Ctrl+C
    adctest jack [--watch]        the debounced headphone jack state, and its changes

Values are raw 12-bit codes and microvolts (raw x 1.8 V / 4096). The numbers a
board's DSDT needs (button press voltages, jack thresholds) are measured with
`read` and `watch`.

    msbuild tools\adctest\adctest.vcxproj /p:Configuration=Release /p:Platform=ARM64
