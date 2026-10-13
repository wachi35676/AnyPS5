"""Raise STATUS_ENTRYPOINT_NOT_FOUND from a lazily bound unresolved import on an aligned stack that the unwinder can walk."""

import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from test_windows_import_modules import executable, provider

ENTRYPOINT_NOT_FOUND = 0xc0000139
GOT_SLOT = 0x4a20
JUMP_THROUGH_SLOT = bytes.fromhex("ff 25")
CALL_THROUGH_SLOT = bytes.fromhex("ff 15")


def caller(opcode):
    image = executable("a.prx", "absent#A#B")
    code = opcode + struct.pack("<i", GOT_SLOT - 0x4006) + bytes.fromhex("0f 0b")
    image[0x4000:0x4000 + len(code)] = code
    return image


def exception_directory(pe):
    header = struct.unpack_from("<I", pe, 0x3c)[0]
    optional = header + 24
    rva, size = struct.unpack_from("<II", pe, optional + 112 + 3 * 8)
    table = optional + struct.unpack_from("<H", pe, header + 20)[0]
    for index in range(struct.unpack_from("<H", pe, header + 6)[0]):
        virtual_size, start, raw_size, raw = struct.unpack_from("<IIII", pe, table + index * 40 + 8)
        if start <= rva and rva + size <= start + min(virtual_size, raw_size):
            return [struct.unpack_from("<III", pe, raw + rva - start + offset) for offset in range(0, size, 12)]
    raise AssertionError("exception directory has no file data")


def main():
    relinker = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="anyps5-lazy-unresolved-") as directory:
        work = Path(directory)
        seed = work / "seed"
        (seed / "prx").mkdir(parents=True)
        (seed / "prx" / "a.prx").write_bytes(provider(11))
        (seed / "input.elf").write_bytes(executable("a.prx"))
        result = subprocess.run([str(relinker), "--windows", str(seed / "input.elf"), str(seed / "output.exe")],
                                capture_output=True, text=True, timeout=20)
        assert result.returncode == 0, (result.stdout, result.stderr)
        native = (seed / "app0" / "prx" / "a.prx.guest.prx").read_bytes()
        for name, opcode in (("jump", JUMP_THROUGH_SLOT), ("call", CALL_THROUGH_SLOT)):
            case = work / name
            (case / "libs").mkdir(parents=True)
            (case / "libs" / "a.prx").write_bytes(native)
            source = case / "input.elf"
            source.write_bytes(caller(opcode))
            output = case / "output.exe"
            result = subprocess.run([str(relinker), "--windows", "--skip-sce-module", "--lazy-binding", str(source), str(output)],
                                    capture_output=True, text=True, timeout=20)
            assert result.returncode == 0, (name, result.stdout, result.stderr)
            if os.name == "nt":
                run = subprocess.run([str(output)], capture_output=True, text=True, timeout=20)
                assert "FAIL: unresolved ELF import absent from a.prx" in run.stderr, (name, run.stderr)
                assert run.returncode & 0xffffffff == ENTRYPOINT_NOT_FOUND, \
                    f"{name}: exit status {run.returncode & 0xffffffff:#x} instead of {ENTRYPOINT_NOT_FOUND:#x}"
            functions = exception_directory(output.read_bytes())
            assert len(functions) == 2 and functions[0][1] <= functions[1][0] < functions[1][1], (name, functions)
    print("Windows lazy unresolved import tests passed (reached by jump and by call)")


if __name__ == "__main__":
    main()
