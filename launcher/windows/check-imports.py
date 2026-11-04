#!/usr/bin/env python3
"""Fail if a Windows executable is not x64 or imports a DLL that Windows 10/11 does not ship.

The download must not depend on MinGW runtime DLLs (libstdc++, libgcc, winpthread), SDL2.dll,
zlib1.dll or anything else from the build machine. Reads the PE import tables directly, so it
works on any build host.
"""
from pathlib import Path
import struct
import sys

SYSTEM = {
    "advapi32.dll", "bcrypt.dll", "cfgmgr32.dll", "comctl32.dll", "comdlg32.dll", "crypt32.dll", "dbghelp.dll",
    "dinput8.dll", "dwmapi.dll", "gdi32.dll", "gdiplus.dll", "hid.dll", "imm32.dll", "kernel32.dll", "msvcrt.dll",
    "ntdll.dll", "ole32.dll", "oleaut32.dll", "opengl32.dll", "setupapi.dll", "shell32.dll", "shlwapi.dll",
    "ucrtbase.dll", "user32.dll", "uxtheme.dll", "version.dll", "winmm.dll", "winspool.drv", "ws2_32.dll",
}
AMD64 = 0x8664


def machine_and_imports(path: Path) -> tuple[int, list[str]]:
    data = path.read_bytes()
    if data[:2] != b"MZ":
        raise ValueError(f"{path} is not a Windows executable")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"{path} has no PE header")
    machine, sections, _, _, _, optional_size, _ = struct.unpack_from("<HHIIIHH", data, pe + 4)
    optional = pe + 24
    if struct.unpack_from("<H", data, optional)[0] != 0x20B:
        return machine, []
    directories = optional + 112
    table = [(struct.unpack_from("<IIII", data, optional + optional_size + 40 * k + 8))
             for k in range(sections)]

    def offset(rva: int) -> int:
        for size, address, raw_size, raw in table:
            if address <= rva < address + max(size, raw_size):
                return raw + rva - address
        raise ValueError(f"{path}: address {rva:#x} is outside every section")

    def name(rva: int) -> str:
        start = offset(rva)
        return data[start:data.index(b"\0", start)].decode("ascii")

    imports = []
    for index, record, name_field in ((1, 20, 12), (13, 32, 4)):  # imports, delay-load imports
        rva, size = struct.unpack_from("<II", data, directories + 8 * index)
        if not rva or not size:
            continue
        cursor = offset(rva)
        while True:
            entry = data[cursor:cursor + record]
            if len(entry) < record or not any(entry):
                break
            imports.append(name(struct.unpack_from("<I", entry, name_field)[0]))
            cursor += record
    return machine, imports


def main() -> int:
    failed = False
    for argument in sys.argv[1:]:
        path = Path(argument)
        machine, imports = machine_and_imports(path)
        if machine != AMD64:
            print(f"{path}: not an x64 executable (machine {machine:#x})", file=sys.stderr)
            failed = True
        foreign = [dll for dll in imports if dll.lower() not in SYSTEM
                   and not dll.lower().startswith(("api-ms-win-", "ext-ms-win-"))]
        if foreign:
            print(f"{path}: non-system DLL dependencies {', '.join(foreign)}; the download is not portable.",
                  file=sys.stderr)
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
