# ESP32-C6 Switch 2 Pro Controller：實機結果與接手紀錄

## 結論

ESP32-C6 已成功模擬 Nintendo Switch 2 Pro Controller 2，完成第一次配對、重新啟動自動重連、睡眠喚醒，以及喚醒後的 HID 輸入。

關鍵是 ESP32-C6 controller library 的 5 ms connection interval patch。未套 patch 時，Switch 2 會被喚醒，NimBLE 也會建立連線並成功提交 notification，但主機不採用 HID report；套 patch 後，同一套韌體與 NVS 配對資料可在重啟及完整 sleep → wake 流程後收到四次獨立 A。正式 gateway 移植的 radio/protocol 基準門檻已通過。

板上執行的 application 含已驗證的 patch；共用 ESP-IDF 6.1 library 已還原成原始 SHA-256，沒有永久污染工具鏈。

## 硬體與工具鏈

- Board：ESP32-C6FH8，revision v0.2
- Flash：8 MB embedded flash
- USB：USB Serial/JTAG
- Port：COM5
- Base MAC：`B0:A6:04:9A:75:64`
- ESP-IDF：`C:\Users\virgil\.espressif\frameworks\esp-idf-v6.1`
- ESP-IDF version：6.1.0
- RISC-V toolchain：ESP 15.2.0
- GDB：`C:\Users\virgil\.espressif\tools\riscv32-esp-elf-gdb\17.1_20260402`
- OpenOCD：`C:\Users\virgil\.espressif\tools\openocd-esp32\v0.12.0-esp32-20260703`

依使用者要求，**沒有備份燒錄前的 Flash**。

## 上游基準

Repository：<https://github.com/zhantss/ESP32-BLE5-NSController-Emulator>

- 本機 clone：`C:\Users\virgil\.graphify\repos\zhantss\ESP32-BLE5-NSController-Emulator`
- Pinned commit：`0ea0c62aeab2440c7f9c77f29cf87ffa3b4c192f`
- Protocol reference：<https://github.com/ndeadly/switch2_controller_research>
- Target：`esp32c6`
- Controller identity：Nintendo VID `0x057E`、manufacturer `0x0553`、Pro Controller 2 PID `0x2069`

### ESP-IDF 6.1 build

執行：

```bat
call C:\Users\virgil\.espressif\frameworks\esp-idf-v6.1\export.bat
idf.py set-target esp32c6
idf.py build
```

第一次 production image：

- `ESP32-BLE5-NSController-Emulator.bin`
- Size：`0x9c6a0` / 640,672 bytes
- App partition 剩餘 58%
- Build 成功

上游 patch verifier：

```bat
python patch\patch_nimble_lib.py ^
  --idf-path C:\Users\virgil\.espressif\frameworks\esp-idf-v6.1 ^
  --target esp32c6 --verify-only
```

結果：`Patch NOT found!`

因此本次成功初次配對不是依賴上游的 5 ms binary patch。除非後續 connection-event 證據顯示必要，不應先修改共用 ESP-IDF library。

## 初次燒錄與配對結果

第一次透過 esptool/COM5 燒錄成功；bootloader、partition table、application 都通過 hash verification，最後 hard reset。

Windows BLE active scan 實際看到：

- Address：`78:81:8C:33:45:C5`
- Connectable：`true`
- Manufacturer company ID：`0x0553`
- Manufacturer payload prefix：`01 00 03 7E 05 69 20`
- `0x057E` = Nintendo VID
- `0x2069` = Pro Controller 2 PID

使用者在 Switch 2 的「變更握法／順序」畫面完成初次配對。這證明以下路徑可用：

1. ESP32-C6 + ESP-IDF 6.1 NimBLE 啟動
2. Nintendo 廣播識別資料正確
3. Switch 2 接受 GATT layout 與配對流程
4. 未套 5 ms patch 仍可完成首次連線

## EasyCon 輸入測試版本

上游預設 `CONFIG_PROTOCOL_LAYER_SIMPLE=y`，但該選項在目前 commit 中沒有掛接 protocol instance，因此 USB transport 無法接收控制輸入。為了做實機按鍵測試，外部 clone 的 `sdkconfig` 已改成：

