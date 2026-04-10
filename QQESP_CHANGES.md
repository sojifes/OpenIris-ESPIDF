# QQESP 板子支持 + 顺手修了几个项目级 bug

本次工作的目的:为 **QQESP**(标识 `XIAOMO_VR_DEVICE_V1`)板子添加固件支持。
QQESP 是一块基于 ESP32-S3R2 的 OpenIris 改版硬件,主要硬件差异:

- 摄像头 PWDN 引脚在 **GPIO40**(其他 wrooms3 板默认 -1)
- IR LED 在 **GPIO41**,**P-MOSFET 高边驱动**(active-low,需要反相 PWM)
- 电池监测在 **GPIO1 / ADC1_CH0**,2:1 分压(10kΩ / 10kΩ)
- **没有** LED 电流检测电路(QQESP 板上没有 shunt + 运放)
- Camera XCLK 硬编码 **20 MHz**(QQESP 原版 Arduino 固件如此)
- IR 默认亮度 **82%**(对应 QQESP 原版 `ir_brightness = 210/255`)

## 修改清单

### 新增文件

#### `boards/qqesp/qqesp` — QQESP 板级 sdkconfig overlay

基于 wrooms3 的摄像头引脚 + QQESP 特有覆盖。关键覆盖:

```
CONFIG_PWDN_GPIO_NUM=40
CONFIG_LED_EXTERNAL_GPIO=41
CONFIG_LED_EXTERNAL_CONTROL=y
CONFIG_LED_EXTERNAL_ACTIVE_LOW=y      # 新增 Kconfig,见下文
CONFIG_LED_EXTERNAL_PWM_FREQ=5000
CONFIG_LED_EXTERNAL_PWM_DUTY_CYCLE=82
CONFIG_MONITORING_BATTERY_ENABLE=y
CONFIG_MONITORING_BATTERY_ADC_GPIO=1
CONFIG_MONITORING_BATTERY_DIVIDER_R_TOP_OHM=10000
CONFIG_MONITORING_BATTERY_DIVIDER_R_BOTTOM_OHM=10000
# CONFIG_MONITORING_LED_CURRENT is not set    # QQESP 没这个电路,显式禁用
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y      # 用正确的 ESPTOOLPY_ 前缀
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_GENERAL_BOARD="XIAOMO_VR_DEVICE_V1"
CONFIG_GENERAL_INCLUDE_UVC_MODE=y
CONFIG_CAMERA_USB_XCLK_FREQ=20000000
CONFIG_CAMERA_WIFI_XCLK_FREQ=20000000
```

Board key:`qqesp`(`qqesp/qqesp` 路径折叠了重复尾段)。

### 修改文件

#### `main/Kconfig.projbuild` — 新增 `LED_EXTERNAL_ACTIVE_LOW` 选项

新增 bool Kconfig 选项,默认 `n`,depends on `LED_EXTERNAL_CONTROL`。

**理由**:QQESP 这种 P-MOSFET 高边开关的 IR LED 拓扑里,GPIO 输出 LOW 时灯亮、HIGH 时灯灭。
软件需要把 LEDC duty 反相(`raw_duty = 255 - logical_duty`),才能让用户视角的 0-100% 保持直觉
(0=灭、100=最亮),不影响其他用 N-MOSFET 或直驱 LED 的 board。

#### `components/LEDManager/LEDManager/LEDManager.cpp` — P-MOSFET 反相

新增 `static inline uint32_t externalLedRawDuty(uint32_t logicalDuty)` helper,
集中反相逻辑(`#ifdef CONFIG_LED_EXTERNAL_ACTIVE_LOW` 时返回 `255 - logicalDuty`,
否则原样返回)。三个写 LEDC duty 的位置全部走这个 helper:

1. `setup()` — 初始 PWM duty
2. `setExternalLEDDutyCycle()` — 运行时调亮度
3. `toggleLED()` — 错误状态闪烁的 mirror duty

注意:`updateState()` 里的 store/restore 逻辑**不需要**改 — 它通过 `ledc_get_duty()` /
`ledc_set_duty()` 操作的是 raw 寄存器值,两端对称,polarity 反相已经包含在内。

