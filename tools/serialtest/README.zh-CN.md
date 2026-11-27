# serialtest

[English](README.md) | 简体中文

串口回环测试：把端口的 TX 接到 RX，然后

```
serialtest list                          列出 COM 口接口和 COM 名
serialtest UART4 --sweep                 9600 到 1500000 波特，每档 16 KiB
serialtest COM4 --baud 115200 --bytes 65536 --chunk 256
```

`PORT` 可以是 COM 名、SerCx2 驱动发布的端口名（`DEVPKEY_DeviceInterface_Serial_PortName`，例如
[rk3xuart](../../drivers/serial/rk3xuart) 的 `UART4`），或接口路径。程序分块写入伪随机数据，每块读回比较；
8N1，无流控。退出码 0 表示通过，1 表示数据不符或读不全，2 表示用法或打开错误。`--sweep` 用的波特率是
24 MHz UART 时钟能在 3% 以内达到的那些。

由 CI 构建（`rk3576-woa-serialtest-arm64`）；任何串口都能用。
