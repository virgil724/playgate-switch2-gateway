#!/usr/bin/env python3
"""PlayGate interoperability client for the ESP32 input gateway.

UDP works with the Python standard library. Serial and BLE dependencies are
imported only when those transports are selected.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import math
import socket
import struct
import sys
import time
from dataclasses import asdict, dataclass
from typing import Iterable, Optional, Sequence

PLAYGATE_VERSION = 2
PLAYGATE_PAYLOAD_SIZE = 17
PLAYGATE_BUTTON_MASK = (1 << 18) - 1
SERIAL_FRAME_VERSION = 1
SERIAL_INPUT_TYPE = 0x01
SERIAL_STATUS_TYPE = 0x81
GATEWAY_STATUS_SIZE = 20
DEFAULT_UDP_PORT = 26760
BLE_INPUT_UUID = "5f5c9c2e-3b4d-4f93-9b7a-4c70a9d7f1e2"
BLE_STATUS_UUID = "5f5c9c2e-3b4d-4f93-9b7a-4c70a9d7f1e3"

BUTTON_BITS = {
    "A": 0,
    "B": 1,
    "X": 2,
    "Y": 3,
    "L": 4,
    "R": 5,
    "ZL": 6,
    "ZR": 7,
    "PLUS": 8,
    "MINUS": 9,
    "HOME": 10,
    "CAPTURE": 11,
    "LSTICK": 12,
    "RSTICK": 13,
    "UP": 14,
    "DOWN": 15,
    "LEFT": 16,
    "RIGHT": 17,
}

_BUTTON_ALIASES = {
    "+": "PLUS",
    "-": "MINUS",
    "L3": "LSTICK",
    "R3": "RSTICK",
    "LS": "LSTICK",
    "RS": "RSTICK",
    "DPADUP": "UP",
    "DPADDOWN": "DOWN",
    "DPADLEFT": "LEFT",
    "DPADRIGHT": "RIGHT",
}

SOURCE_NAMES = {0: "none", 1: "serial", 2: "udp", 3: "ble"}
HID_STATE_NAMES = {0: "idle", 1: "discoverable", 2: "connecting", 3: "connected"}


@dataclass(frozen=True)
class GatewayStatus:
    version: int
    hid_state: int
    active_source: int
    flags: int
    last_sequence: int
    accepted_total: int
    invalid: int
    stale: int
    conflicts: int
    superseded: int
    latency_us: int

    @property
    def wifi_connected(self) -> bool:
        return bool(self.flags & 0x01)

    @property
    def ble_client_connected(self) -> bool:
        return bool(self.flags & 0x02)

    @property
    def timeout_neutral(self) -> bool:
        return bool(self.flags & 0x04)

    def display_dict(self) -> dict[str, object]:
        result = asdict(self)
        result.update(
            hid_state_name=HID_STATE_NAMES.get(self.hid_state, "unknown"),
            active_source_name=SOURCE_NAMES.get(self.active_source, "unknown"),
            wifi_connected=self.wifi_connected,
            ble_client_connected=self.ble_client_connected,
            timeout_neutral=self.timeout_neutral,
        )
        return result


@dataclass(frozen=True)
class InputState:
    buttons: int = 0
    lx: int = 0
    ly: int = 0
    rx: int = 0
    ry: int = 0


class SequenceCounter:
    """A wrapping uint32 sequence generator."""

    def __init__(self, first: int = 0) -> None:
        if not 0 <= first <= 0xFFFFFFFF:
            raise ValueError("sequence must be in the range 0..0xffffffff")
        self._next = first

    def take(self) -> int:
        value = self._next
        self._next = (value + 1) & 0xFFFFFFFF
        return value


def normalize_axis(value: int) -> int:
    """Clamp an axis to PlayGate's symmetric signed range."""
    return max(-32767, min(32767, int(value)))


