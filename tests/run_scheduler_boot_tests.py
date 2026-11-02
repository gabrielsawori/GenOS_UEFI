#!/usr/bin/env python3
"""Exercise scheduler, memory, user access and multicore using real QEMU RAM."""
import argparse
import io
from pathlib import Path
import shlex
import shutil
import subprocess
import tarfile
import tempfile
import json
import socket
import time

ROOT = Path(__file__).resolve().parents[1]
CASES = [("single", 1, 512), ("dual", 2, 512), ("quad", 4, 512),
         ("octa", 8, 512), ("low-64", 4, 64), ("low-128", 4, 128)]


def run(command, **kwargs):
    return subprocess.run(command, cwd=ROOT, check=True, **kwargs)


def inject_input(path, log_path, process, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("guest exited before input probe")
        if log_path.exists() and "[CORE TEST READY] input" in log_path.read_text():
            break
        time.sleep(0.02)
    else:
        raise RuntimeError("guest did not request input\n" + log_path.read_text())
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(5)
        connection.connect(str(path))
        stream = connection.makefile("rwb")
        json.loads(stream.readline())  # QMP greeting

        def execute(name, arguments=None):
            command = {"execute": name}
            if arguments is not None:
                command["arguments"] = arguments
            stream.write((json.dumps(command) + "\n").encode())
            stream.flush()
            while True:
                reply = json.loads(stream.readline())
                if "error" in reply:
                    raise RuntimeError(reply)
                if "return" in reply:
                    return

        execute("qmp_capabilities")
        for down in (True, False):
            execute("input-send-event", {"events": [{"type": "key", "data": {
                "down": down, "key": {"type": "qcode", "data": "a"}}}]})
        execute("input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}},
            {"type": "rel", "data": {"axis": "x", "value": 12}},
            {"type": "rel", "data": {"axis": "y", "value": -7}}]})