```text
# CONFIG_PROTOCOL_LAYER_SIMPLE is not set
CONFIG_PROTOCOL_LAYER_EASYCON=y
CONFIG_PROTOCOL_LAYER=1
```

重新 build 後：

- Image size：`0x9d050`
- Build 成功

此時普通 esptool 自動 reset 無法讓已執行 USB Serial/JTAG transport 的裝置進 ROM download mode：

```text
Failed to connect to ESP32-C6: No serial data received.
```

改用 ESP USB-JTAG/OpenOCD 寫入成功，application 區塊 verify OK。可重複使用的命令：

```bash
C:/Users/virgil/.espressif/tools/openocd-esp32/v0.12.0-esp32-20260703/openocd-esp32/bin/openocd.exe \
  -s C:/Users/virgil/.espressif/tools/openocd-esp32/v0.12.0-esp32-20260703/openocd-esp32/share/openocd/scripts \
  -f board/esp32c6-builtin.cfg \
  -c 'program_esp build/bootloader/bootloader.bin 0x0 verify' \
  -c 'program_esp build/partition_table/partition-table.bin 0x8000 verify' \
  -c 'program_esp build/ESP32-BLE5-NSController-Emulator.bin 0x10000 verify reset exit'
```

NVS partition 沒有被重寫，因此首次配對資料保留。

### USB transport 證據

EasyCon hello frame：

```text
TX: A5 A5 81
RX: 80
```

持續開啟 COM5 後，裝置每次都正確回覆 `80`。這證明 EasyCon parser、USB Serial/JTAG RX/TX 與 protocol dispatcher 都在執行。

EasyCon HID frame 是 7 raw bytes 經 7-bit packing 變成 8 bytes，最後一個 byte 設 bit 7。Neutral frame：

```text
00 00 01 08 04 02 01 80
```

測試依序送出：

1. Neutral
2. A
3. D-pad Up / Right / Down / Left
4. Left stick X− / X+ / Y− / Y+
5. Right stick X− / X+ / Y− / Y+
6. Neutral

Host 端 sequence 完整送出，EasyCon hello 成功；使用者看到控制器測試畫面有反應，並表示看起來應該正常，但沒有逐項確認每個 axis label，所以精確軸向仍應在正式 gateway 完成後重測。

第二次重播期間測試畫面自行退出。送出的 button mask 只有 EasyCon 的 A（bit 2）；**沒有送 Home**。中斷後已立即送出 Neutral，沒有留下卡鍵狀態。

## 睡眠、喚醒與重連結果

使用者先讓 Switch 2 進入睡眠，再透過 USB-JTAG reset ESP32-C6：

```bash
openocd ... \
  -c 'telnet port disabled' \
  -c 'tcl port disabled' \
  -c 'gdb port disabled' \
  -f board/esp32c6-builtin.cfg \
  -c 'init' -c 'reset run' -c 'shutdown'
```

未套 patch 的實際結果：

- Switch 2 被喚醒：成功
- 自動恢復可用 HID 輸入：失敗
- Host 成功送出四個獨立 A press/release，最後 Neutral，但 Switch 2 沒有反應

接著對 ESP-IDF 6.1 `libble_app.a` 做受控 A/B 測試：

- 原始 library SHA-256：`88ad3871130c3424c75c864f1d7ec00c8d990dc60ae965626234b345dbb401b2`
- `ble_ll_conn.c.o` SHA-256：`6c191e7025fddeda3cd527095004711266a6f9c59cac4880eafd2ba1286f77d8`
- 原始指令 `addi a5,a4,-6`：整個 object 恰好一處，offset `0x4052`
- 該指令位於 `r_ble_ll_conn_slave_start+0x184`
- 修改後指令：`addi a5,a4,-4`
- Patched library SHA-256：`e0d52f889a74a96333291ea81a95975c0f280e734dba60bbaa2179532ba87723`
- 最終 ELF disassembly 在 `r_ble_ll_conn_slave_start` 確認包含 `ffc70793 addi a5,a4,-4`