def encode_input_payload(
    buttons: int,
    lx: int,
    ly: int,
    rx: int,
    ry: int,
    sequence: int,
) -> bytes:
    """Encode exactly one raw 17-byte PlayGate v2 little-endian payload."""
    if not 0 <= sequence <= 0xFFFFFFFF:
        raise ValueError("sequence must be in the range 0..0xffffffff")
    return struct.pack(
        "<BIhhhhI",
        PLAYGATE_VERSION,
        int(buttons) & PLAYGATE_BUTTON_MASK,
        normalize_axis(lx),
        normalize_axis(ly),
        normalize_axis(rx),
        normalize_axis(ry),
        sequence,
    )


def decode_status(data: bytes) -> GatewayStatus:
    """Decode an exact 20-byte gateway_status_v1 value."""
    if len(data) != GATEWAY_STATUS_SIZE:
        raise ValueError(f"gateway status must be {GATEWAY_STATUS_SIZE} bytes, got {len(data)}")
    values = struct.unpack("<BBBBIHHHHHH", data)
    if values[0] != 1:
        raise ValueError(f"unsupported gateway status version {values[0]}")
    return GatewayStatus(*values)


def crc16_ccitt_false(data: bytes) -> int:
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xffff)."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def cobs_encode(data: bytes) -> bytes:
    """COBS-encode data without adding the zero delimiter."""
    output = bytearray([0])
    code_index = 0
    code = 1
    for byte in data:
        if byte == 0:
            output[code_index] = code
            code_index = len(output)
            output.append(0)
            code = 1
        else:
            output.append(byte)
            code += 1
            if code == 0xFF:
                output[code_index] = code
                code_index = len(output)
                output.append(0)
                code = 1
    output[code_index] = code
    return bytes(output)


def cobs_decode(data: bytes) -> bytes:
    """Decode one delimiter-free COBS packet."""
    if not data:
        raise ValueError("empty COBS packet")
    output = bytearray()
    index = 0
    while index < len(data):
        code = data[index]
        if code == 0:
            raise ValueError("zero byte inside COBS packet")
        index += 1
        end = index + code - 1
        if end > len(data):
            raise ValueError("truncated COBS packet")
        output.extend(data[index:end])
        index = end
        if code != 0xFF and index < len(data):
            output.append(0)
    return bytes(output)


def encode_serial_frame(payload: bytes, frame_type: int = SERIAL_INPUT_TYPE) -> bytes:
    """Frame a payload as version/type/length, CRC LE, COBS, and delimiter."""
    if len(payload) > 0xFF:
        raise ValueError("serial payload is too large for the one-byte length")
    if not 0 <= frame_type <= 0xFF:
        raise ValueError("serial frame type must fit in one byte")
    body = bytes((SERIAL_FRAME_VERSION, frame_type, len(payload))) + payload
    decoded = body + struct.pack("<H", crc16_ccitt_false(body))
    return cobs_encode(decoded) + b"\x00"


def decode_serial_frame(packet: bytes) -> tuple[int, bytes]:
    """Decode and validate one delimiter-free COBS serial packet."""
    decoded = cobs_decode(packet)
    if len(decoded) < 5:
        raise ValueError("serial frame is shorter than its header and CRC")
    version, frame_type, payload_length = decoded[:3]
    if version != SERIAL_FRAME_VERSION:
        raise ValueError(f"unsupported serial frame version {version}")
    if len(decoded) != 3 + payload_length + 2:
        raise ValueError("serial frame length does not match its header")
    expected_crc = struct.unpack_from("<H", decoded, len(decoded) - 2)[0]
    actual_crc = crc16_ccitt_false(decoded[:-2])
    if actual_crc != expected_crc:
        raise ValueError("serial frame CRC mismatch")
    return frame_type, decoded[3:-2]


class SerialFrameDecoder:
    """Incremental zero-delimited COBS frame decoder with error recovery."""

    def __init__(self, maximum_encoded_size: int = 260) -> None:
        self._buffer = bytearray()
        self._maximum_encoded_size = maximum_encoded_size
        self._discarding = False

    def feed(self, data: bytes) -> list[tuple[int, bytes]]:
        frames: list[tuple[int, bytes]] = []
        for byte in data:
            if byte == 0:
                if self._buffer and not self._discarding:
                    try:
                        frames.append(decode_serial_frame(bytes(self._buffer)))
                    except ValueError:
                        pass
                self._buffer.clear()
                self._discarding = False
            elif not self._discarding:
                if len(self._buffer) >= self._maximum_encoded_size:
                    self._buffer.clear()
                    self._discarding = True
                else:
                    self._buffer.append(byte)
        return frames


