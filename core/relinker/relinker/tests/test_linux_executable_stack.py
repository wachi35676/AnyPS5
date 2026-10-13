"""Check that a Linux relink marks the stack non-executable with one PT_GNU_STACK entry.

Without the entry glibc keeps its executable default and maps every thread stack with PROT_EXEC."""

from pathlib import Path
import platform
import signal
import struct
import subprocess
import sys
import tempfile

from test_linux_input_program_headers import fixture as slots_fixture
from test_linux_load_alignment import fixture as single_load_fixture
from test_linux_program_header_order import DATA, RODATA, TEXT, multi_load_fixture

PT_LOAD = 1
PT_PHDR = 6
PT_GNU_STACK = 0x6474E551
PF_X = 1
PF_W = 2
PF_R = 4


def program_headers(elf):
    phoff, = struct.unpack_from("<Q", elf, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", elf, 0x36)
    assert phoff + phnum * phentsize <= len(elf), (phoff, phnum, phentsize, len(elf))
    return phoff, [struct.unpack_from("<IIQQQQQQ", elf, phoff + index * phentsize) for index in range(phnum)]


def check_stack(name, elf):
    phoff, headers = program_headers(elf)
    stacks = [header for header in headers if header[0] == PT_GNU_STACK]
    assert len(stacks) == 1, (name, [hex(header[0]) for header in headers])
    kind, flags, offset, vaddr, paddr, filesz, memsz, align = stacks[0]
    assert flags == PF_R | PF_W, (name, flags)
    assert (offset, vaddr, paddr, filesz, memsz) == (0, 0, 0, 0, 0), (name, stacks[0])
    assert align != 0 and align & (align - 1) == 0, (name, align)
    phdr = headers[0]
    assert phdr[0] == PT_PHDR and phdr[5] == len(headers) * 56, (name, phdr, len(headers))
    return phoff, headers


def run_expecting_trap(name, output):
    if not (sys.platform.startswith("linux") and platform.machine() in ("x86_64", "AMD64")):
        return
    output.chmod(0o755)
    executed = subprocess.run([str(output)], capture_output=True, timeout=20)
    assert executed.returncode == -signal.SIGTRAP, (name, executed.returncode, executed.stderr)


def main():
    relinker = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="anyps5-executable-stack-") as directory:
        work = Path(directory)

        def convert(name, image):
            source = work / (name + ".elf")
            output = work / (name + ".out.elf")
            source.write_bytes(image)
            result = subprocess.run([str(relinker), "--skip-sce-module", str(source), str(output)],
                                    capture_output=True, text=True, timeout=20)
            assert result.returncode == 0, (name, result.stdout, result.stderr)
            return output

        moved = convert("moved-table", single_load_fixture())
        phoff, _ = check_stack("moved-table", moved.read_bytes())
        assert phoff != 64, phoff

        in_place = convert("in-place-table", slots_fixture(False, False, 9))
        phoff, headers = check_stack("in-place-table", in_place.read_bytes())
        assert phoff == 64 and len(headers) == 9, (phoff, len(headers))
        run_expecting_trap("in-place-table", in_place)

        declared = convert("declared", multi_load_fixture([TEXT, RODATA, DATA, (PT_GNU_STACK, PF_R | PF_W, 0, 0, 0, 0, 16)]))
        check_stack("declared", declared.read_bytes())
        run_expecting_trap("declared", declared)
    print("Linux executable stack test passed")


if __name__ == "__main__":
    main()