用 patched library 完整 clean build 後，立即把共用 IDF library 還原；還原後 SHA-256 再次是 `88ad3871130c3424c75c864f1d7ec00c8d990dc60ae965626234b345dbb401b2`。已建出的 ELF/bin 仍包含 patched controller code。

Patched firmware 的兩次實機結果：

1. Switch 2 已醒著時重新燒錄及重啟：自動重連成功，四次 A 全部收到。
2. Switch 2 睡眠後 reset ESP32-C6：主機被喚醒、自動重連成功，四次 A 全部收到。

每次 host 測試都先取得 EasyCon `hello=80`，每個 A 都有獨立 press/release，最後明確送 Neutral。

## 未套 patch 的失敗當下 JTAG/GDB 證據

以 OpenOCD + `riscv32-esp-elf-gdb` 讀取 live firmware：

```text
g_device_status                  = DEV_READY
g_adv_opcode                     = 0x81
g_protocol_inst                  = &easycon_protocol_instance
g_transport_protocol_task        = non-null
g_hid_controller.task_handle     = non-null
pairing LTK                      = present
pairing peer address             = present
g_ltk_sec->ltk_present           = 1
g_ltk_sec->authenticated         = 1
g_ltk_sec->sc                    = 1
subscription handle              = 26 (0x001A entry)
subscription conn_handle         = 0
subscription notify_enabled      = true
controller report body size      = 63
gatt_notify() return code         = 0
```

FreeRTOS thread list也顯示：

- `nimble_host`
- `ble_ll_task`
- `controller_task`
- `usj_rx_tp`
- `esp_timer`

全部存在。

這個狀態很重要：ESP32 端不是單純「沒有建立連線」。它保留/重建了 connection handle、notification subscription 與 controller task，`gatt_notify()` 甚至回傳成功，但 Switch 2 沒有接收/採用 HID input。上游 `g_device_status` 在 disconnect handler 中也沒有退回 advertising/disconnected state，所以 `DEV_READY` 本身不能當成有效重連證據。

## A/B 測試判定

本次可控制的唯一 radio 差異是 `r_ble_ll_conn_slave_start` 的最小 interval 檢查由 6 units（7.5 ms）改為 4 units（5 ms）。同一塊板、同一份配對 NVS、同一套 application source 與同一個四次 A 測試，在未套 patch 時失敗、套 patch 後連續通過 reboot reconnect 與 sleep/wake reconnect。

因此 5 ms controller interval 是這次 reconnect report failure 的實證根因，不只是推測。初次配對能在未套 patch 的 IDF 6.1 上完成，不能代表重新連線路徑支援 Switch 2 要求的 5 ms interval。

上游仍有幾個應在正式移植時修正的狀態機問題：

1. `main/src/gap.c` 的 disconnect handler 沒有清除 subscription map，也沒有立即退出 `DEV_READY`。
2. `controller_task` 只依賴 `DEV_READY` 與 cached subscription，兩者都可能是 stale state。
3. 正式 gateway 應保存獨立 console connection handle，disconnect 時設為 `BLE_HS_CONN_HANDLE_NONE`，並等待新連線的 encryption 與 CCCD subscription 完成後才啟動 report task。

這些是 hardening 工作，但不是本次 A/B failure 的主要來源；5 ms patch 已讓未改 application source 的上游基準完整通過。

## 下一步建議

### 第一階段：上游基準重連（已完成）

上游基準已通過：

- 初次 Switch 2 配對
- USB Serial/JTAG EasyCon 輸入
- 重新啟動後自動重連與四次 A
- sleep → wake → reconnect → 四次 A

後續 production build 必須可重現同一個受控 patch。不要直接依賴目前已還原的共用 IDF library；應使用隔離 IDF tree，或在 build pipeline 中先驗證原始 archive/object hash、要求唯一指令位置、build 後還原，再檢查最終 ELF disassembly。

### 第二階段：通過基準後才移植 gateway

目前專案根目錄仍是原本 ESP32 Classic Bluetooth gateway，尚未完成 ESP32-C6 cutover。要做 clean cut：