def parse_buttons(items: Iterable[str], numeric_mask: int = 0) -> int:
    mask = int(numeric_mask) & PLAYGATE_BUTTON_MASK
    for item in items:
        for token in item.replace("|", ",").split(","):
            name = token.strip().upper().replace("_", "").replace("-", "")
            if not name:
                continue
            name = _BUTTON_ALIASES.get(name, name)
            if name not in BUTTON_BITS:
                choices = ", ".join(BUTTON_BITS)
                raise ValueError(f"unknown button {token!r}; choose from {choices}")
            mask |= 1 << BUTTON_BITS[name]
    return mask


def _percentile(values: Sequence[int], percentile: float) -> Optional[float]:
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return float(ordered[0])
    position = (len(ordered) - 1) * percentile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return float(ordered[lower])
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def summarize_latencies(statuses: Sequence[GatewayStatus]) -> dict[str, object]:
    samples = [status.latency_us for status in statuses]
    return {
        "samples": len(samples),
        "p50_us": _percentile(samples, 0.50),
        "p95_us": _percentile(samples, 0.95),
        "min_us": min(samples) if samples else None,
        "max_us": max(samples) if samples else None,
    }


def _state_from_args(args: argparse.Namespace) -> InputState:
    try:
        buttons = parse_buttons(args.press, args.buttons)
    except ValueError as error:
        raise SystemExit(str(error)) from error
    return InputState(buttons, args.lx, args.ly, args.rx, args.ry)


def _soak_state(index: int, base: InputState) -> InputState:
    patterns = (
        base,
        InputState(1 << BUTTON_BITS["A"]),
        InputState(1 << BUTTON_BITS["UP"]),
        InputState(1 << BUTTON_BITS["RIGHT"]),
        InputState(0, 32767, 0, 0, 0),
        InputState(0, -32767, 0, 0, 0),
        InputState(0, 0, -32767, 32767, 32767),
        InputState(),
    )
    return patterns[index % len(patterns)]


def _payload(state: InputState, sequences: SequenceCounter) -> bytes:
    return encode_input_payload(
        state.buttons, state.lx, state.ly, state.rx, state.ry, sequences.take()
    )


class UdpTransport:
    def __init__(self, host: str, port: int, bind: Optional[str], timeout: float) -> None:
        family, socktype, proto, _, address = socket.getaddrinfo(
            host, port, family=socket.AF_INET, type=socket.SOCK_DGRAM
        )[0]
        self._address = address
        self._socket = socket.socket(family, socktype, proto)
        if bind:
            self._socket.bind((bind, 0))
        self._socket.settimeout(timeout)

    def __enter__(self) -> "UdpTransport":
        return self

    def __exit__(self, *_: object) -> None:
        self._socket.close()

    def send(self, payload: bytes) -> None:
        self._socket.sendto(payload, self._address)

    def read_status(self, timeout: float) -> Optional[GatewayStatus]:
        previous_timeout = self._socket.gettimeout()
        self._socket.settimeout(max(timeout, 0.001))
        try:
            self._socket.sendto(b"\xF0", self._address)
            while True:
                data, sender = self._socket.recvfrom(256)
                if sender[0] != self._address[0] or sender[1] != self._address[1]:
                    continue
                return decode_status(data)
        except (OSError, ValueError):
            return None
        finally:
            self._socket.settimeout(previous_timeout)


