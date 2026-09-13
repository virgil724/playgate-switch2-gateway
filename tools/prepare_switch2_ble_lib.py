#!/usr/bin/env python3
"""Prepare and verify the build-local ESP32-C6 Switch 2 BLE controller archive."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from typing import NoReturn, Sequence


TARGET = "esp32c6"
MEMBER_NAME = "ble_ll_conn.c.o"
SYMBOL_NAME = "r_ble_ll_conn_slave_start"
SOURCE_ARCHIVE_SHA256 = "88ad3871130c3424c75c864f1d7ec00c8d990dc60ae965626234b345dbb401b2"
SOURCE_OBJECT_SHA256 = "6c191e7025fddeda3cd527095004711266a6f9c59cac4880eafd2ba1286f77d8"
PATCHED_ARCHIVE_SHA256 = "e0d52f889a74a96333291ea81a95975c0f280e734dba60bbaa2179532ba87723"
INSTRUCTION_FILE_OFFSET = 0x4052
INSTRUCTION_SYMBOL_OFFSET = 0x184
OLD_INSTRUCTION_WORD = "ffa70793"
PATCHED_INSTRUCTION_WORD = "ffc70793"
OLD_INSTRUCTION_BYTES = bytes.fromhex("9307a7ff")
PATCHED_INSTRUCTION_BYTES = bytes.fromhex("9307c7ff")

_SYMBOL_HEADER_RE = re.compile(r"^\s*([0-9a-fA-F]+)\s+<([^>]+)>:\s*$")
_INSTRUCTION_RE = re.compile(r"^\s*([0-9a-fA-F]+):\s+([0-9a-fA-F]{8})\s+(.+?)\s*$")


class VerificationError(RuntimeError):
    """A fail-closed verification failure."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _fail(message: str) -> NoReturn:
    raise VerificationError(message)


def _resolve_tool(environment_name: str, executable_name: str) -> Path:
    configured = os.environ.get(environment_name)
    candidate = configured if configured else shutil.which(executable_name)
    if not candidate:
        _fail(
            f"required ESP32-C6 toolchain executable was not found: {executable_name} "
            f"(set {environment_name} or activate ESP-IDF)"
        )

    path = Path(candidate).expanduser().resolve()
    if not path.is_file():
        _fail(f"configured {environment_name} is not a file: {path}")

    basename = path.name.lower().removesuffix(".exe")
    if executable_name == "riscv32-esp-elf-ar":
        accepted_names = {"riscv32-esp-elf-ar", "riscv32-esp-elf-gcc-ar"}
    else:
        accepted_names = {executable_name}
    if basename not in accepted_names:
        _fail(f"refusing non-ESP32-C6 tool for {environment_name}: {path}")
    return path