1. Target 改為 `esp32c6`、8 MB、NimBLE peripheral、extended advertising。
2. 移除 Bluedroid、Classic HID、BTDM dual-mode 與 dual-core pinning。
3. 保留已完成的 PlayGate v2 controller core：17-byte payload、sequence、300 ms lease、100 ms neutral、status、COBS/CRC、UDP。
4. 新增 `switch2_controller` component：Nintendo GATT 必須先註冊並保持固定 handles。
5. 使用固定 63-byte report buffer、100 Hz absolute deadlines、無 hot-path heap。
6. USB Serial/JTAG 成為 C6 預設 Serial ingress。
7. PlayGate auxiliary BLE service 必須在 Nintendo services 後註冊，使用第二 advertising instance/connection。
8. 對移植自上游的程式與 tables 保留 MIT notice 和 pinned commit provenance。

### 第三階段：正式驗證

最低通過條件：

- 初次配對
- A/B/X/Y、D-pad、shoulders、Home/Capture
- 雙搖桿中心/端點/Y 軸方向
- sleep/wake 自動重連後仍能送 report
- Serial/UDP/BLE 的 lease、timeout-neutral、sequence wrap、superseded
- 100 Hz reports、無 burst catch-up
- Wi-Fi + Nintendo BLE + auxiliary BLE coexistence soak

## 板子狀態（EasyCon 基準驗證階段；該階段韌體已被下方 gateway 韌體取代）

當時板子上燒錄的是：

- 上游 commit `0ea0c62`
- ESP-IDF 6.1 clean build
- ESP32-C6 target
- EasyCon protocol enabled
- USB Serial/JTAG transport
- Application 內含已驗證的 5 ms controller interval patch
- 已保存 Switch 2 pairing NVS
- Switch 2 初次配對成功
- reboot 自動重連成功
- sleep/wake 自動重連成功
- 喚醒後四次 A 全部收到
- 最後送入狀態是 Neutral

共用 ESP-IDF `libble_app.a` 已還原為原始 SHA-256。OpenOCD/GDB service 已停止，不會繼續 halt CPU 或占用 JTAG port。

## 傳輸層與輸入 ingress

三條 ingress（Serial、Wi-Fi/UDP、BLE）共用同一組線路格式與仲裁規則，差別只在底層承載。Wi-Fi 另提供一個可手動操作的控制網頁。

### 共用線路格式

#### 輸入 payload（17 bytes）

| offset | 型別 | 內容 |
|---|---|---|
| 0 | u8 | version = `2` |
| 1–4 | u32 LE | buttons，遮罩 `0x0003FFFF` |
| 5–6 | i16 LE | left_x |
| 7–8 | i16 LE | left_y |
| 9–10 | i16 LE | right_x |
| 11–12 | i16 LE | right_y |
| 13–16 | u32 LE | sequence（每 frame +1，u32 環繞）|

軸向範圍 `-32767..32767`（`PLAYGATE_AXIS_MIN/MAX`），Y 軸負值為上。

#### 狀態 payload（20 bytes，`GATEWAY_STATUS_VERSION = 1`）

| offset | 型別 | 內容 |
|---|---|---|
| 0 | u8 | version = `1` |
| 1 | u8 | hid_state：0 idle、1 discoverable、2 connecting、3 connected |
| 2 | u8 | active_source：0 none、1 serial、2 udp、3 ble |
| 3 | u8 | flags：bit0 wifi_connected、bit1 ble_connected、bit2 timeout_neutral |
| 4–7 | u32 LE | last accepted sequence |
| 8–9 | u16 LE | accepted_total（飽和）|
| 10–11 | u16 LE | invalid（飽和）|
| 12–13 | u16 LE | stale（飽和）|
| 14–15 | u16 LE | conflicts（飽和）|
| 16–17 | u16 LE | superseded（飽和）|
| 18–19 | u16 LE | last_receive_to_submit_us（飽和）|

計數器為 u16 飽和，因此長時間 soak 後會停在 65535。`last_receive_to_submit_us` 只有在取樣與送出之間對得上同一 generation 時才更新，idle 時可能停在 65535。