class SerialTransport:
    def __init__(self, port: str, baud: int, timeout: float) -> None:
        try:
            import serial  # type: ignore[import-not-found]
        except ImportError as error:
            raise SystemExit(
                "Serial mode requires pyserial; install it with "
                "'python -m pip install -r tools/requirements.txt'."
            ) from error
        self._serial = serial.Serial(port=port, baudrate=baud, timeout=0)
        self._decoder = SerialFrameDecoder()

    def __enter__(self) -> "SerialTransport":
        return self

    def __exit__(self, *_: object) -> None:
        self._serial.close()

    def send(self, payload: bytes) -> None:
        self._serial.write(encode_serial_frame(payload))

    def read_status(self, timeout: float) -> Optional[GatewayStatus]:
        deadline = time.monotonic() + timeout
        first_read = True
        while first_read or time.monotonic() < deadline:
            first_read = False
            waiting = self._serial.in_waiting
            chunk = self._serial.read(waiting) if waiting else b""
            for frame_type, payload in self._decoder.feed(chunk):
                if frame_type == SERIAL_STATUS_TYPE:
                    try:
                        return decode_status(payload)
                    except ValueError:
                        continue
            if timeout <= 0:
                break
            time.sleep(min(0.001, max(0.0, deadline - time.monotonic())))
        return None


class BleTransport:
    def __init__(self, client: object, status_queue: "asyncio.Queue[bytes]") -> None:
        self._client = client
        self._status_queue = status_queue

    async def send(self, payload: bytes) -> None:
        await self._client.write_gatt_char(BLE_INPUT_UUID, payload, response=False)  # type: ignore[attr-defined]

    async def read_status(self, timeout: float) -> Optional[GatewayStatus]:
        try:
            while True:
                queued = self._status_queue.get_nowait()
                try:
                    return decode_status(queued)
                except ValueError:
                    continue
        except asyncio.QueueEmpty:
            pass
        try:
            data = await asyncio.wait_for(
                self._client.read_gatt_char(BLE_STATUS_UUID),  # type: ignore[attr-defined]
                timeout=max(timeout, 0.05),
            )
            return decode_status(bytes(data))
        except (asyncio.TimeoutError, ValueError):
            return None


def _duration(args: argparse.Namespace) -> float:
    if args.soak is not None:
        return args.soak
    if args.latency is not None:
        return args.latency
    return args.hold


def _record_status(status: Optional[GatewayStatus], statuses: list[GatewayStatus], verbose: bool) -> None:
    if status is not None:
        statuses.append(status)
        if verbose:
            print(json.dumps(status.display_dict(), sort_keys=True))


def _print_results(statuses: Sequence[GatewayStatus], show_status: bool, latency_mode: bool) -> None:
    if show_status and statuses:
        print(json.dumps(statuses[-1].display_dict(), indent=2, sort_keys=True))
    if latency_mode:
        print(json.dumps({"latency": summarize_latencies(statuses)}, indent=2, sort_keys=True))


def run_sync_transport(transport: object, args: argparse.Namespace) -> None:
    state = _state_from_args(args)
    sequences = SequenceCounter(args.sequence)
    statuses: list[GatewayStatus] = []
    duration = _duration(args)
    status_sampling = args.status or args.latency is not None or args.soak is not None

    if args.status_only:
        status = transport.read_status(args.timeout)  # type: ignore[attr-defined]
        if status is None:
            raise SystemExit("No valid gateway status received before timeout.")
        print(json.dumps(status.display_dict(), indent=2, sort_keys=True))
        return

    start = time.monotonic()
    deadline = start + duration
    period = 1.0 / args.rate
    tick = 0
    next_status = start
    sent_once = False
    try:
        while not sent_once or time.monotonic() < deadline:
            target = start + tick * period
            now = time.monotonic()
            if target > now:
                time.sleep(target - now)
            elif tick and now - target >= period:
                tick = math.floor((now - start) / period)
            elapsed = time.monotonic() - start
            active = _soak_state(int(elapsed / args.rotate_interval), state) if args.soak is not None else state
            transport.send(_payload(active, sequences))  # type: ignore[attr-defined]
            sent_once = True
            tick += 1
            now = time.monotonic()
            if status_sampling and now >= next_status:
                _record_status(
                    transport.read_status(min(args.timeout, period)),  # type: ignore[attr-defined]
                    statuses,
                    args.verbose_status,
                )
                next_status = now + args.status_interval
    finally:
        if sent_once:
            transport.send(_payload(InputState(), sequences))  # type: ignore[attr-defined]

    if status_sampling:
        _record_status(transport.read_status(args.timeout), statuses, args.verbose_status)  # type: ignore[attr-defined]
    if args.status and not statuses:
        raise SystemExit("No valid gateway status received before timeout.")
    _print_results(statuses, args.status, args.latency is not None or args.soak is not None)


