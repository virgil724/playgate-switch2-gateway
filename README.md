# PlayGate Switch 2 Gateway

ESP32-C6 firmware that presents itself to a Nintendo Switch 2 as a **Pro Controller 2**, and lets a host drive it over USB serial, Wi-Fi (UDP or WebSocket), or Bluetooth LE.

The point of the project is the *gateway* shape: one input core with a single ownership/lease model, four interchangeable ingresses feeding it, and a host-side status stream that reports exactly what the firmware did with every frame.

```
USB serial ─┐
UDP ────────┤
WebSocket ──┼─► input_core ──► Switch 2 HID report (100 Hz)
BLE ────────┘   (lease/stale/   over Nintendo GATT
                 conflict)      │
                                └─► 20-byte status (serial, BLE notify,
                                    UDP reply, WebSocket reply)
```

## Status

Verified on real hardware (ESP32-C6FH8):

- Nintendo link: first pairing, automatic reconnect after reboot, reconnect after console sleep/wake, and HID input accepted after wake.
- Serial ingress: framed input accepted, `invalid = 0`.
- Wi-Fi SoftAP fallback, and the embedded web control page loaded from a phone browser.
- Web page over USB (Web Serial) in Chrome: buttons, D-pad and sticks move `accepted` with `invalid = 0`, ~5–7 ms device-reported receive→submit.
- Unit tests: **39 tests, 0 failures** (`test_apps/unit`).

Implemented but not yet verified end to end on hardware in this repository's current state:

- Simultaneous serial + UDP input: expected `conflicts` increment and 300 ms handover.
- BLE ingress (`PLAYGATE` GATT service) against a real host; it is covered by unit tests only.
- Wi-Fi + Nintendo BLE coexistence soak.
- A physical gamepad through the browser's Gamepad API (verified with an injected virtual pad only).

See [`SWITCH2_C6_HANDOFF.md`](SWITCH2_C6_HANDOFF.md) for the full bring-up log, evidence and protocol notes (written in Chinese).

## Hardware

| | |
|---|---|
| MCU | ESP32-C6 (target `esp32c6` only — the build hard-fails otherwise) |
| Flash | 8 MB, custom partition table: NVS + phy_init + one 3 MB factory app |
| Host link | USB Serial/JTAG on the ESP32-C6's native USB |
| Toolchain | ESP-IDF **v6.1.0 exactly** (`CMakeLists.txt` rejects any other version) |

Single-core FreeRTOS build. Wi-Fi, NimBLE and the HID report path all share core 0.

## Wire formats

Both the UDP and WebSocket ingresses carry the raw payloads below with no framing. The serial and BLE ingresses carry the same payloads inside their own transport framing.

### Input payload — 17 bytes

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | version = `2` |
| 1–4 | u32 LE | buttons, mask `0x0003FFFF` |
| 5–6 | i16 LE | left_x |
| 7–8 | i16 LE | left_y |
| 9–10 | i16 LE | right_x |
| 11–12 | i16 LE | right_y |
| 13–16 | u32 LE | sequence, incremented per frame (wraps) |

Axes are `-32767..32767`; **negative Y is up**.

### Status payload — 20 bytes

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | version = `1` |
| 1 | u8 | hid_state: 0 idle, 1 discoverable, 2 connecting, 3 connected |
| 2 | u8 | active_source: 0 none, 1 serial, 2 udp, 3 ble |
| 3 | u8 | flags: bit0 wifi_connected, bit1 ble_connected, bit2 timeout_neutral |
| 4–7 | u32 LE | last accepted sequence |
| 8–9 | u16 LE | accepted_total |
| 10–11 | u16 LE | invalid |
| 12–13 | u16 LE | stale |
| 14–15 | u16 LE | conflicts |
| 16–17 | u16 LE | superseded |
| 18–19 | u16 LE | last_receive_to_submit_us |

The counters are saturating `u16`, so long soaks stop at 65535. `last_receive_to_submit_us` is only updated when the sampled generation and the reported generation match, so it can hold a stale value while idle.

### Button bits

`A`=0, `B`=1, `X`=2, `Y`=3, `L`=4, `R`=5, `ZL`=6, `ZR`=7, `PLUS`=8, `MINUS`=9, `HOME`=10, `CAPTURE`=11, `L_STICK`=12, `R_STICK`=13, `DPAD_UP`=14, `DPAD_DOWN`=15, `DPAD_LEFT`=16, `DPAD_RIGHT`=17.

### Source arbitration