#### `components/RestAPI/RestAPI/RestAPI.cpp` — 两处改动

1. **注册 `/api/get/battery/` 路由**,挂到现有 `CommandType::GET_BATTERY_STATUS`。
   命令早就实现了(`device_commands.cpp:215 getBatteryStatusCommand`),只是没暴露到 HTTP。

2. **mongoose log 重定向到 ESP_LOG + 降级到 `MG_LL_ERROR`**(详见 bug #3)。

#### `main/openiris_main.cpp` — REST 任务栈大小

`HandleRestAPIPollTask` 栈大小从 `2024 * 2`(4 KB,有 typo)改成 `1024 * 8`(8 KB)。
**修复 bug #1**(详见 bug 清单)。

#### `components/CommandManager/CommandManager/CommandManager.cpp` — 真正解析 body

`executeFromType` 现在真正用 `nlohmann::json::parse()` 解析 HTTP body,而不是把
`std::string_view` 隐式构造成一个 STRING 类型的 JSON 值。**修复 bug #2**(详见下文)。

#### `.github/workflows/build-and-release.yml` — CI 矩阵

加 `{board_name: qqesp, target: esp32s3}` 一项。

#### `sdkconfig` / `sdkconfig.old` — 切到 qqesp 状态

项目历史上每次提交都把 sdkconfig 一起提交,反映"当前 fork 主用 board"是 qqesp。
其他人 clone 后想构建别的板子,先跑 `python3 ./tools/switchBoardType.py --board <name>`。

---

## 注意事项 / 构建坑

### ⚠️ 不要在 `switchBoardType.py` 之后跑 `idf.py set-target`

`set-target` 是**破坏性操作**:它会丢弃当前 sdkconfig 重新生成,**不读
`boards/sdkconfig.base_defaults`**。结果是 `CONFIG_HTTPD_WS_SUPPORT=y` 等关键配置被丢掉,
然后 `StreamServer.cpp:146` 引用的 `httpd_uri_t::is_websocket` 字段消失,编译报错:

```
error: 'httpd_uri_t' has no non-static data member named 'is_websocket'
```

**正确流程**:

```bash
python3 ./tools/switchBoardType.py --board qqesp
idf.py build         # 或 idf.py reconfigure
```

如果 build 目录从未初始化过(全新 clone),第一次需要 `set-target` 一次,然后再用
`switchBoardType.py` 切板子,顺序倒一下:

```bash
idf.py set-target esp32s3                       # 一次性,生成空白 sdkconfig
python3 ./tools/switchBoardType.py --board qqesp # 覆写到 qqesp
idf.py build                                     # 编译
```

### `dutyCycle=100` 不会自动持久化到 NVS

PATCH `/api/update/led_duty_cycle/` 只改运行时状态,**重启后会回到 NVS 里的旧值**
(默认从 `CONFIG_LED_EXTERNAL_PWM_DUTY_CYCLE=82` 来)。要持久化必须再 POST 一次:

```
POST /api/set/config/save/
```

### 烧录时 `flash_args.json` 显示 `--flash_mode dio` 是正常的

ESP-IDF 的 Kconfig 故意把 `ESPTOOLPY_FLASHMODE_QIO/QOUT/DIO` 都映射到 .bin 头部的
`"dio"` 字符串(只有 DOUT 例外)。原因:ESP32 ROM bootloader 只能用 DIO 加载第二阶段
bootloader,然后第二阶段 bootloader 读 `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` 在运行时切到 QIO。

启动 log 里 `flash io: qio` 才是真实运行模式:

```
I (33) qio_mode: Enabling default flash chip QIO
I (38) boot.esp32s3: SPI Mode       : QIO
...
I (702) spi_flash: flash io: qio
```

### LED 电流监测在 QQESP 上被禁用

其他板子默认开 `CONFIG_MONITORING_LED_CURRENT=y`,QQESP 显式关掉。原因:
QQESP 没有 shunt + 运放电流检测电路,默认 GPIO 3 在 QQESP 上是悬空(或别的功能)。
如果开着,`/api/get/led_current/` 会返回约 1.27 mA 的"电流",其实是 ADC baseline 噪声
(实测:dutyCycle=0 → 1.273 mA,dutyCycle=100 → 1.409 mA,几乎不变)。

### 烧录命令(参考)

完整烧录(全部 4 个 binary):

```powershell
python -m esptool --chip esp32s3 -p COMxx -b 460800 write-flash `
  0x0     bootloader.bin `
  0x8000  partition-table.bin `
  0xe000  ota_data_initial.bin `
  0x10000 blink.bin
```

只烧 app 增量(开发时常用,bootloader / 分区表不变):

```powershell
python -m esptool --chip esp32s3 -p COMxx -b 460800 write-flash 0x10000 blink.bin
```

注意:**esptool v5.x 把命令名 `write_flash` 改成了 `write-flash`**(中划线)。

---

## Bug 清单

### 已修复的项目级 bug(全部是项目原有,不是这次工作引入的)

| # | bug | 位置 | 严重度 | 状态 |
|---|---|---|---|---|
| 1 | `HandleRestAPIPollTask` 栈 4 KB 太小,任何调 `getBatteryStatusCommand` / `getLEDCurrentCommand` 的 REST 请求都会 stack overflow 崩溃 | `main/openiris_main.cpp:223` | **高(运行时崩溃)** | ✅ 已修(4 KB → 8 KB) |
| 2 | `CommandManager::executeFromType` 不解析 HTTP body — 11 个 REST PATCH/POST 端点全部失效 | `components/CommandManager/CommandManager/CommandManager.cpp:136` | **致命** | ✅ 已修 |
| 3 | mongoose log 默认 `MG_LL_DEBUG` 级别 + 自有时间戳/格式,串口刷屏每次 read/write | `components/RestAPI/RestAPI/RestAPI.cpp:78` | 中(噪音) | ✅ 已修(降级到 ERROR + 重定向到 ESP_LOG) |
| 4 | `2024 * 2` 应该是 `2048 * 2` 的 typo | `main/openiris_main.cpp:223` | 极低 | ✅ 修栈大小时一并改了 |

#### bug #2 的细节(最严重的发现)

```cpp
// 旧代码
CommandManagerResponse CommandManager::executeFromType(const CommandType type, const std::string_view json) const
{
    const auto command = createCommand(type, json);  // ← BUG
    ...
}
```

`createCommand` 的签名是 `createCommand(CommandType, const nlohmann::json&)` —
它要求一个**已经解析好的 nlohmann::json 对象**。但这里传的 `json` 是 `std::string_view`
(原始 HTTP body 字符串)。

`nlohmann::json` 有从字符串的隐式构造,**那个构造把字符串当成 JSON STRING 类型的值**
(就像 `"{\"dutyCycle\":100}"` 是个字符串字面量),**不会去解析它**。所以命令处理
函数里的 `json.contains("dutyCycle")` 永远返回 false,所有读 body 字段的 PATCH/POST
端点全部返回 `"Invalid payload - missing X"` 错误。

对比:`executeFromJson`(串口/CDC 通道用的)写得是对的:

```cpp
nlohmann::json parsedJson = nlohmann::json::parse(json);   // ← 真的解析
const auto commandPayload = commandData.contains("data")
    ? commandData["data"]
    : nlohmann::json::object();
auto command = createCommand(commandType, commandPayload);
```

**影响范围**:REST API 上 11 个 PATCH/POST 端点(全部)从这次修复后才真正可用:

- `/api/update/wifi/` (UPDATE_WIFI)
- `/api/update/device/mode/` (SWITCH_MODE)
- `/api/update/camera/` (UPDATE_CAMERA)
- `/api/update/ota/credentials` (UPDATE_OTA_CREDENTIALS)
- `/api/update/ap/` (UPDATE_AP_WIFI)
- `/api/update/led_duty_cycle/` (SET_LED_DUTY_CYCLE)
- `/api/set/pause/` (PAUSE)
- `/api/set/wifi/` (SET_WIFI)
- `/api/set/mdns/` (SET_MDNS)
- `/api/reset/config/` (RESET_CONFIG)

**含义**:这个项目的 REST API PATCH/POST 接口**从来没有真正被任何用户用过**。
所有现存的 OpenIris 用户配置 WiFi、改摄像头参数、切模式,应该全部都走串口/CDC
通道(`executeFromJson` 那条路是对的)。REST API 看起来注册了一堆 PATCH/POST 路由,
但**没有一个能 work**。

### 已发现但**未修复**的项目级 bug(原有,不是这次引入)

| # | bug | 位置 | 严重度 | 备注 |
|---|---|---|---|---|
| 5 | 12 个 board overlay 用错了 `CONFIG_FLASHMODE_QIO`(缺 `ESPTOOLPY_` 前缀) | `boards/*/`(除 qqesp 外所有) | 中(配置静默失效,所有板子的 flash mode 都没真正通过 overlay 设置) | qqesp 用对了,其他 board 未动 |
| 6 | `wrooms3QIO/wrooms3QIO:35` 的 `CONFIG_FLASHMODE_QIO = y` 等号两边有空格 | `boards/wrooms3QIO/wrooms3QIO` | 低(被 #5 掩盖) | 未修 |
| 7 | `WiFiManager` 把 WiFi 密码以明文 `ESP_LOGI` 到串口 — 任何能接到板子串口的人都能读到密码 | `components/wifiManager/wifiManager/wifiManager.cpp` | **高(安全/隐私)** | 用户决定暂时不修 |
| 8 | `Preferences::getString` 把 NVS NOT_FOUND 当 `ESP_LOGE` 打 — 第一次启动时一行红字"错误",其实是预期行为 | `components/Preferences/Preferences/Preferences.cpp:579, 607` | 低(噪音) | 未修 |
| 9 | `wifiManager.cpp:263` `wifi_ap_config_t` 多个字段缺少初始化(`-Wmissing-field-initializers` warning × 11) | `components/wifiManager/wifiManager/wifiManager.cpp` | 低(warning) | 未修 |
| 10 | `device_commands.cpp:107` `esp_timer_create_args_t` 字段缺少初始化(同上) | `components/CommandManager/CommandManager/commands/device_commands.cpp` | 低(warning) | 未修 |
| 11 | `WiFiManager.cpp` log 里 `Stored netoworks failed` — `networks` 拼错 | `components/wifiManager/wifiManager/wifiManager.cpp` | 极低(typo) | 未修 |

### 这次工作引入的 bug 数

**0 个**。所有改动:

- `boards/qqesp/qqesp` 是新文件,只影响 qqesp 这一块板子
- `LED_EXTERNAL_ACTIVE_LOW` Kconfig 默认 `n`,只在 `=y` 时生效,不影响任何现有 board
- `LEDManager.cpp` 的 helper 在 `LED_EXTERNAL_ACTIVE_LOW=n` 时是恒等映射,行为不变
- `RestAPI.cpp` 注册新路由不影响其他路由
- mongoose log 重定向影响所有 board,但只是日志格式更干净
- `executeFromType` 修复让所有 board 的 REST PATCH/POST 都能用了
- REST 任务栈拉大让所有 board 的 REST 调用更稳

---

## 硬件验证(实机测试)

| 项目 | 设置 |
|---|---|
| 板子 | QQESP (XIAOMO_VR_DEVICE_V1) |
| 芯片 | ESP32-S3R2 / 240 MHz / 8 MB Flash / 2 MB PSRAM |
| 烧录工具 | esptool v5.0.2,baud 460800 |
| WiFi | 连到家庭路由器 MERCURY_76EF,IP 192.168.0.107,RSSI -30 dBm |

| 验证项 | 结果 | 证据 |
|---|---|---|
| 编译 | ✅ | `blink.bin` 1.52 MB,app 分区使用 81% / 剩 19% |
| 启动 + ASCII logo | ✅ | 完整 boot log + OpenIris logo |
| QIO flash 模式 | ✅ | `qio_mode: Enabling default flash chip QIO`、`spi_flash: flash io: qio` |
| PSRAM 2 MB | ✅ | `Found 2MB PSRAM device, Speed: 80MHz` |
| 摄像头 PWDN GPIO40 | ✅ | `gpio: GPIO[40]\| OutputEn: 1` |
| OV2640 sensor 检测 | ✅ | `Detected OV2640 camera, PID=0x26 VER=0x42 MIDL=0x7f MIDH=0xa2` |
| Camera module 名字 | ✅ | `Camera module is QQESP` |
| **LED P-MOSFET 反相**(关键) | ✅ | `Setting dutyCycle to: 209 (raw 46)`(209+46=255) |
| **手机相机肉眼验证**(终极) | ✅ | dutyCycle=0 时 IR LED 灭、dutyCycle=100 时 IR LED 亮 |
| Battery 监测启用 | ✅ | `Battery monitor enabled (GPIO=1, scale=2.000)` |
| WiFi 连接 | ✅ | `connected to ap SSID:MERCURY_76EF`,`got ip: 192.168.0.107` |
| Stream server :80 | ✅ | `Stream server started on port 80` |
| REST API :81 | ✅ | mongoose 监听 0.0.0.0:81 |
| `GET /api/get/battery/`(新加的) | ✅ | 返回 `{voltage_mv:"4202.00", percentage:"100.0"}` — 满电锂电池 |
| `GET /api/get/who_am_i/` | ✅ | 返回 `{who_am_i:"XIAOMO_VR_DEVICE_V1", version:"0.0.1"}` |
| `GET /api/get/led_duty_cycle/` | ✅ | 默认返回 82,动态修改后准确反映 100/50/0 |
| `GET /api/get/serial_number/` | ✅ | 返回 `{mac:"88:56:A6:AC:91:08", serial:"8856A6AC9108"}` |
| **`PATCH /api/update/led_duty_cycle/`**(bug #2 修复后) | ✅ | 0/50/100 都成功写入并回读一致 |
| mongoose log 静默 | ✅ | 重定向 + ERROR 级别后,正常运行时 mongoose 不再刷屏 |

### 未在板子上跑过的验证项

- mDNS `qqesp.local` 解析(没在 LAN 里 ping 过)
- MJPEG 视频流(浏览器 `http://192.168.0.107/`)
- UVC 模式切换(QQESP 主用 WiFi,UVC 编译进了但没切过去)
- LED 错误闪烁 mirror(没故意拔摄像头排线触发 `CameraError`)
- 重启后 NVS 配置持久化(没显式重启过)

---

## 客户端兼容性

QQESP 自带的 Windows GUI 客户端 `vrcft_set_1.6.5.py`(本仓库未包含) **不兼容**
当前固件,因为客户端期望:

- HTTP 路径 `/control/builtin/command/<cmd>?<params>` 风格(GET-only,query string)
- 字段 `voltage`(浮点 V)而不是 `voltage_mv`(字符串毫伏)
- 命令名 `setCamera` / `save` / `reboot` 而不是 `update_camera` / `save_config` / `restart_device`
- 串口 baudrate **3,000,000**(3 Mbps)而不是默认的 115200
- 字段 `networkName` 而不是 `name`
- 字段 `ir_brightness`(0-255)而不是 `dutyCycle`(0-100)

本次工作**不做客户端适配** — 用户决定保持 OpenIris-ESPIDF 项目原生 API 风格,
直接用 PowerShell `Invoke-RestMethod` 或串口 JSON 命令操作板子。如果以后想支持
QQESP 客户端,需要在 RestAPI 里加一个 `/control/builtin/command/*` 命名空间的
兼容 shim,详见 `QQESP_PORT_TO_ESPIDF.md`(未包含在仓库)的差异审计。

---

## 仓库里没包含的辅助文件

这些文件在工作目录里**没提交**到仓库:

| 文件 | 原因 |
|---|---|
| `QQESP_PORT_TO_ESPIDF.md` | 第三方编写的 QQESP 移植参考文档(从 Arduino 版迁到 ESP-IDF 的特性 checklist),用户工作笔记 |
| `vrcft_set_1.6.5.py` | QQESP 的 Windows GUI 客户端(VRCFT face tracking,45 KB),第三方代码,版权不明 |
| `QQESP_PORT_TO_ESPIDF.md:Zone.Identifier` | Windows 下载元数据,无用 |
| `build/` | ESP-IDF 构建产物,`.gitignore` 已排除 |