def _run_tool(tool: Path, arguments: Sequence[str], *, cwd: Path | None = None) -> str:
    command = [str(tool), *arguments]
    try:
        completed = subprocess.run(
            command,
            cwd=cwd,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except OSError as error:
        _fail(f"could not execute {tool}: {error}")

    if completed.returncode != 0:
        output = (completed.stdout + completed.stderr).strip()
        _fail(
            f"tool failed with exit {completed.returncode}: {' '.join(command)}"
            + (f"\n{output}" if output else "")
        )
    return completed.stdout


def _format_offsets(offsets: Sequence[int]) -> str:
    return "[" + ", ".join(f"0x{offset:x}" for offset in offsets) + "]"


def _find_offsets(data: bytes, needle: bytes) -> list[int]:
    offsets: list[int] = []
    start = 0
    while True:
        offset = data.find(needle, start)
        if offset < 0:
            return offsets
        offsets.append(offset)
        start = offset + 1


def _symbol_instructions(objdump: Path, binary: Path) -> tuple[int, list[tuple[int, str, str]]]:
    disassembly = _run_tool(
        objdump,
        ["-d", f"--disassemble={SYMBOL_NAME}", str(binary)],
    )

    symbol_bases: list[int] = []
    instructions: list[tuple[int, str, str]] = []
    in_symbol = False
    for line in disassembly.splitlines():
        header = _SYMBOL_HEADER_RE.match(line)
        if header:
            in_symbol = header.group(2) == SYMBOL_NAME
            if in_symbol:
                symbol_bases.append(int(header.group(1), 16))
            continue
        if not in_symbol:
            continue
        instruction = _INSTRUCTION_RE.match(line)
        if instruction:
            instructions.append(
                (
                    int(instruction.group(1), 16),
                    instruction.group(2).lower(),
                    instruction.group(3),
                )
            )

    if len(symbol_bases) != 1:
        _fail(
            f"objdump found {len(symbol_bases)} definitions of {SYMBOL_NAME} in {binary}; "
            "expected exactly one"
        )
    if not instructions:
        _fail(f"objdump found no instructions in {SYMBOL_NAME} in {binary}")
    return symbol_bases[0], instructions


def _instruction_offsets(
    symbol_base: int,
    instructions: Sequence[tuple[int, str, str]],
    word: str,
) -> list[int]:
    return [address - symbol_base for address, encoding, _assembly in instructions if encoding == word]


def _normalized_assembly(assembly: str) -> str:
    return "".join(assembly.split()).lower()


def _verify_instruction(
    objdump: Path,
    binary: Path,
    *,
    required_word: str,
    forbidden_word: str,
    required_assembly: str,
    expected_offset: int | None = INSTRUCTION_SYMBOL_OFFSET,
) -> None:
    symbol_base, instructions = _symbol_instructions(objdump, binary)
    required_offsets = _instruction_offsets(symbol_base, instructions, required_word)
    forbidden_offsets = _instruction_offsets(symbol_base, instructions, forbidden_word)
    if expected_offset is None:
        if len(required_offsets) != 1:
            _fail(
                f"{binary}: {required_word} offsets in {SYMBOL_NAME} are "
                f"{_format_offsets(required_offsets)}; expected exactly one"
            )
        selected_offset = required_offsets[0]
    else:
        if required_offsets != [expected_offset]:
            _fail(
                f"{binary}: {required_word} offsets in {SYMBOL_NAME} are "
                f"{_format_offsets(required_offsets)}; expected [0x{expected_offset:x}]"
            )
        selected_offset = expected_offset
    if forbidden_offsets:
        _fail(
            f"{binary}: forbidden {forbidden_word} offsets in {SYMBOL_NAME} are "
            f"{_format_offsets(forbidden_offsets)}; expected []"
        )

    matching = [
        assembly
        for address, encoding, assembly in instructions
        if encoding == required_word and address - symbol_base == selected_offset
    ]
    actual_assembly = _normalized_assembly(matching[0])
    if actual_assembly != _normalized_assembly(required_assembly):
        _fail(
            f"{binary}: instruction at {SYMBOL_NAME}+0x{selected_offset:x} "
            f"disassembles as {matching[0]!r}; expected {required_assembly!r}"
        )


def _default_source_archive(idf_path: Path) -> Path:
    return (
        idf_path
        / "components"
        / "bt"
        / "controller"
        / "lib_esp32c6"
        / "esp32c6-bt-lib"
        / "esp32c6"
        / "libble_app.a"
    )


def _is_relative_to(path: Path, directory: Path) -> bool:
    try:
        path.relative_to(directory)
    except ValueError:
        return False
    return True


def _prepare_archive(idf_path: Path, source_archive: Path, output: Path) -> None:
    ar = _resolve_tool("SWITCH2_AR", "riscv32-esp-elf-ar")
    objdump = _resolve_tool("SWITCH2_OBJDUMP", "riscv32-esp-elf-objdump")

    if not source_archive.is_file():
        _fail(f"source archive does not exist: {source_archive}")
    source_hash = _sha256(source_archive)
    if source_hash != SOURCE_ARCHIVE_SHA256:
        _fail(
            f"source archive SHA-256 mismatch for {source_archive}: "
            f"actual {source_hash}, expected {SOURCE_ARCHIVE_SHA256}"
        )

    resolved_output = output.resolve()
    if resolved_output == source_archive or _is_relative_to(resolved_output, idf_path):
        _fail(f"refusing to write into ESP-IDF or overwrite the source archive: {resolved_output}")
    output_parent = resolved_output.parent
    output_parent.mkdir(parents=True, exist_ok=True)

    members = [line.strip() for line in _run_tool(ar, ["t", str(source_archive)]).splitlines()]
    member_count = members.count(MEMBER_NAME)
    if member_count != 1:
        _fail(
            f"archive member {MEMBER_NAME!r} occurs {member_count} times in {source_archive}; "
            "expected exactly once"
        )

    with tempfile.TemporaryDirectory(prefix="switch2_ble_", dir=output_parent) as temporary_name:
        temporary = Path(temporary_name)
        local_archive = temporary / "libble_app.a"
        shutil.copyfile(source_archive, local_archive)
        _run_tool(ar, ["x", str(local_archive), MEMBER_NAME], cwd=temporary)

        object_file = temporary / MEMBER_NAME
        if not object_file.is_file():
            _fail(f"toolchain ar did not extract {MEMBER_NAME}")
        object_hash = _sha256(object_file)
        if object_hash != SOURCE_OBJECT_SHA256:
            _fail(
                f"extracted object SHA-256 mismatch for {MEMBER_NAME}: "
                f"actual {object_hash}, expected {SOURCE_OBJECT_SHA256}"
            )

        object_data = object_file.read_bytes()
        old_offsets = _find_offsets(object_data, OLD_INSTRUCTION_BYTES)
        patched_offsets = _find_offsets(object_data, PATCHED_INSTRUCTION_BYTES)
        if old_offsets != [INSTRUCTION_FILE_OFFSET]:
            _fail(
                f"{MEMBER_NAME}: old instruction byte offsets are {_format_offsets(old_offsets)}; "
                f"expected [0x{INSTRUCTION_FILE_OFFSET:x}]"
            )
        if patched_offsets:
            _fail(
                f"{MEMBER_NAME}: source already contains patched instruction at "
                f"{_format_offsets(patched_offsets)}; expected []"
            )
        _verify_instruction(
            objdump,
            object_file,
            required_word=OLD_INSTRUCTION_WORD,
            forbidden_word=PATCHED_INSTRUCTION_WORD,
            required_assembly="addi a5,a4,-6",
        )

        patched_data = bytearray(object_data)
        patched_data[
            INSTRUCTION_FILE_OFFSET : INSTRUCTION_FILE_OFFSET + len(OLD_INSTRUCTION_BYTES)
        ] = PATCHED_INSTRUCTION_BYTES
        object_file.write_bytes(patched_data)
        _verify_instruction(
            objdump,
            object_file,
            required_word=PATCHED_INSTRUCTION_WORD,
            forbidden_word=OLD_INSTRUCTION_WORD,
            required_assembly="addi a5,a4,-4",
        )

        _run_tool(ar, ["rD", str(local_archive), MEMBER_NAME], cwd=temporary)
        output_hash = _sha256(local_archive)
        if output_hash != PATCHED_ARCHIVE_SHA256:
            _fail(
                f"patched archive SHA-256 mismatch: actual {output_hash}, "
                f"expected {PATCHED_ARCHIVE_SHA256}"
            )
        os.replace(local_archive, output)

    print(f"prepared {output} (SHA-256 {PATCHED_ARCHIVE_SHA256})")


def _verify_elf(elf: Path) -> None:
    if not elf.is_file():
        _fail(f"ELF does not exist: {elf}")
    objdump = _resolve_tool("SWITCH2_OBJDUMP", "riscv32-esp-elf-objdump")
    _verify_instruction(
        objdump,
        elf,
        required_word=PATCHED_INSTRUCTION_WORD,
        forbidden_word=OLD_INSTRUCTION_WORD,
        required_assembly="addi a5,a4,-4",
        expected_offset=None,
    )
    print(
        f"verified {elf}: {SYMBOL_NAME} contains exactly one addi a5,a4,-4 "
        "and no addi a5,a4,-6"
    )


def _parse_arguments(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Prepare or verify the fail-closed ESP32-C6 Switch 2 BLE controller patch."
    )
    parser.add_argument("--idf-path", required=True, type=Path)
    parser.add_argument("--target", required=True, choices=[TARGET])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--output", type=Path)
    mode.add_argument("--verify-elf", type=Path)
    parser.add_argument(
        "--source-archive",
        type=Path,
        help="test-only override for the verified ESP-IDF source archive",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parse_arguments(sys.argv[1:] if argv is None else argv)
    idf_path = arguments.idf_path.expanduser().resolve()
    if not idf_path.is_dir():
        print(f"error: IDF path does not exist: {idf_path}", file=sys.stderr)
        return 1

    try:
        if arguments.verify_elf is not None:
            if arguments.source_archive is not None:
                _fail("--source-archive is valid only with --output")
            _verify_elf(arguments.verify_elf.expanduser().resolve())
        else:
            source_archive = (
                arguments.source_archive.expanduser().resolve()
                if arguments.source_archive is not None
                else _default_source_archive(idf_path).resolve()
            )
            _prepare_archive(
                idf_path,
                source_archive,
                arguments.output.expanduser().resolve(),
            )
    except VerificationError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