#### 按鍵位元（`controller_core.h`）

A=0、B=1、X=2、Y=3、L=4、R=5、ZL=6、ZR=7、PLUS=8、MINUS=9、HOME=10、CAPTURE=11、L_STICK=12、R_STICK=13、DPAD_UP=14、DPAD_DOWN=15、DPAD_LEFT=16、DPAD_RIGHT=17。

#### 來源仲裁（`controller_core.c`）

- 每個來源各自持有 epoch（最後 sequence + 最後出現時間）。
- `CONFIG_PLAYGATE_SOURCE_LEASE_MS`（預設 300）：靜默超過此值即釋放 epoch；接手來源不會被誤判為 stale。
- 同一 epoch 內 sequence 非更新（以 signed diff 判斷 u32 環繞）→ `stale`。
- 目前 owner 的 lease 未過期且來源不同 → `conflicts`，該 frame 不套用。
- `CONFIG_PLAYGATE_INPUT_TIMEOUT_MS`（預設 100）：owner 超過此時間沒有輸入 → `timeout_neutral`，HID 回中立但 `active_source` 保留。
- 來源斷線（BLE disconnect、serial 停止）呼叫 `input_core_release()`，會清空該來源 epoch 並在它是 owner 時立即回中立。
- `superseded`：已接受但尚未被 HID 回報的 generation 被新輸入取代的次數。

### Serial ingress（USB Serial/JTAG 與 UART0）

檔案：`components/input_transports/serial_input.c`、`serial_parser.c`。

- Backend 由 `PLAYGATE_SERIAL_BACKEND` 選擇：ESP32-C6 預設 **USB Serial/JTAG**，另一個選項是 **UART0**（`PLAYGATE_UART_BAUD`，預設 921600）。USB Serial/JTAG 由硬體決定速率，host 端的 baud 設定會被忽略。
- 線路格式（`serial_parser.c`）：解碼後 frame = `[version=0x01][type][len][payload][crc_lo][crc_hi]`，整段做 COBS encode 後尾端補一個 `0x00` delimiter。
  - `type`：input = `0x01`（payload 必須 17 bytes）、status = `0x81`（payload 必須 20 bytes）。
  - CRC = CRC-16/CCITT-FALSE（poly `0x1021`、init `0xFFFF`），計算範圍為前 `3 + len` bytes，little-endian 存放。
  - 尺寸上限：payload 20、decoded 25、encoded 27 bytes。
  - 解析端以 `0x00` 切 frame，逐項驗證 version / type-length 組合 / 總長度 / CRC，任一不符即靜默丟棄（不影響後續 frame）。
- 接收：`serial_input_task`（priority 9、stack 3072、IO chunk 128）持續讀 backend，通過驗證的 input frame 以 `input_core_submit(INPUT_SOURCE_SERIAL, payload, 17, esp_timer_get_time())` 送入。RX/TX ring buffer 各 1024 bytes。
- 送出：狀態發布器（見下）每秒一次、以及狀態變更時呼叫 `serial_input_publish_status()`，把 20-byte 狀態包成 `0x81` frame 寫回同一 backend。
- `CONFIG_ESP_CONSOLE_NONE=y`，因此 USB Serial/JTAG 上只有 framed 資料，沒有 log 或 console 輸出。
- 指令測試：`python tools/gateway_client.py serial COM5 --press A --hold 2 --status`。

### Wi-Fi ingress（STA / SoftAP）與 UDP

檔案：`components/input_transports/udp_input.c`。