- Every source keeps its own epoch (last sequence + last-seen time).
- `PLAYGATE_SOURCE_LEASE_MS` (default 300): after that much silence the epoch is dropped, so a new source taking over is not misread as stale.
- A sequence that is not newer (signed comparison, so wrap-around works) counts as `stale`.
- Input from a different source while the current owner's lease is live counts as `conflicts`; the frame is dropped.
- `PLAYGATE_INPUT_TIMEOUT_MS` (default 100): an owner that goes quiet neutralizes the report (`timeout_neutral`) while `active_source` is retained.
- Disconnects call `input_core_release()`, clearing that source's epoch and neutralizing immediately.

## Transports

| Ingress | Carrier | Input | Status |
|---|---|---|---|
| Serial | USB Serial/JTAG (default) or UART0 | `version/type/len/payload/CRC16`, COBS, `0x00` delimiter | pushed 1 Hz and on every state change |
| UDP | Wi-Fi STA or SoftAP, port 26760 | raw 17-byte datagram | reply to a 1-byte `0xF0` probe |
| WebSocket | `esp_http_server` on `:80`, path `/ws` | raw 17-byte binary frame | reply to a 1-byte `0xF0` probe, or after every input frame |
| BLE | `PLAYGATE` GATT service | 17-byte write to `…f1e2` | `READ`/`NOTIFY` on `…f1e3` |

Serial framing is `[version=0x01][type][len][payload][crc_lo][crc_hi]`, COBS-encoded, terminated by a `0x00`. `type` is `0x01` for input (17 bytes) and `0x81` for status (20 bytes). The CRC is CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`) over the first `3 + len` bytes, little-endian. Malformed frames are dropped silently without disturbing the stream. Application console output is disabled (`CONFIG_ESP_CONSOLE_NONE`) so the USB link carries only framed data.

The WebSocket ingress deliberately reuses `INPUT_SOURCE_UDP`: both are "network input", which keeps the source enum, the status encoding and the existing tests unchanged. The 300 ms lease separates the two if you switch between them.

BLE advertising for the PlayGate service starts only once the Nintendo link is ready. Nintendo services register first and keep fixed attribute handles (input `0x0035`, status `0x0037`), which the firmware verifies after registration.

## Web control page

A single self-contained HTML file with no external assets, served by the device and also usable straight off disk:

- **Wi-Fi** — browse to the device's address. In SoftAP mode that is the ESP-IDF default `192.168.4.1`; read the actual address from `esp_netif_get_ip_info()` if you have changed it. The page talks to `/ws`.
- **USB** — open `tools/gamepad.html` directly in Chrome. It defaults to Web Serial when `location.protocol` is `file:`.

Both modes are switchable in the page. Features: draggable sticks, D-pad, all 18 buttons, a keyboard map, a live status panel, and two latency readouts (the device's own `last_receive_to_submit_us` plus the browser's round trip). Latency judgement is left to the user; the page only displays numbers.

It also supports the **Web Gamepad API**: a physical pad recognised by the browser is translated from the W3C standard mapping to PlayGate bits and drives the gateway directly, taking precedence while deflected and handing back to the on-screen controls when it returns to neutral. Axes get a 0.08 deadzone.

Web Serial needs a secure context. Chrome treats `file://` as potentially trustworthy; if `navigator.serial` is missing, serve the repo over localhost instead:

```
python -m http.server 8000
# then open http://localhost:8000/tools/gamepad.html
```

## Build

```bat
call C:\Users\virgil\.espressif\frameworks\esp-idf-v6.1\export.bat
idf.py build
idf.py -p COM5 flash
```

`idf.py monitor` is not useful on the gateway build: the UART console is disabled so the USB Serial/JTAG link stays free for framed data.

Two build-system behaviours are worth knowing:

- **A BLE controller patch is applied at build time.** NimBLE does not hold a 5 ms connection interval, and without it the Switch 2 wakes and connects but never accepts HID reports. `cmake/switch2_ble_patch.cmake` builds a patched copy of the verified ESP-IDF controller archive into `build/switch2_ble/libble_app.a` and repoints the imported target at it — **the shared ESP-IDF installation is not modified**. A post-link step then asserts the patched instruction is present in the ELF:
  `verified … r_ble_ll_conn_slave_start contains exactly one addi a5,a4,-4 and no addi a5,a4,-6`.
  If the ESP-IDF version or archive differs, the build fails rather than producing a subtly wrong binary.
- Queued ATT writes are compiled out (`MYNEWT_VAL_BLE_ATT_SVR_QUEUED_WRITE=0`) so PlayGate prepared writes are rejected before application code can accept them.

Flashing writes the bootloader, partition table and app; existing NVS pairing data is preserved by an app-only flash.

## Unit tests