def main(default_suite="scheduler"):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=("scheduler", "memory", "uaccess", "core", "umem", "context"), default=default_suite)
    parser.add_argument("--case", action="append", choices=[c[0] for c in CASES])
    parser.add_argument("--timeout", type=int, default=30)
    parser.add_argument("--fatal-probe", choices=("kernel-ud", "user-nmi", "user-mce", "user-rsvd"),
                        help="context-suite negative control; guest must remain in its fatal handler")
    parser.add_argument("--log-dir", type=Path,
                        help="keep guest serial logs outside the temporary build directory")
    args = parser.parse_args()
    suite = args.suite
    if args.fatal_probe and suite != "context":
        parser.error("--fatal-probe requires --suite context")
    if args.log_dir:
        args.log_dir = args.log_dir.resolve()
        args.log_dir.mkdir(parents=True, exist_ok=True)
    # Rebuild production objects so every suite validates the selected flags.
    run(["make", "-B", "kernel.elf"])
    variables = "include Makefile\nprint-test-vars:\n"
    for name in ("CC", "CFLAGS", "USER_CFLAGS", "LD", "LDFLAGS", "OBJS"):
        variables += f"\t@echo '$({name})'\n"
    result = run(["make", "--no-print-directory", "-s", "-f", "-", "print-test-vars"],
                 input=variables, text=True, capture_output=True)
    cc, cflags, user_cflags, ld, ldflags, objects = map(shlex.split, result.stdout.splitlines())
    objects.remove("kernel/task.o")
    with tempfile.TemporaryDirectory(prefix="genos-scheduler-") as tmp:
        work = Path(tmp)
        iso_root = work / "iso_root"
        iso_root.mkdir()
        fixture = work / f"{suite}_boot_test.o"
        fixture_name = "context_fatal" if args.fatal_probe else suite
        probe_flags = ([f"-DCONTEXT_FATAL_PROBE={('kernel-ud', 'user-nmi', 'user-mce', 'user-rsvd').index(args.fatal_probe) + 1}"]
                       if args.fatal_probe else [])
        run(cc + cflags + probe_flags + ["-Werror", "-c", f"tests/{fixture_name}_boot_test.c",
                          "-o", str(fixture)])
        wraps = ["pmm_alloc_contiguous_pages", "pmm_alloc_page", "shm_init",
                 "serial_write_string", "create_user_task", "syscall_handler",
                 "isr_handler", "timer_tick"]
        if suite == "memory":
            wraps = ["pmm_alloc_page", "pmm_ref_page", "shm_init", "serial_write_string",
                     "create_user_task", "syscall_handler", "isr_handler", "vmm_map_page_in",
                     "vmm_map_pages_in"]
        if suite == "core":
            wraps = ["create_user_task", "syscall_handler"]
        if suite == "context":
            wraps = ["create_user_task", "syscall_handler", "isr_handler"]
        if suite == "uaccess":
            wraps = ["pmm_alloc_page", "create_user_task", "syscall_handler", "isr_handler"]
        if suite == "umem":
            wraps = ["pmm_alloc_page", "kmalloc", "vmm_map_page_in", "vmm_map_pages_in",
                     "tlb_shootdown", "shm_init", "syscall_handler"]
        if args.fatal_probe:
            wraps = ["create_user_task"]
        run(ld + ldflags + ["-o", str(iso_root / "kernel.elf")] + objects +
            [str(fixture)] + [f"--wrap={symbol}" for symbol in wraps])
        user_obj = work / "user.o"
        user_elf = work / "shell.elf"
        suffix = "c" if suite in ("uaccess", "core", "umem", "context") else "S"
        # The explicit register probes themselves must remain integer-only.
        probe_flags = cflags if suite == "context" else user_cflags
        run(cc + probe_flags + ["-Werror", "-c", f"tests/{suite}_user_test.{suffix}", "-o", str(user_obj)])
        user_objects = [str(user_obj)]
        if suite == "context":
            math_obj = work / "math.o"
            run(cc + user_cflags + ["-Werror", "-c", "tests/context_math_test.c", "-o", str(math_obj)])
            user_objects.append(str(math_obj))
        if suite == "umem":
            stdlib_obj = work / "stdlib.o"
            run(cc + user_cflags + ["-Werror", "-c", "libc/stdlib.c", "-o", str(stdlib_obj)])
            user_objects.append(str(stdlib_obj))
        if suite == "uaccess":
            stdio_obj = work / "stdio.o"
            run(cc + user_cflags + ["-Werror", "-c", "libc/stdio.c", "-o", str(stdio_obj)])
            user_objects.append(str(stdio_obj))
        run(ld + ["-nostdlib", "-Ttext", "0x50000000", "-o", str(user_elf)] + user_objects)
        with tarfile.open(iso_root / "ramdisk.tar", "w", format=tarfile.USTAR_FORMAT) as tar:
            tar.add(user_elf, arcname="shell.elf")
            if suite == "memory":
                truncated = user_elf.read_bytes()[:64]
                entry = tarfile.TarInfo("truncated.elf")
                entry.size = len(truncated)
                tar.addfile(entry, io.BytesIO(truncated))
        for filename in ("limine-bios.sys", "limine-bios-cd.bin"):
            shutil.copy(ROOT / "limine" / filename, iso_root / filename)
        config = (ROOT / "limine.cfg").read_text().replace("TIMEOUT=3", "TIMEOUT=0")
        (iso_root / "limine.cfg").write_text(config)
        iso = work / "test.iso"
        run(["xorriso", "-as", "mkisofs", "-b", "limine-bios-cd.bin",
             "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
             str(iso_root), "-o", str(iso)], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        for name, cpus, memory in CASES:
            if args.case and name not in args.case:
                continue
            log_label = f"{suite}-{args.fatal_probe}" if args.fatal_probe else suite
            log_path = (args.log_dir / f"{log_label}-{name}.log" if args.log_dir
                        else work / f"{name}.log")
            # Persistent logs may contain a previous READY marker before QEMU
            # has opened/truncated its serial file. Never act on that marker.
            log_path.write_text("")
            command = ["qemu-system-x86_64", "-accel", "tcg", "-m", f"{memory}M",
                       "-smp", str(cpus), "-cdrom", str(iso), "-display", "none",
                       "-monitor", "none", "-serial", f"file:{log_path}",
                       "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
                       "-no-reboot"]
            qmp_path = work / f"{name}.qmp"
            if suite == "core":
                command += ["-qmp", f"unix:{qmp_path},server=on,wait=off"]
            started_at = time.monotonic()
            timed_out = False
            with subprocess.Popen(command, cwd=ROOT, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.PIPE) as process:
                try:
                    if suite == "core":
                        inject_input(qmp_path, log_path, process, args.timeout)
                    _, errors = process.communicate(timeout=args.timeout)
                except subprocess.TimeoutExpired:
                    timed_out = True
                    process.terminate()
                    _, errors = process.communicate(timeout=5)
                except Exception as error:
                    process.terminate()
                    _, errors = process.communicate(timeout=5)
                    log = log_path.read_text() if log_path.exists() else ""
                    raise RuntimeError(f"{name}: {error}\n{log}\n{errors.decode()}") from error
            log = log_path.read_text() if log_path.exists() else ""
            if args.fatal_probe:
                expected = {"kernel-ud": "KERNEL PANIC: Invalid Opcode",
                            "user-nmi": "KERNEL PANIC: Non Maskable Interrupt",
                            "user-mce": "KERNEL PANIC: Machine Check",
                            "user-rsvd": "[FATAL] Page fault kernel atau tabel rusak"}[args.fatal_probe]
                if (not timed_out or process.returncode not in (0, -15) or
                        "[CONTEXT FATAL ARMED]" not in log or
                        expected not in log or "[USER TRAP]" in log or "TEST FAIL" in log):
                    raise RuntimeError(f"negative probe {args.fatal_probe} failed:\n{log}\n{errors.decode()}")
                print(f"PASS {args.fatal_probe}/{name}: fatal handler retained", flush=True)
                continue
            required = ("1000 stack lifecycles", "OOM and thread lifetime",
                        "quantum and preemption guard", "1000 SHM cycles and partial OOM",
                        "PIT time and runtime reaper/syscalls")
            prefix = "SCHED"
            if suite == "memory":
                prefix = "MEM"
                required = ("mappings and rollback", "ELF validation and layout", "ELF allocation failures",
                            "heap growth rollback",
                            "CoW ownership and fork rollback", "1000 fork/write/SHM lifecycles",
                            "Ring3 CoW, shared data, RO and NX")
            if suite == "uaccess":
                prefix = "UACCESS"
                required = ("ranges, permissions and strings", "annotated copy recovery",
                            "1000 CoW OOM retries", "Ring3 syscall buffers and nested pointers")
            if suite == "core":
                prefix = "CORE"
                required = ("PCI and PS2 initialization",
                            "PS2 IRQ to Ring3 input events",
                            "concurrent CPUs, TLB replacement and allocators",
                            "Ring3 multicore fork, clone, sleep and cleanup")
            if suite == "umem":
                prefix = "UMEM"
                required = ("dynamic tracking, guards and VA reuse", "allocation and resize rollback",
                            "large SHM and fork ownership", "Ring3 malloc, fork, threads and cleanup")
            if suite == "context":
                prefix = "CONTEXT"
                required = ("x87/XMM/MXCSR across preemption and sleep",
                            "live fork/clone state and clean exec",
                            "Ring3 traps, group termination and reclamation",
                            "per-CPU kernel and user FP isolation")
            filtered = "\n".join(line for line in log.splitlines()
                                 if "[OK] IST1 (double-fault)" not in line)
            if (timed_out or process.returncode != 1 or
                    not all(f"[{prefix} TEST PASS] {m}" in log for m in required) or
                    any(m in filtered.upper() for m in
                        ("TEST FAIL", "PANIC", "FATAL", "EXCEPTION", "FAULT", "BAD CS"))):
                raise RuntimeError(f"{name} failed (exit {process.returncode}):\n"
                                   f"{log}\n{errors.decode()}")
            elapsed = time.monotonic() - started_at
            print(f"PASS {name}: {cpus} vCPU(s), {memory} MB ({elapsed:.1f}s)", flush=True)


if __name__ == "__main__":
    main()