- 模式選擇：`CONFIG_PLAYGATE_WIFI_SSID` **非空 → STA**；**空 → SoftAP fallback**（見下方變更說明）。兩種模式都使用 `WIFI_STORAGE_RAM`（不寫 NVS）並呼叫 `esp_wifi_set_ps(WIFI_PS_NONE)` 關閉省電。
- STA：連上後由 `IP_EVENT_STA_GOT_IP` 設 `wifi_connected`；`WIFI_EVENT_STA_DISCONNECTED` 清除並要求重建 socket，同時自動重連。
- SoftAP：`esp_netif_create_default_wifi_ap()` + `WIFI_MODE_AP`，SSID = `"<PLAYGATE_AP_SSID_PREFIX>-XXXX"`（XXXX 為 SoftAP MAC 後兩 bytes），channel 1、最多 4 個 client。`PLAYGATE_AP_PASSWORD` ≥8 字元 → `WIFI_AUTH_WPA2_PSK`，否則開放網路。`WIFI_EVENT_AP_START` / `WIFI_EVENT_AP_STOP` 分別設定與清除 `wifi_connected`。IP 由 IDF 的 default netif 提供（預設 `192.168.4.1`）。
- UDP：listen `0.0.0.0:CONFIG_PLAYGATE_UDP_PORT`（預設 26760），socket 為 non-blocking，`wifi_connected` 為 false 時不建立、斷線時關閉並在恢復後重建。每包處理：
  - 長度 17 → `input_core_submit(INPUT_SOURCE_UDP, ...)`。
  - 長度 1 且 byte 為 `0xF0`（`UDP_STATUS_PROBE`）→ 回覆 20-byte 狀態給來源位址。
  - 其他長度 → 丟棄。
- 指令測試：`python tools/gateway_client.py udp 192.168.4.1 --press B --hold 2 --status`。

### Wi-Fi 控制網頁（HTTP + WebSocket）

檔案：新 component `components/web_console/`、網頁 `tools/gamepad.html`。

- `web_console_start()` 在 `input_transports_start_wifi_udp()` 之後啟動，`esp_http_server` 綁 `0.0.0.0:80`，`max_open_sockets = 4`、`lru_purge_enable`、wildcard URI match。不需等待 IP：介面取得 IP 後才有請求進來。
- `GET /`：以 `EMBED_FILES` 內嵌的 `tools/gamepad.html`，`text/html` + `Cache-Control: no-store`。
- `GET /ws`（`is_websocket`）：binary frame 長度 17 → `input_core_submit(INPUT_SOURCE_UDP, ...)`；長度 1 且為 `0xF0` → 狀態查詢；其他長度或非 binary 一律忽略。輸入與查詢都回覆 20-byte 狀態給同一個連線。網頁端不需要 fd 追蹤或 server push，閒置時每秒送一次 `0xF0`。
- 網頁刻意與 UDP 共用 `INPUT_SOURCE_UDP`（屬「網路輸入」），因此 `INPUT_SOURCE_COUNT`、狀態編碼與既有 39 項單元測試都不受影響；兩者共用 sequence epoch，但 300 ms lease 會自動重置。
- `tools/gamepad.html` 為單一檔案、無外部資源、無 build step，放 `tools/`（與 `gateway_client.py` 同層）可直接開檔，同時被 `EMBED_FILES` 嵌進韌體。
  - 以 `location.protocol` 選預設 transport：`file:` → USB（Web Serial），其他 → Wi-Fi（WebSocket），兩者可切換。
  - Wi-Fi：`new WebSocket("ws://" + location.host + "/ws")`，`binaryType = "arraybuffer"`；每個 tick 送 17 raw bytes，收到 20 bytes 即更新狀態與瀏覽器端往返時間；無按鍵變更時每秒送一個 `0xF0`；斷線自動重連，重連前送一次 neutral。
  - USB：`navigator.serial.requestPort()` → `open({ baudRate })`（USB Serial/JTAG 忽略 baud，API 要求提供）。寫入自行組框，逐位元對齊 `serial_parser.c`；讀取端以 `0x00` 切 frame、COBS decode、驗 version/type/len/CRC 後解析 20-byte 狀態。
  - 支援 **Web Gamepad API**：輪詢 `navigator.getGamepads()`，依 W3C standard mapping 對應 PlayGate 按鈕（index 12–15 為 D-pad、16/17 為 Guide/Share，因此不是 identity mapping），類比軸 ±1 含 0.08 deadzone 後乘 32767，多支手把時出現下拉選單。
  - 版面：`body` 不設 `touch-action`（否則手指無法捲動頁面），只有 `.disc` 設 `touch-action: none`；按鈕不設，讓從按鈕上開始的滑動仍可捲動。手機橫置（`orientation: landscape and max-height: 560px`）時 `#play-area` 變四欄，左右搖桿分貼兩側邊緣。
  - 輸入來源仲裁：`touch`（螢幕控件）與 `gamepad`（實體手把）各自保存狀態，手把有輸入時獨佔，回到中立時交還螢幕控件，兩者都不會留下卡住的按鍵。
  - 指標放開、按鍵放開、切換 transport、`beforeunload` 都會送 neutral frame；裝置端 100 ms timeout-neutral 是這一層之外的保險。
  - Web Serial 需要 secure context；Chrome 將 `file://` 視為 potentially trustworthy，若 `navigator.serial` 不存在，改用 repo root 的 `python -m http.server 8000` 開 `http://localhost:8000/tools/gamepad.html`。

