# 让我们的 Wi-Fi 驱动用上 Windows 自己的 Wi-Fi（WiFiCx 1.0）

[English](wificx.md) | 简体中文

状态（2026-11-25）：`aic8800usb`（ROCK 4D，USB 接口的 AIC8800D80）在分支 `aic8800-wificx` 上有了 WiFiCx 模式：用 Windows 的网络列表、它的 WPA2 supplicant 和它的配置文件，驱动只负责扫描、关联和装密钥。**这里的东西都还没在板子上跑过。** 内核那一侧在主机上对着模拟芯片测过，测试程序扮演 Windows 的 supplicant（`tools/aictest`）；WiFiCx 前端由 CI 用 WDK 编译，还没碰到过真正的 WiFiCx。以太网式模式（[aic8800.zh-CN.md](aic8800.zh-CN.md)）保留，装哪个 INF 就是哪种模式。`bcmwlsdio`（泰山派 3M、CM5-IO）还没有这一层，计划在文末。

## 目录

- [问题与结论](#问题与结论)
- [22621 上的 WiFiCx 1.0](#22621-上的-wificx-10)
- [WiFiCx 对驱动的要求](#wificx-对驱动的要求)
- [WPA2 由谁来做](#wpa2-由谁来做)
- [数据通路](#数据通路)
- [WiFiCx 模式下的 aic8800usb](#wificx-模式下的-aic8800usb)
- [INF、模式与切换](#inf模式与切换)
- [注册表值](#注册表值)
- [测试](#测试)
- [板上测试计划](#板上测试计划)
- [风险与未决问题](#风险与未决问题)
- [下一步：bcmwlsdio](#下一步bcmwlsdio)
- [参考来源](#参考来源)

## 问题与结论

到目前为止，我们的 Wi-Fi 驱动给 Windows 的是一个看起来像有线网卡的适配器，由脚本把 SSID 和密码写进注册表，交给驱动自带的 supplicant。Windows 的 Wi-Fi 界面（设置、任务栏网络列表、点一下连接、输密码的对话框、`netsh wlan`）需要一个 WLAN miniport，在 Windows 11 上就是 WiFiCx 客户端驱动。

在 23H2 上可行，但有限制：

- Windows 11 22H2/23H2（build 22621/22631）**只有 WiFiCx 1.0**，配 NetAdapterCx 2.2。为 1.0 编译的驱动能在上面跑；1.1/1.2 的功能（MLO、较新的能力字段、动作帧唤醒）用不上。
- **WPA2 的四次握手由 Windows 自己做。** EAPOL 帧作为普通数据帧走数据通路，Windows 再把密钥交下来（`WDI_SET_ADD_CIPHER_KEYS`）。所以 WiFiCx 模式下不用我们自己的 supplicant。AIC8800D80 的固件本来就是这样工作的（把 EAPOL 交给主机，用 `MM_KEY_ADD` 收密钥），固件这边什么都不用改。
- 数据通路两个方向上走的都是**原生 802.11 数据帧**，不是以太网帧；驱动负责转换。
- 网络列表由 Windows 自己维护，驱动只报告扫描期间听到的东西。

## 22621 上的 WiFiCx 1.0

| | Windows 11 22H2/23H2（22621/22631） | 24H2（26100） |
|---|---|---|
| KMDF | 1.33 | 1.33 |
| NetAdapterCx | WiFiCx 驱动用 2.2（微软原话："has to be forced to 2.2"） | 2.2+ |
| WiFiCx | **1.0** | 1.0、1.1、1.2 |
| `wificx.sys` | 我们的 22621.2428 镜像里是 10.0.22621.2215 | |

出处是微软的 "Building a WiFiCx driver for multiple versions of Windows"。1.1/1.2 的驱动要设 `WIFI_MINIMUM_VERSION_REQUIRED=0`，并且每个较新的调用前都要做运行时检查；直接按 1.0 编译两样都省了。`aic8800usb` 就是这么做的：

```xml
<KMDF_VERSION_MAJOR>1</KMDF_VERSION_MAJOR><KMDF_VERSION_MINOR>33</KMDF_VERSION_MINOR>
<NetAdapterDriver>true</NetAdapterDriver>
<NETADAPTER_VERSION_MAJOR>2</NETADAPTER_VERSION_MAJOR><NETADAPTER_VERSION_MINOR>2</NETADAPTER_VERSION_MINOR>
<WifiDriver>true</WifiDriver>
<WIFI_VERSION_MAJOR>1</WIFI_VERSION_MAJOR><WIFI_VERSION_MINOR>0</WIFI_VERSION_MINOR>
```

**CI 用的 WDK（10.0.26100.0）里有什么。** `km/wificx/1.0/`（`wificx.h`、`wificxtypes.h`、`wififuncenum.h`：39 个函数），另有 1.1 和 1.2；`km/wlan/2.0/` 里是微软的 WDI TLV 生成器和解析器（`TlvGeneratorParser.hpp`、`TlvGenerated_.hpp`、`WiFiCxModel.xml`），库是 `km/<arch>/wlan/2.0/WificxTLVGenParse.lib`；WDI 的消息 ID 和类型在 `km/dot11wificxintf.h` 和 `km/dot11wificxtypes.hpp`。这个库是 C++ 写的，需要驱动提供 `operator new(size_t, ULONG_PTR)` 和 `operator delete`，还要链接 `libcntpr.lib`，并用 `/Zc:sizedDealloc-` 编译。

**26100 WDK 的 1.0 头文件是不是 22621 期望的那份？** WiFiCx 会检查每个能力结构的 `Size`。22621 WDK 的 `km/wificx/1.0/wificx.h`（第三方镜像 WindowsDatabase/WdkCollection）和 26100 那份字段相同、顺序相同，包括 `WIFI_STATION_CAPABILITIES` 末尾的多链路字段；唯一区别是一处改名（`HostWPA3FIPSModeEnabled` 改成 `..._Deprecated`）。22621 的头文件没有 `*_INIT` 函数，所以驱动自己设 `Size = sizeof(...)`。（21H2 即 22000 的头文件更短：站点能力少三个字段。21H2 不在目标之内。）

**WDI 版本。** TLV 带版本（有些字段从 WDI 1.1.4、2.0.5 等版本才加入）。驱动声明它编译时的版本（`WDI_VERSION_LATEST`，这个 WDK 里是 2.0.12），按操作系统报告的版本（`WifiDeviceGetOsWdiVersion`）解析和生成 TLV，和微软的示例、rtw89-win 的做法一样。22621 报告的是哪个 WDI 版本现在还不知道；驱动会记下来（`OsWdiVersion`）。

**没有公开的 1.0 示例。** 微软的示例（Windows-driver-samples 的 `network/wlan/wificx`，MS-PL）是 WiFiCx 1.2、NetAdapterCx 2.5/2.6，硬件是模拟的；它用了 1.1 以后的字段，也不处理密钥。最接近的真实驱动是 rtw89-win（RK3588 上的 RTL8852BE，BSD-3-Clause，WiFiCx 1.1，build 26100），它的 `docs/en/observed-behavior.md` 记录了 WiFiCx 的实际行为；下文的编号（SCAN-3、CONN-5 等）指的就是它。两者的代码都没有复制。

## WiFiCx 对驱动的要求

**初始化。**

1. `EvtDriverDeviceAdd`：`NetDeviceInitConfig`，然后 `WifiDeviceInitConfig`，然后 `WdfDeviceCreate`；接着 `WIFI_DEVICE_CONFIG_INIT`（WDI 版本、`EvtWifiDeviceSendCommand`、`EvtWifiDeviceCreateAdapter`、`EvtWifiDeviceCreateWifiDirectDevice`）和 `WifiDeviceInitialize`。
2. `EvtDevicePrepareHardware`：报告能力，用 `WifiDeviceSetDeviceCapabilities`、`...StationCapabilities`、`...BandCapabilities`、`...PhyCapabilities`（WiFiCx 会复制一份）。WiFiCx 1.0 从来不发 `WDI_TASK_OPEN`、`WDI_GET_ADAPTER_CAPABILITIES` 或 `WDI_TASK_CREATE_PORT`。
3. WiFiCx 调用 `EvtWifiDeviceCreateAdapter` 要站点端口：驱动做 `NetAdapterCreate`、`WifiAdapterInitialize`、设置 NetAdapterCx 能力、`NetAdapterStart`。这个适配器的链路状态由 WiFiCx 自己报告（CONN-6）。

**命令。** 每个 `WIFIREQUEST` 里是一个 `WDI_MESSAGE_HEADER`（端口、状态、事务 ID、IHV ID）加上若干 TLV（类型和长度各 16 位，小端）。`EvtWifiDeviceSendCommand` 可能在 DISPATCH_LEVEL 调用，所以驱动只把请求排队，在自己的线程上逐个处理，每个请求恰好完成一次（INIT-3）。

- **属性**（`WDI_SET_*`、`WDI_GET_*`）完成（`WifiRequestComplete`，即 M3）就结束。
- **任务**（`WDI_TASK_*`）接受时给 M3，之后再发完成通知（M4，用 `WifiDeviceReceiveIndication`，事务 ID 相同），顺序永远是先 M3 后 M4。M3 失败就没有 M4。
- **主动通知**（BSS 列表、关联结果、链路状态、断开）的事务 ID 是 0。

**Windows 发给站点的消息**（rtw89-win 的 SCAN-1、CONN-1，在 26100 上观察到）：

| 时机 | 消息 |
|---|---|
| WLAN 服务启动 | `SET_ADAPTER_CONFIGURATION`（85）、`TASK_DOT11_RESET`（7）、`GET_STATISTICS`（34）、被 `ABORT_TASK`（75）中止的 `TASK_SCAN`（4）、`SET_LOCATION_PRIVACY`（138）、`TASK_SCAN` |
| 空闲 | 每 60 秒一次 `TASK_SCAN` |
| 连接 WPA2 个人网络 | `TASK_DOT11_RESET`、`GET_STATISTICS`、`SET_PRIVACY_EXEMPTION_LIST`（28，EtherType 0x888E）、`TASK_CONNECT`（6）；关联后 `SET_ADD_CIPHER_KEYS`（29）先装成对密钥，再装组密钥 |
| 断开 | `TASK_DISCONNECT`（8）、`TASK_DOT11_RESET` |
| 无线开关 | `TASK_SET_RADIO_STATE`（11） |

**扫描。** 听到的每个 beacon 和 probe response 都放进 `WDI_INDICATION_BSS_ENTRY_LIST` 往上报（BSSID、信号、信道和频段、从 Timestamp 开始的帧体，不带 802.11 头：SCAN-3），然后是 `WDI_INDICATION_SCAN_COMPLETE`，被中止时也要发（状态 `NDIS_STATUS_REQUEST_ABORTED`，SCAN-5）。`BSSListCachemanagement` 为 FALSE 时，列表由 Windows 保存，它从不来要（SCAN-2）。`ABORT_TASK` 用裸消息 ID 指明任务，不是 OID 形式（SCAN-4）。

**连接。** `TASK_CONNECT` 带连接参数（SSID、认证和加密算法列表、允许和禁止的 BSSID，其中广播地址表示"任意"：CONN-2）和候选 BSS，每个都附 beacon 或 probe response 帧体。驱动对试过的 BSS 报告 `WDI_INDICATION_ASSOCIATION_RESULT`；成功时带上关联请求帧体（Windows 从里面取我们的 RSN 元素放进消息 2）、响应帧体、AP 的 beacon 帧体（用来核对消息 3）、`PortAuthorized` 为 FALSE（CONN-5）；然后是 `WDI_INDICATION_LINK_STATE_CHANGE` 和 `WDI_INDICATION_CONNECT_COMPLETE`。AP 走掉时发 `WDI_INDICATION_DISASSOCIATION`；Wlansvc 随后带 `RoamRequest` 重新连接（CONN-7）。

## WPA2 由谁来做

Windows。对 WPA2 个人网络，它在数据通路上做四次握手和组密钥握手（EAPOL 帧作为 802.11 数据帧，EtherType 0x888E，由隐私豁免列表免于加密），再用 `WDI_SET_ADD_CIPHER_KEYS` 装密钥：关联后约 0.4 秒装成对密钥，不带密钥 ID，然后用 AP 给的密钥 ID 装组密钥（rtw89-win CONN-1，硬件上观察）。WPA3-SAE 也在主机上做，通过 `SAE_AUTH_PARAMS_NEEDED` 和 `SET_SAE_AUTH_PARAMS`。WiFiCx 1.0 里没有让固件做四次握手的能力项；`HostFIPSModeImplemented` 保持 0。

所以 WiFiCx 模式不用我们的 supplicant：`aic_wpa.c` 留在二进制里给以太网式模式用，在 WiFiCx 模式下闲着。AIC8800D80 的固件不需要新东西：设了 `CONTROL_PORT_HOST` 以后，它本来就把 EAPOL 交上来，在主机打开端口（`ME_SET_CONTROL_PORT`）之前挡住其他帧，并用 `MM_KEY_ADD` 收密钥，Linux 上 wpa_supplicant 那条路也是这样。

要留意一个竞争：Windows 发出消息 4 后马上就把成对密钥交下来；如果密钥先于消息 4 到达固件，AP 收到的消息 4 就是加密的，会丢掉。驱动在 `MM_KEY_ADD` 之前等 USB 数据队列清空（最多 50 ms，再多等 10 ms）。

## 数据通路

WiFiCx 通过 NetAdapterCx 的队列收发**原生 802.11 数据帧**（`NetPacketLayer2TypeIeee80211`；INF 里 `*MediaType` 为 16，即 NdisMediumNative802_11），Windows 在上层和以太网互转（rtw89-win，硬件上验证过）。一个 TX 队列加一个 RX 队列就够；TX 分流是可选的，而且没配分流就调用 `WifiAdapterAddPeer` 会蓝屏（HS-8）。

AIC8800D80 的固件收的是以太网负载，802.11 头由它自己构造；它交上来的是已解密的 802.11 帧，内核原本就会把它们转成以太网帧（去掉 802.11 头、安全头和 LLC/SNAP，拆开 A-MSDU）。所以 WiFiCx 的数据通路就是在原有转换外面再包两个小转换：

- **RX**：`aic_eth_to_80211` 给每个以太网帧加上 24 字节的数据帧头，置 From DS（A1 为目的地址，A2 为 AP，A3 为源地址），再加 RFC 1042 头（AppleTalk ARP 和 IPX 用 802.1H），每个缓冲区一个 MSDU，和 rtw89-win 一样。
- **TX**：`aic_80211_to_eth` 把 Windows 发来的 To DS 帧（允许 QoS 和 +HTC 头，拒绝受保护的帧）还原成以太网帧，交给 `aic_build_tx`。

从关联起帧就开始流动（最先是 EAPOL）。关联是在固件的 `SM_CONNECT_IND` 之后才报给 Windows 的，但握手的消息 1 可能和它在同一个 USB 传输里到达，所以收到的帧先在驱动的 FIFO 里等着，直到 `ASSOCIATION_RESULT` 报上去（`RxHold`）。

## WiFiCx 模式下的 aic8800usb

```
  设置 / 任务栏 / netsh wlan（系统自带：wlansvc、nwifi）
  WiFiCx 1.0（wificx.sys）  +  NetAdapterCx 2.2
  ------------------------------------------------------------
  aic8800usb.sys（服务 aic8800usb，Mode 1）
    wificx.cpp   WDI 命令：排队，在工作线程上分派，
                 能力，站点适配器，通知
                 （WificxTLVGenParse.lib）
    netadapter.c 同一套 TX/RX 队列，这个模式下走 802.11 帧
    driver.c     PnP、Mode、工作线程（运行 AicWifiRun）
    usb.c        bulk 管道、连续读取、ops->scan_bss
    -- 可移植内核（主机上测试） --
    aic_wlan.c   aic_scan_start、aic_connect_start、aic_bss_from_body、
                 aic_assoc_req_body/rsp_body、aic_host_set_key、事件
    aic_data.c   aic_eth_to_80211、aic_80211_to_eth；EAPOL 当数据上交
    （aic_wpa.c  这个模式下不用）
```

| WDI 消息 | 驱动做什么 |
|---|---|
| `TASK_SET_RADIO_STATE` | 关：结束扫描和连接；状态变了就发 `RADIO_STATUS`；M4 |
| `TASK_DOT11_RESET` | 结束扫描和连接任务，离开网络（`SM_DISCONNECT`），M4 |
| `TASK_SCAN` | `SCANU_START`（所有信道，通配 SSID）；每个结果经 `ops->scan_bss` 进缓冲区；`SCANU_START_CFM` 时发 `BSS_ENTRY_LIST`（每条 16 项）和 `SCAN_COMPLETE`。固件上一次扫描没完就先等它 |
| `ABORT_TASK` | 扫描：立即把听到的报上去，发中止状态的 `SCAN_COMPLETE`（固件自己扫完）；连接：`SM_DISCONNECT`，发中止状态的 `CONNECT_COMPLETE` |
| `TASK_CONNECT` | 取第一个开放的、或 RSN + PSK + CCMP 的候选，用 Windows 交回来的帧体重建（`aic_bss_from_body`）；发 `SM_CONNECT`，带我们的 RSN 元素和 `CONTROL_PORT_HOST`；收到 `SM_CONNECT_IND` 后发 `ASSOCIATION_RESULT`、`LINK_STATE_CHANGE`（65 Mbit/s，HT20）、`CONNECT_COMPLETE`；失败或超时（`ConnectTimeoutMs`）：失败的 `ASSOCIATION_RESULT` 和失败的 `CONNECT_COMPLETE`。没有能用的候选：M3 返回 `STATUS_NOT_SUPPORTED` |
| `TASK_DISCONNECT` | `SM_DISCONNECT`、`DISASSOCIATION`（主机断开）、`DISCONNECT_COMPLETE` |
| `SET_ADD_CIPHER_KEYS` | 只支持 CCMP：成对密钥（等 TX 队列清空后）`MM_KEY_ADD`，再 `ME_SET_CONTROL_PORT` 打开端口；组密钥按它的密钥 ID `MM_KEY_ADD` |
| `SET_ADAPTER_CONFIGURATION`、`SET_PRIVACY_EXEMPTION_LIST`、`SET_DELETE_CIPHER_KEYS`、`SET_DEFAULT_KEY_ID`、`SET_CONNECTION_QUALITY`、`SET_RECEIVE_PACKET_FILTER`、`SET_MULTICAST_LIST`、`SET_FLUSH_BSS_ENTRY` | 接受，不用做什么（EAPOL 由固件自己放行，地址过滤也由固件做；密钥随站点表项一起删除） |
| 其他（`GET_STATISTICS`、`SET_LOCATION_PRIVACY`、P2P、AP 等） | `STATUS_NOT_SUPPORTED`（对站点来说没问题：SCAN-1） |
| 固件的 `SM_DISCONNECT_IND` | `DISASSOCIATION`（对端解除认证，或无法维持）；Windows 会重连 |

**报告的能力**：无线开启、固件版本、1x1、蓝牙共存由固件处理；扫描 SSID 列表 1，隐私豁免列表 8，不支持 WEP、MFP 和自动省电（省电是关的），BSS 列表由操作系统保存；单播和组播数据为 open/none 和 RSNA-PSK/CCMP，管理帧为 open/none；2.4 GHz 信道 1 到 13（HR/DSSS、ERP、HT），芯片支持时 5 GHz 信道 36 到 165（OFDM、HT），只有 20 MHz；每种 PHY 的传统速率。不报告 Wi-Fi Direct 能力，所以没有 Wi-Fi Direct 设备，也没有移动热点；`EvtWifiDeviceCreateWifiDirectDevice` 直接拒绝。

第一版没有的：隐藏网络（扫描只用通配 SSID）、WPA3、PMF、TKIP 组密钥、40/80 MHz 和 VHT/HE、驱动在同一网络的多个 AP 之间漫游、省电、`GET_STATISTICS`、WLAN 唤醒。

## INF、模式与切换

`aic8800wifi.inf` 就是 `aic8800usb.inf` 换成 Wi-Fi 的网络关键字：

| | `aic8800usb.inf`（Mode 0） | `aic8800wifi.inf`（Mode 1） |
|---|---|---|
| `*IfType` | 6（以太网） | 71（IEEE 802.11） |
| `*MediaType` | 0（802.3） | 16（原生 802.11） |
| `*PhysicalMediaType` | 14（802.3） | 9（原生 802.11） |
| `Characteristics` | 0x04 | 0x84（和微软示例一样） |
| `LowerRange` | `ethernet` | `wlan,ethernet` |
| `NumberOfNetworkInterfaces` | | 1（只有站点；类安装程序安装时会预留这么多个适配器，rtw89-win INST-1） |
| Device Parameters 的 `Mode` | 0 | 1 |

不需要 `Include`/`Needs` 行（微软的文档和示例 INF 都没有）；`KmdfService` 和普通 KMDF 驱动一样。两个 INF 都绑定 `USB\VID_A69C&PID_8D81&MI_02`，装同一个服务和同一个二进制，写 `Mode` 时都不带 NOCLOBBER，因为 Device Parameters 在换驱动后还会保留。驱动在 `EvtDriverDeviceAdd` 里读 `Mode`（设备创建之前就要决定是否初始化 WiFiCx），并记下 `ActiveMode`。

驱动库里只能有其中一个（两者排名相同）。`aic-bringup.ps1 -Mode WiFiCx` 会把 `aic8800usb.inf` 的包从设备和驱动库里删掉，暂存 `aic8800wifi.inf`，并把 `Stage` 提到 4（WiFiCx 模式需要固件初始化，即 Stage 2 或以上）；然后重启 Windows。`-Mode Ethernet` 切回去。彻底卸载再安装会改变接口 GUID，Windows 的配置文件也跟着没了（INST-2）：切换后要准备重新输一次密码。

## 注册表值

除了[以太网式模式的那些](aic8800.zh-CN.md#注册表值)，运行时设备的 Device Parameters 里还有：

| 值 | 含义 |
|---|---|
| `Mode` | INF 写的：0 以太网式，1 WiFiCx |
| `ActiveMode` | 驱动启动时用的模式 |
| `WifiInitStatus`、`OsWdiVersion`、`DriverWdiVersion` | `WifiDeviceInitialize` 的结果；操作系统用的 WDI 版本和驱动声明的版本 |
| `WifiCapsStatus`、`WifiDeviceCapsStatus`、`WifiStationCapsStatus`、`WifiBandCapsStatus`、`WifiPhyCapsStatus` | 每次能力调用的结果（0 为成功） |
| `AdapterStartStatus`、`WdiPortId` | 站点适配器 |
| `WdiTrace` | 收到的前 64 个 WDI 消息 ID，按顺序 |
| `WdiMessages`、`WdiUnsupported`、`WdiRefused`、`WdiIndications`、`WdiIndicationErrors` | 计数 |
| `WdiScanTasks`、`WdiBssIndicated`、`WdiConnectTasks`、`WdiLastConnectStatus`、`WdiKeys`、`WdiKeyErrors`、`WdiLastKeyStatus`、`WdiDisassociations`、`WdiRadioOn`、`WdiConnected` | 发生了什么 |

设置（服务键）：`WdiVersion`（DWORD，默认 0，即 WDK 的 `WDI_VERSION_LATEST`）可以改驱动声明的版本，以防 22621 的 WiFiCx 不认较新的版本号。`ConnectTimeoutMs` 限定一次连接任务的时长。这个模式不用 `Ssid`、`Passphrase` 和 `Pmk`。

## 测试

`tools/aictest`（CI 任务 `aictest`，也在 ASan/UBSan 下跑）对着模拟芯片和模拟 AP 扮演 Windows：

- 不阻塞地扫描：每个结果都到达 `scan_bss`，帧体从 Timestamp 开始（8 处是 beacon 间隔，10 处是能力，12 处是 SSID）；
- 清空网络列表，再用 Windows 会交回来的帧体重建（`aic_bss_from_body`），按 BSSID 找到；
- 不给密码就连接；关联事件；消息 1 留给操作系统；关联请求帧体里有 SSID 和我们的 RSN 元素，响应帧体状态为 0 并带 AID；
- 测试程序自己的 supplicant（PRF-384 算 PTK、MIC、AES 密钥解包）在 802.11 帧上做四次握手，帧的构造和解析用的是和驱动相同的转换函数，用到的只有驱动报告的东西：消息 2 带的是报告的关联请求里的 RSN 元素（AP 会核对），消息 3 的 RSN 元素等于 beacon 里的；然后 `aic_host_set_key` 装成对密钥（端口打开）和组密钥；
- 以 802.11 帧双向向网关发 ARP；AP 解除认证时产生断开事件；
- 单独测转换函数：满长帧、IPX 用 802.1H、QoS 和 +HTC 头、拒绝受保护帧和管理帧、不带 SNAP 的 LLC 帧、过短的帧。

主机上测不到的：WiFiCx 本身（TLV 在运行时由微软的库生成）。Windows 任务用 WDK 的 WiFiCx 1.0 和 TLV 头文件编译 `wificx.cpp` 并链接那个库。

## 板上测试计划

ROCK 4D，Windows 11 23H2，开测试签名，用网线访问，CI 产物 `aic8800-bringup-signed-arm64`，来自分支 `aic8800-wificx`。先按 [aic8800.zh-CN.md](aic8800.zh-CN.md#板上测试计划) 做以太网式模式的阶段，至少到阶段 2（固件下载和初始化先要能用，WiFiCx 才可能用）。测试期间打开定位服务（`netsh wlan show networks` 需要它，SCAN-7）。保留固件启动的串口日志。

1. **切换。** `aic-bringup.ps1 -Mode WiFiCx`，重启。通过：设备名为"AIC8800D80 USB Wi-Fi (WiFiCx)"，没有问题代码；`-Status` 显示 `ActiveMode` 1、`WifiInitStatus` 0、所有 `Wifi*CapsStatus` 为 0、`AdapterStartStatus` 0；记下 `OsWdiVersion`。`Get-NetAdapter` 把它列为"Wi-Fi"（媒体类型原生 802.11）；`netsh wlan show interfaces` 能列出它，`netsh wlan show drivers` 显示驱动名，"支持的无线电类型"有 802.11n，认证方式里有"WPA2-个人 CCMP"。要记录的失败情况：代码 10 或 31（看事件日志和 `WifiCapsStatus`；`Size` 被拒会表现为 `WifiStationCapsStatus` 非 0）；适配器起来了但没有 WLAN 接口（这时试试像 rtw89-win 那样把 `NumberOfNetworkInterfaces` 设成 3）。
2. **扫描。** 打开任务栏的 Wi-Fi 列表。通过：10 秒内出现家里的网络和邻居的网络，信号格合理，刷新后还在；`WdiTrace` 的开头和 SCAN-1 一样（85 7 34 4 75 ...）；空闲时 `WdiScanTasks` 大约每分钟加一，`WdiBssIndicated` 跟着涨。和 Armbian 上的 `iw dev wlan0 scan` 对比。
3. **连接 WPA2 个人网络。** 点家里的网络，输密码。通过：15 秒内显示"已连接，安全"；`WdiConnected` 1、`WdiKeys` 2、`WdiKeyErrors` 0、`LinkState` 3；`ipconfig` 有 DHCP 地址；拔掉网线后 `curl https://example.com` 能用；下载 100 MB 能完成（记下速度）。5 次冷启动各连一次，共 5 次，必须全部通过，而且每次启动 Windows 都能自己重连（配置文件）。输错密码：Windows 会提示（驱动不用改什么）。
4. **开放网络**（不设密码的手机热点）：能连上，拿到 DHCP。
5. **断开与重连**：在列表里点"断开连接"，再连上；关掉路由器再打开（Windows 自己重连：`WdiDisassociations` 增加，之后 `WdiConnected` 又是 1）；开关飞行模式（`WdiRadioOn`）。
6. **切回以太网式**：`aic-bringup.ps1 -Mode Ethernet`，重启，阶段 4 和以前一样能用。

如果握手一直完不成（Windows 显示"无法连接"，`WdiKeys` 为 0）：看 `RxEapol`（消息 1 到了驱动）和 `NetRxFrames`（也到了 Windows），`NetTxFrames`（消息 2 发出去了）；消息 2 里的 RSN 元素不对的表现是 AP 一直不发消息 3。如果成对密钥装上了但不通数据，怀疑消息 4 的竞争（AP 日志里缺消息 4）：把 `SetAddCipherKeys` 里的延时加大。

## 风险与未决问题

- **WDI 版本。** 22621 的 `OsWdiVersion` 要等板子告诉我们；TLV 按它来。如果 `WifiDeviceInitialize` 失败，试试把 `Parameters\WdiVersion` 设成 0x10100（WDI 1.1.0）或者报告出来的那个值。
- **能力的大小和取值。** 结构体是 22621 的布局，但 22621 对各个字段怎么校验还不清楚（rtw89-win：不合法的组合会被悄悄丢掉；HS-2：某些 Wi-Fi Direct 取值会让 nwifi 拒绝整个适配器。我们完全不报告 Wi-Fi Direct）。
- **`NumberOfNetworkInterfaces` 为 1** 没测过（rtw89-win 带 Wi-Fi Direct，用的是 3）。
- **隐藏网络**找不到（只用通配 SSID）；`TASK_SCAN` 里的 SSID 列表还没交给固件。
- **消息 4 的竞争**（见上）。
- **链路速率和信号**是固定的估计值（65 Mbit/s，扫描时的 RSSI）；还没读固件报告的速率和信号。
- **连接状态下固件自己扫描**可能影响链路；Windows 在空闲时大约每分钟扫一次，列表打开时更频繁。

## 下一步：bcmwlsdio

同一个前端也适用于 `bcmwlsdio`（泰山派 3M 上的 BCM43456、CM5-IO 上的 BCM43752；分支 `tspi3m-wifi-driver` 和 `cm5io-wifi-bt`），只要它的内核提供同样那几个调用；WDI 那一半（`wificx.cpp`）几乎可以原样搬过去，只是改一下内核调用的名字。Broadcom 内核需要的（和它其余部分一样参照 OpenBSD bwfm）：

- 扫描：`escan` 结果里有带元素的 `wl_bss_info`；用它给 Windows 拼帧体（8 个字节的零作 Timestamp、beacon 间隔、能力，然后是元素）；
- 连接：WPA2-PSK 设 `wsec`/`wpa_auth`，`sup_wpa` 设 0 让固件把握手交给主机（FullMAC 固件也可以在 `sup_wpa` 为 1 时拿 PMK 自己做，WiFiCx 用不上），然后 `join`；关联元素从 `assoc_req_ies`/`assoc_resp_ies` 取；
- EAPOL 当数据（固件的 supplicant 关掉时会把 0x888E 交上来）；密钥用 `wsec_key`；
- 数据：Broadcom 的固件同样用以太网（BDC 头），所以同样那两个 802.11 转换也适用。

## 参考来源

- 微软：[WiFiCx 概述](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/wifi-wdf-class-extension-wificx)、
  [编写 WiFiCx 客户端驱动](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/writing-a-wificx-client-driver)、
  [为多个 Windows 版本构建 WiFiCx 驱动](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/building-a-wificx-driver-for-multiple-versions-of-windows)、
  [WiFiCx 消息结构](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/wificx-message-structure)、
  [OID_WDI_TASK_CONNECT](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/oid-wdi-task-connect)、
  [OID_WDI_TASK_SCAN](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/oid-wdi-task-scan)、
  [OID_WDI_SET_ADD_CIPHER_KEYS](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/oid-wdi-set-add-cipher-keys)、
  [NetAdapterCx 客户端驱动的 INF 文件](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/inf-files-for-netadaptercx-client-drivers)。
- 上文提到的 WDK 10.0.26100.0 头文件；22621 WDK 的 `km/wificx/1.0/wificx.h`，来自 WindowsDatabase/WdkCollection 镜像，逐字段对比过。
- [微软的 WiFiCx 示例](https://github.com/microsoft/Windows-driver-samples/tree/main/network/wlan/wificx)（MS-PL）。
- [rtw89-win](https://github.com/Meari-Prototype/rtw89-win)（BSD-3-Clause），`docs/en/observed-behavior.md`：WiFiCx 在硬件上的行为（build 26100）。