```bat
cd test_apps\unit
idf.py build
idf.py -p COM5 flash
```

Connect to the port at 115200, press Enter for the list and `*` to run everything. Expected: `39 Tests 0 Failures 0 Ignored`. The suite covers the input core, the serial parser, and the Switch 2 protocol, pairing, command and advertising logic.

## Host tool

`tools/gateway_client.py` sends input and decodes status over any transport. UDP needs no third-party package; serial needs `pyserial`, BLE needs `bleak` (`tools/requirements.txt`).

```bash
python tools/gateway_client.py udp 192.168.4.1 --press B --hold 2 --status
python tools/gateway_client.py serial COM5 --press A --hold 2 --status
python tools/gateway_client.py ble --name "PlayGate Input" --press A --status
python tools/gateway_client.py udp 192.168.4.1 --status-only
python tools/gateway_client.py serial COM5 --soak 60 --press A,B
```

`--press` accepts `A,B,X,Y,L,R,ZL,ZR,PLUS,MINUS,HOME,CAPTURE,LSTICK,RSTICK,UP,DOWN,LEFT,RIGHT`; `--lx/--ly/--rx/--ry` set axes; `--latency` and `--soak` report status latency percentiles. Every run ends by sending neutral.

## Configuration

`idf.py menuconfig` → *PlayGate Switch Gateway*:

| Option | Default | Notes |
|---|---|---|
| `PLAYGATE_ENABLE_UDP` | y | Wi-Fi radio and the UDP ingress |
| `PLAYGATE_ENABLE_WEB` | y | HTTP page + WebSocket; needs `HTTPD_WS_SUPPORT` (set in `sdkconfig.defaults`) |
| `PLAYGATE_ENABLE_SERIAL` | y | Serial ingress |
| `PLAYGATE_SERIAL_BACKEND` | USB Serial/JTAG | or UART0 |
| `PLAYGATE_UDP_PORT` | 26760 | |
| `PLAYGATE_WIFI_SSID` / `_PASSWORD` | empty | **empty selects SoftAP fallback** |
| `PLAYGATE_AP_SSID_PREFIX` | `PlayGate` | AP SSID becomes `<prefix>-XXXX` from the SoftAP MAC |
| `PLAYGATE_AP_PASSWORD` | `playgate` | under 8 characters makes the AP open |
| `PLAYGATE_INPUT_TIMEOUT_MS` | 100 | neutralization timeout |
| `PLAYGATE_SOURCE_LEASE_MS` | 300 | source ownership lease |
| `PLAYGATE_REPORT_HZ` | 100 | HID report rate |

With no Wi-Fi credentials the device starts a SoftAP instead of silently disabling the transport. Change `PLAYGATE_AP_PASSWORD` before deploying anywhere you do not control.

## Repository layout

```
components/controller_core/     input core, source arbitration, status encoding
components/input_transports/    serial (USB Serial/JTAG, UART) and Wi-Fi/UDP ingress
components/switch2_controller/  NimBLE lifecycle, Nintendo GATT, pairing, protocol
components/web_console/         esp_http_server, serves the embedded page
main/                           app_main, Kconfig options
tools/gamepad.html              web control page (also embedded into the firmware)
tools/gateway_client.py         host CLI for all transports
tools/prepare_switch2_ble_lib.py  build-time BLE controller archive patcher
test_apps/unit/                 target-side unit tests
cmake/switch2_ble_patch.cmake   wires the patched archive into the build
```

## Provenance

The Nintendo Switch 2 controller protocol implementation derives from published research and from [`zhantss/ESP32-BLE5-NSController-Emulator`](https://github.com/zhantss/ESP32-BLE5-NSController-Emulator) (pinned commit `0ea0c62`, MIT). The upstream license text is bundled at `components/switch2_controller/ESP32-BLE5-NSController-Emulator_LICENSE.txt`; keep it and the commit provenance with any code taken from there.

Protocol references: [`ndeadly/switch2_controller_research`](https://github.com/ndeadly/switch2_controller_research) and the upstream emulator linked above.

No top-level license file is present yet.

## Limitations

- **ESP32-C6 only**, and ESP-IDF 6.1.0 exactly; both are enforced at configure time.
- SoftAP default password is `playgate` — fine for a bench, not for anywhere else.
- Status counters saturate at 65535, so they are unsuitable as long-run soak measurements without reset.
- `last_receive_to_submit_us` can read stale (including 65535) when idle.
- The gateway is not authenticated: anyone who can reach the UDP port, `/ws`, or the UART can inject input. There is no pairing or auth on any ingress.
- Not affiliated with or endorsed by Nintendo.