### BLE ingress（既有，輔助通路）

檔案：`components/switch2_controller/switch2_gatt.c`。PlayGate auxiliary service（`5f5c9c2e-3b4d-4f93-9b7a-4c70a9d7f1e1`）提供：

- input characteristic（`...f1e2`）：`WRITE | WRITE_NO_RSP`，長度必須正好 17 bytes，否則回 `BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN`；通過後 `input_core_submit(INPUT_SOURCE_BLE, ...)`。
- status characteristic（`...f1e3`）：`READ | NOTIFY`，20 bytes。
- Nintendo 服務先註冊並固定 handles（input `0x0035`、status `0x0037`，開機時驗證），PlayGate 服務註冊在其後。BLE client 斷線時 `input_core_release(INPUT_SOURCE_BLE)`。

### 狀態發布器

`input_transports_start_status_publisher()` 建立 `gateway_status` task（priority 5、stack 3072）：每秒發布一次，另外在 `gateway_status_set_*` 實際改變值時立即發布（`gateway_status_set_changed_callback`）。每次發布都會同時送往 Serial（`0x81` frame）與 BLE status characteristic（notify），Web/UDP 則在被查詢或收到輸入時才回覆。

### 本節變更檔案

- `components/input_transports/udp_input.c`：移除 SSID 為空時靜默 `return ESP_OK` 的陷阱，改為 SoftAP fallback（`s_ap_mode` 決定 netif／mode／config／`WIFI_IF_*`，並新增 `WIFI_EVENT_AP_START/STOP` 分支與 `build_ap_ssid()`）。STA 分支行為與原本相同。
- 新 component `components/web_console/`：HTTP + WebSocket 控制頁（`web_console.c`、`web_console.h`、`CMakeLists.txt` 的 `EMBED_FILES`）。`PLAYGATE_ENABLE_WEB` 關閉時編譯為 `ESP_ERR_NOT_SUPPORTED` stub。
- `tools/gamepad.html`：新增，雙 transport 手動控制頁。
- `main/Kconfig.projbuild`：`PLAYGATE_ENABLE_WEB`（`depends on PLAYGATE_ENABLE_UDP && HTTPD_WS_SUPPORT`）、`PLAYGATE_AP_SSID_PREFIX`、`PLAYGATE_AP_PASSWORD`。
- `sdkconfig.defaults`：`CONFIG_HTTPD_WS_SUPPORT=y` 與上述三者。
- `main/app_main.c`、`main/CMakeLists.txt`：啟動 web console 並加入 `web_console` 依賴。

### 已完成實機驗證