async def run_ble_transport(args: argparse.Namespace) -> None:
    try:
        from bleak import BleakClient, BleakScanner  # type: ignore[import-not-found]
    except ImportError as error:
        raise SystemExit(
            "BLE mode requires bleak; install it with "
            "'python -m pip install -r tools/requirements.txt'."
        ) from error

    address = args.address
    if not address:
        device = await BleakScanner.find_device_by_name(args.name, timeout=args.timeout)
        if device is None:
            raise SystemExit(f"BLE device named {args.name!r} was not found.")
        address = device.address

    queue: asyncio.Queue[bytes] = asyncio.Queue()

    def notification(_: object, data: bytearray) -> None:
        queue.put_nowait(bytes(data))

    async with BleakClient(address, timeout=args.timeout) as client:
        try:
            await client.start_notify(BLE_STATUS_UUID, notification)
        except Exception as error:
            print(f"warning: BLE status notifications unavailable: {error}", file=sys.stderr)
        transport = BleTransport(client, queue)
        await run_async_traffic(transport, args)


async def run_async_traffic(transport: BleTransport, args: argparse.Namespace) -> None:
    state = _state_from_args(args)
    sequences = SequenceCounter(args.sequence)
    statuses: list[GatewayStatus] = []
    duration = _duration(args)
    status_sampling = args.status or args.latency is not None or args.soak is not None

    if args.status_only:
        status = await transport.read_status(args.timeout)
        if status is None:
            raise SystemExit("No valid gateway status received before timeout.")
        print(json.dumps(status.display_dict(), indent=2, sort_keys=True))
        return

    loop = asyncio.get_running_loop()
    start = loop.time()
    deadline = start + duration
    period = 1.0 / args.rate
    tick = 0
    next_status = start
    sent_once = False
    try:
        while not sent_once or loop.time() < deadline:
            target = start + tick * period
            now = loop.time()
            if target > now:
                await asyncio.sleep(target - now)
            elif tick and now - target >= period:
                tick = math.floor((now - start) / period)
            elapsed = loop.time() - start
            active = _soak_state(int(elapsed / args.rotate_interval), state) if args.soak is not None else state
            await transport.send(_payload(active, sequences))
            sent_once = True
            tick += 1
            now = loop.time()
            if status_sampling and now >= next_status:
                _record_status(
                    await transport.read_status(min(args.timeout, period)),
                    statuses,
                    args.verbose_status,
                )
                next_status = now + args.status_interval
    finally:
        if sent_once:
            await transport.send(_payload(InputState(), sequences))

    if status_sampling:
        _record_status(await transport.read_status(args.timeout), statuses, args.verbose_status)
    if args.status and not statuses:
        raise SystemExit("No valid gateway status received before timeout.")
    _print_results(statuses, args.status, args.latency is not None or args.soak is not None)


def _unsigned_u32(text: str) -> int:
    value = int(text, 0)
    if not 0 <= value <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("must be in the range 0..0xffffffff")
    return value


def _positive_float(text: str) -> float:
    value = float(text)
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("must be a finite number greater than zero")
    return value


def _nonnegative_float(text: str) -> float:
    value = float(text)
    if not math.isfinite(value) or value < 0:
        raise argparse.ArgumentTypeError("must be a finite non-negative number")
    return value


