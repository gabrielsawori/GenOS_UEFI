#!/usr/bin/env python3
"""Build temporary instrumented kernels and test SMP boot under QEMU TCG."""
import argparse
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = [("single", 1, 0), ("quad", 4, 0), ("fallback", 1, 1),
         ("reversed", 4, 2), ("bsp-beyond-limit", 17, 2),
         ("allocation-failure", 4, 3)]


def run(command, **kwargs):
    return subprocess.run(command, cwd=ROOT, check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=int, default=15,
                        help="QEMU observation time per case in seconds")
    parser.add_argument("--case", action="append", choices=[case[0] for case in CASES],
                        help="run only the selected case(s)")
    args = parser.parse_args()
    run(["make", "kernel.elf", "ramdisk.tar"])

    # Use the repository's compiler, flags, and exact object list.
    variables = "include Makefile\nprint-smp-vars:\n"
    for name in ("CC", "CFLAGS", "LD", "LDFLAGS", "OBJS"):
        variables += f"\t@echo '$({name})'\n"
    result = run(["make", "--no-print-directory", "-s", "-f", "-", "print-smp-vars"],
                 input=variables, text=True, capture_output=True)
    cc, cflags, ld, ldflags, objects = map(shlex.split, result.stdout.splitlines())
    cases = [case for case in CASES if not args.case or case[0] in args.case]

    with tempfile.TemporaryDirectory(prefix="genos-smp-") as tmp:
        work = Path(tmp)
        iso_root = work / "iso_root"
        iso_root.mkdir()
        for filename in ("limine-bios.sys", "limine-bios-cd.bin"):
            shutil.copy(ROOT / "limine" / filename, iso_root / filename)
        shutil.copy(ROOT / "ramdisk.tar", iso_root / "ramdisk.tar")
        config = (ROOT / "limine.cfg").read_text().replace("TIMEOUT=3", "TIMEOUT=0")
        (iso_root / "limine.cfg").write_text(config)

        for name, cpus, mode in cases:
            fixture = work / "smp_boot_test.o"
            run(cc + cflags + [f"-DSMP_TEST_MODE={mode}", "-c",
                              "tests/smp_boot_test.c", "-o", str(fixture)])
            wraps = ["smp_init", "smp_start_timers", "percpu_init_ap", "lapic_timer_init",
                     "pmm_alloc_contiguous_pages"]
            run(ld + ldflags + ["-o", str(iso_root / "kernel.elf")] + objects +
                [str(fixture)] + [f"--wrap={symbol}" for symbol in wraps])
            iso = work / "test.iso"
            run(["xorriso", "-as", "mkisofs", "-b", "limine-bios-cd.bin",
                 "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
                 str(iso_root), "-o", str(iso)], stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL)
            log_path = work / f"{name}.log"
            command = ["qemu-system-x86_64", "-accel", "tcg", "-m", "512M",
                       "-smp", str(cpus), "-cdrom", str(iso), "-display", "none",
                       "-monitor", "none", "-serial", f"file:{log_path}",
                       "-no-reboot", "-no-shutdown"]
            with subprocess.Popen(command, cwd=ROOT, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.PIPE) as process:
                try:
                    _, errors = process.communicate(timeout=args.timeout)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    _, errors = process.communicate(timeout=5)
            log = log_path.read_text() if log_path.exists() else ""
            required = ("[SMP TEST PASS] topology and stacks",
                        "[SMP TEST PASS] timers", "[SWITCH #1]")
            # The normal IST initialization message mentions 'double-fault'.
            filtered = "\n".join(line for line in log.splitlines()
                                 if "[OK] IST1 (double-fault)" not in line)
            if (not all(marker in log for marker in required) or
                    any(marker in filtered.upper() for marker in
                        ("SMP TEST FAIL", "PANIC", "FATAL", "EXCEPTION", "FAULT"))):
                raise RuntimeError(f"{name} failed:\n{log}\n{errors.decode()}")
            print(f"PASS {name}: {cpus} vCPU(s)", flush=True)


if __name__ == "__main__":
    main()
