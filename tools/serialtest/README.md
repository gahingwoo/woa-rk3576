# serialtest

English | [简体中文](README.zh-CN.md)

Loopback test for a serial port: wire the port's TX to its RX, then

```
serialtest list                          COM-port interfaces and COM names
serialtest UART4 --sweep                 9600 to 1500000 baud, 16 KiB each
serialtest COM4 --baud 115200 --bytes 65536 --chunk 256
```

`PORT` is a COM name, the port name a SerCx2 driver publishes
(`DEVPKEY_DeviceInterface_Serial_PortName`, e.g. `UART4` from
[rk3xuart](../../drivers/serial/rk3xuart)), or an interface path. The test
writes a pseudo-random pattern in chunks, reads each chunk back and
compares; 8N1, no flow control. Exit code 0 is a pass, 1 a mismatch or short
read, 2 a usage or open error. The `--sweep` rates are the ones a 24 MHz UART
clock reaches within 3 %.

Built by CI (`rk3576-woa-serialtest-arm64`); works with any serial port.