def _axis(text: str) -> int:
    try:
        value = int(text, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be an integer") from error
    return normalize_axis(value)


def _add_traffic_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--press",
        "--button",
        action="append",
        default=[],
        metavar="NAME[,NAME...]",
        help="buttons to press (A,B,X,Y,L,R,ZL,ZR,PLUS,MINUS,HOME,CAPTURE,LSTICK,RSTICK,UP,DOWN,LEFT,RIGHT)",
    )
    parser.add_argument("--buttons", type=_unsigned_u32, default=0, help="explicit button bitmask (decimal or 0x...); reserved bits are masked")
    parser.add_argument("--lx", type=_axis, default=0, help="left X axis; values are clamped to [-32767,32767]")
    parser.add_argument("--ly", type=_axis, default=0, help="left Y axis; negative is up")
    parser.add_argument("--rx", type=_axis, default=0, help="right X axis")
    parser.add_argument("--ry", type=_axis, default=0, help="right Y axis; negative is up")
    parser.add_argument("--sequence", type=_unsigned_u32, default=0, help="first uint32 sequence (wraps after 0xffffffff)")
    parser.add_argument("--hold", type=_nonnegative_float, default=0.1, metavar="SECONDS", help="hold the selected state, then automatically send neutral (default: 0.1)")
    parser.add_argument("--rate", type=_positive_float, default=120.0, metavar="HZ", help="transmit rate during hold/latency/soak (default: 120)")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--latency", type=_positive_float, metavar="SECONDS", help="send for this duration and report status latency p50/p95")
    modes.add_argument("--soak", type=_positive_float, metavar="SECONDS", help="rotate buttons/sticks for this duration and report latency p50/p95")
    parser.add_argument("--rotate-interval", type=_positive_float, default=1.0, metavar="SECONDS", help="soak pattern rotation interval (default: 1.0)")
    parser.add_argument("--status", action="store_true", help="print the final decoded 20-byte gateway status")
    parser.add_argument("--status-only", action="store_true", help="read/probe status without sending controller input")
    parser.add_argument("--status-interval", type=_positive_float, default=1.0, metavar="SECONDS", help="latency/soak status sampling interval (default: 1.0)")
    parser.add_argument("--verbose-status", action="store_true", help="print each decoded status sample as JSON")
    parser.add_argument("--timeout", type=_positive_float, default=2.0, metavar="SECONDS", help="transport/status timeout (default: 2.0)")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Send PlayGate v2 input to an ESP32 Switch 2 gateway.",
        epilog="Every input run ends with an automatic neutral report. UDP needs no third-party package.",
    )
    subparsers = parser.add_subparsers(dest="transport", required=True)

    udp = subparsers.add_parser("udp", help="send raw 17-byte payloads over UDP")
    udp.add_argument("host", help="gateway IPv4 address or hostname")
    udp.add_argument("--port", type=int, default=DEFAULT_UDP_PORT, help="UDP port (default: 26760)")
    udp.add_argument("--bind", metavar="ADDRESS", help="optional local IPv4 address")
    _add_traffic_arguments(udp)

    serial = subparsers.add_parser("serial", help="send COBS/CRC framed input over Serial (USB Serial/JTAG or UART)")
    serial.add_argument("port", help="Serial device for USB Serial/JTAG or UART, for example COM5 or /dev/ttyUSB0")
    serial.add_argument("--baud", type=int, default=921600, help="baud rate (default: 921600)")
    _add_traffic_arguments(serial)

    ble = subparsers.add_parser("ble", help="write the PlayGate Input BLE GATT characteristic")
    ble.add_argument("--address", help="BLE address/identifier; if omitted, scan by name")
    ble.add_argument("--name", default="PlayGate Input", help="BLE advertising name (default: PlayGate Input)")
    _add_traffic_arguments(ble)
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    if args.transport == "udp":
        with UdpTransport(args.host, args.port, args.bind, args.timeout) as transport:
            run_sync_transport(transport, args)
    elif args.transport == "serial":
        with SerialTransport(args.port, args.baud, args.timeout) as transport:
            run_sync_transport(transport, args)
    elif args.transport == "ble":
        asyncio.run(run_ble_transport(args))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("Interrupted; the gateway will neutralize input after its timeout.", file=sys.stderr)
        raise SystemExit(130)