- `idf.py build` 成功，仍輸出 `verified ... r_ble_ll_conn_slave_start contains exactly one addi a5,a4,-4 and no addi a5,a4,-6`。
- `test_apps/unit`：**39 Tests 0 Failures 0 Ignored**（`controller_core` 未修改）。
- AP fallback：裝置回報的 20-byte status 為 `wifi_connected: true`（即 `WIFI_EVENT_AP_START` 已到達）。修改前此值恆為 false。
- Serial ingress 未回歸：`gateway_client.py serial COM5 --press A --hold 2 --status` → `accepted_total=218`、`active_source_name=serial`、`invalid=0`。
- `tools/gamepad.html` 的 encoder/decoder 以真實裝置驗證：直接取用頁面內的 `buildInputFrame`/`serialEncode`/`decodeSerialPacket`/`decodeStatus` 對 COM5 送 A，裝置 `accepted 0→1`、`active_source=serial`、`invalid=0`；1.5 s 連續輸入時 `last_receive_to_submit_us` 為 5259 µs（idle 時可能出現 65535 飽和值，屬既有 u16 飽和行為）。
- Chrome 實際載入 `tools/gamepad.html`（`file:`）：`navigator.serial` 可用、選到 USB Serial/JTAG 並開啟、狀態面板即時更新；A / D-pad / 左搖桿分別讓 `accepted` 遞增（0→19→82→876），`invalid=0`，延遲顯示 5.2–6.8 ms。指標放開與按鍵放開都會回到 neutral。
- 版面（Chrome device metrics）：844×390 橫置時左搖桿左緣 6 px、右搖桿右緣 838 px、無面板重疊、無水平溢出，pad 區底部 343 px < 390 px；390×844 直式與 1280×800 桌機皆無水平溢出。觸控捲動以 `Input.dispatchTouchEvent` 實測：一般內容可捲動約 600 px，而拖曳搖桿只產生 13 px 位移且正確輸出 `L 0, -21650` → 放開回到 `L 0, 0`。
- Web Gamepad API：以注入的虛擬手把（standard mapping）驗證 18 個按鈕 index 全部對應正確、類比 trigger `value > 0.5` 會觸發、0.03 落在 deadzone 內讀為 0、0.5 → −16383、0.75 → 24575、超出 ±1 會 clamp 到 ±32767；手把持有時 `input source = gamepad` 且螢幕按鈕同時按下不會搶走，手把回到中立後交還螢幕控件；手把斷線後 `none connected` 且無殘留按鍵。
- 手把路徑到裝置：把 `tools/gamepad.html` 內的 mapping table 與 `buildInputFrame` 抽出，以「D-pad up + B + 左搖桿左上」的手把狀態組框後經 COM5 送出，裝置 `accepted 0→1`、`active_source=serial`、`invalid=0`、`last_receive_to_submit_us=5820`。
- 重新燒錄後 `gateway_client.py serial COM5 --press A --hold 1 --status` → `accepted_total=122`、`active_source_name=serial`、`invalid=0`、`wifi_connected=true`；內嵌 HTML 大小與 `tools/gamepad.html` 完全一致（37,687 bytes）。

### 尚未實機驗證

使用者已實機確認 Wi-Fi 路徑成功（SoftAP 連線與 `http://192.168.4.1/` 網頁），因此以下由使用者自行量測，文件未記錄數值：

- SoftAP SSID 廣播內容
- `gateway_client.py udp 192.168.4.1 --press B --hold 2 --status`
- Serial 與 UDP 同時輸入時的 `conflicts` 與 300 ms 接手
- AP + Nintendo BLE 共存 soak
- 實體手把（真實 Gamepad API 裝置）在瀏覽器中的行為；本文件記錄的是以注入虛擬手把驗證的 mapping 與仲裁邏輯

### Build size

Application 由 665,008 bytes 增至 1,297,552 bytes（app partition 3 MB，剩 59%）。原因不是新功能本身（`libweb_console.a` 含約 37 KB HTML 約 38 KB），而是原本 SSID 為空時的提早 `return` 讓 GCC 把整個 Wi-Fi 初始化路徑視為不可達而刪除，`net80211`/`pp`/`wpa_supplicant`/`phy` 因此從未被連結；AP fallback 讓該路徑重新可達。

## 專案狀態

此 workspace 內現有的 `controller_core`、`input_transports`、`switch_hid`、unit tests 和 `tools/gateway_client.py` 是原始 ESP32 Classic Bluetooth 設計。它們是後續 C6 migration 的來源，但 `switch_hid`/Classic protocol 不能直接在 ESP32-C6 使用。

以上兩段描述的是 EasyCon 基準驗證階段；該階段沒有修改 production source files，只更新本接手文件。Serial／Wi-Fi ingress 與 web console 的 production 變更見「傳輸層與輸入 ingress」一節。
