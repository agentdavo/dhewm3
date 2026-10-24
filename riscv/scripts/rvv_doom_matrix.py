#!/usr/bin/env python3
"""Build matching-VLEN Doom shader/resolve tests, then run QEMU serially.

Compilation can run concurrently; measured QEMU runs never overlap. Logs and
ELFs remain under riscv/build/doom-matrix. Run with the compiler DLLs on PATH.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[2]
RV = ROOT / "riscv"
SG = RV / "softgles2"
RT = [RV / p for p in ("boot/start.S", "boot/runtime.c", "boot/sbi.c", "boot/trap.c",
                       "boot/fdt.c", "libc/string.c", "libc/printf.c", "libc/malloc.c")]
CORE = [SG / "src" / (p + ".c") for p in
        ("sg_diag", "glsl", "glsl_lex", "glsl_parse", "glsl_ir", "glsl_simt", "sg_kernels", "sg_fb", "sg_raster", "sg_prof", "sg_texture")]
TESTS = {"interaction": "iakern_gl4es_validate", "bindings": "kernel_bindings_validate",
         "resolve": "resolve_validate", "stencil": "stencil_compare_validate", "copy": "copy_texture_validate",
         "alpha": "alpha_store_validate", "setup": "raster_setup_validate",
         "overlay": "overlay_validate", "dither": "dither_bias_validate"}


def command(args, log):
    with log.open("w") as stream:
        result = subprocess.run([str(x) for x in args], cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"Command failed; see {log}")


def build(vlen, args, pool):
    folder = args.out / str(vlen)
    folder.mkdir(parents=True, exist_ok=True)
    flags = [f"-march=rva23s64_zbc_zbkb_zbkc_zbkx_zfh_zvfh_zvfbfwma_zvbc_zvkng_zvksg_zvl{vlen}b",
             "-mabi=lp64d", "-mcmodel=medany", "-mrvv-vector-bits=zvl", "-O2", "-g",
             "-ffreestanding", "-fno-pic", "-fno-common", "-fno-math-errno",
             "-fno-tree-loop-distribute-patterns", "-ffunction-sections", "-fdata-sections",
             "-DSGL_BAREMETAL", "-DSGL_GATHER_RUNTIME_TOGGLE", f"-DSGL_SIMT_LANES={vlen // 16}",
             "-I" + str(RV), "-I" + str(SG / "src"), "-I" + str(SG / "include"), "-I" + str(SG / "test")]
    sources = RT + CORE + [SG / "test" / (name + ".c") for name in TESTS.values()]
    digest = hashlib.sha256((str(args.cc) + repr(flags)).encode())
    digest.update((RV / 'boot/linker.ld').read_bytes())
    digest.update(Path(__file__).read_bytes())
    digest.update(str(getattr(args, "setup_focused", False)).encode())
    for path in sorted(set(sources + list((SG / "src").glob("*.h")) +
                           list((SG / "include").glob("*.h")) + list((SG / "test").glob("*.h")) +
                           list((RV / "boot").glob("*.h")) + list((RV / "libc").glob("*.h")))):
        digest.update(str(path).encode()); digest.update(path.read_bytes())
    stamp = folder / "build.sha256"
    key = digest.hexdigest()
    if not args.link_only and stamp.exists() and stamp.read_text() == key and all((folder / (t + ".elf")).exists() for t in TESTS):
        return []
    jobs = []
    objects = {}
    for source in sources:
        obj = folder / (source.name + ".o")
        objects[source] = obj
        if args.link_only:
            if not obj.exists():
                raise RuntimeError(f"Missing object for --link-only: {obj}")
        else:
            extra = [f"-DSGL_SETUP_BENCH_FOCUSED={args.setup_focused}"] if getattr(args, "setup_focused", False) and source.name == "raster_setup_validate.c" else []
            jobs.append(pool.submit(command, [args.cc, *flags, *extra, "-c", source, "-o", obj],
                                    folder / (source.name + ".build.log")))

    def link_all():
        for job in jobs:
            job.result()
        for test, name in TESTS.items():
            core = CORE if test in ("stencil", "alpha", "setup") else [p for p in CORE if p.stem not in ("sg_raster", "sg_prof")]
            core = [p for p in core if p.stem != "sg_texture" or test == "copy"]
            if test == "dither": core = []  # includes production raster source; GC drops unused entry points
            command([args.cc, *flags, "-nostdlib", "-Wl,--gc-sections",
                     "-Wl,-T" + str(RV / "boot/linker.ld"),
                     *[objects[p] for p in RT + core], objects[SG / "test" / (name + ".c")],
                     "-lgcc", "-o", folder / (test + ".elf")], folder / (test + ".link.log"))
        stamp.write_text(key)
        print(f"VLEN {vlen}: {len(TESTS)} test payloads built", flush=True)
    return [link_all]


def qemu_run(command, timeout, cpu):
    if cpu is None:
        return subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=timeout)
    process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        if os.name == "nt":
            import ctypes
            from ctypes import wintypes
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
            kernel.OpenProcess.restype = wintypes.HANDLE
            kernel.SetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
            kernel.SetProcessAffinityMask.restype = wintypes.BOOL
            kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            if not 0 <= cpu < min(os.cpu_count() or 1, ctypes.sizeof(ctypes.c_size_t)*8):
                raise ValueError("CPU must fit this process's Windows affinity mask")
            handle = kernel.OpenProcess(0x0200, False, process.pid)
            if not handle:
                raise ctypes.WinError(ctypes.get_last_error())
            try:
                if not kernel.SetProcessAffinityMask(handle, 1 << cpu):
                    raise ctypes.WinError(ctypes.get_last_error())
            finally:
                kernel.CloseHandle(handle)
        else:
            os.sched_setaffinity(process.pid, {cpu})
        output, _ = process.communicate(timeout=timeout)
        return subprocess.CompletedProcess(command, process.returncode, output)
    except BaseException:
        process.kill()
        process.communicate()
        raise


def run(vlen, test, args, repetition):
    folder = args.out / str(vlen)
    log = folder / f"{test}-{repetition}.log"
    cpu = (f"rva23s64,vlen={vlen},v=true,vext_spec=v1.0,elen=64,zbc=true,zbkb=true,zbkc=true,"
           "zbkx=true,zfh=true,zvfh=true,zvfbfmin=true,zvfbfwma=true,zvbc=true,zvkng=true,zvksg=true")
    result = qemu_run([str(args.qemu), "-machine", "virt,acpi=off", "-cpu", cpu,
                             "-smp", "1", "-m", "1G", "-display", "none", "-monitor", "none",
                             "-bios", "default", "-serial", "file:" + log.as_posix(),
                             "-kernel", str(folder / (test + ".elf"))], args.timeout, getattr(args, "cpu", None))
    data = log.read_text(errors="replace") if log.exists() else ""
    passed = result.returncode == 0 and "FATAL" not in data and "FAIL" not in data and "MISMATCH" not in data
    if test == "interaction":
        passed &= "[gik] done." in data and "[gik-vs] kernel=YES maxdiff=0 e-6 bitmiss=0 OK" in data
        passed &= "bitmiss=0  OK(bit-exact)" in data
        values = re.findall(r"\[gik\] kernel (\d+) ns/frag", data)
        metrics = {"kernel_ns_per_fragment": int(values[-1])} if values else {}
    elif test == "bindings":
        passed &= "[bindings] PASS" in data
        metrics = {}
    elif test == "overlay":
        passed &= "[ovk] ALL PASS" in data
        metrics = {}
    elif test in ("stencil", "copy", "alpha", "setup", "dither"):
        passed &= f"[{test}] PASS" in data
        metrics = {}
    else:
        passed &= "[resolve] PASS 56" in data
        pairs = [(int(a), int(b)) for a, b in re.findall(r"legacy_ticks=(\d+) rvv_ticks=(\d+)", data)]
        metrics = {"legacy_ticks": statistics.median(a for a, _ in pairs),
                   "rvv_ticks": statistics.median(b for _, b in pairs),
                   "speedup": statistics.median(a / b for a, b in pairs)} if pairs else {}
    row = {"vlen": vlen, "test": test, "repetition": repetition, "cpu": getattr(args, "cpu", None), "passed": bool(passed),
           "log": str(log), **metrics}
    print(json.dumps(row), flush=True)
    return row


def main():
    local_cc = ROOT / "build/toolchain/install/bin" / ("riscv64-unknown-elf-gcc.exe" if os.name == "nt" else "riscv64-unknown-elf-gcc")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", type=Path, default=local_cc if local_cc.is_file() else shutil.which("riscv64-unknown-elf-gcc"))
    parser.add_argument("--qemu", type=Path, default=Path("C:/msys64/ucrt64/bin/qemu-system-riscv64.exe") if os.name == "nt" else shutil.which("qemu-system-riscv64"))
    parser.add_argument("--out", type=Path, default=RV / "build/doom-matrix")
    parser.add_argument("--vlens", nargs="+", type=int, choices=[128, 256, 512, 1024], default=[128, 256, 512, 1024])
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument("--setup-focused", nargs="?", const=32, default=0, type=int, choices=[4,8,12,32], help="Long balanced tiny-triangle benchmark for this varying count (default 32); ignore its first two rounds")
    parser.add_argument("--cpu", type=int, help="Pin each single-hart QEMU benchmark to this logical CPU")
    parser.add_argument("--tests", nargs="+", choices=list(TESTS), default=list(TESTS), help="Payloads to run; building still prepares the complete matrix")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--run-only", action="store_true")
    parser.add_argument("--link-only", action="store_true", help="Reuse already compiled matching objects after a link failure")
    args = parser.parse_args()
    if not args.run_only:
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            finish = [fn for vlen in args.vlens for fn in build(vlen, args, pool)]
            for fn in finish:
                fn()
    if not args.build_only:
        rows = [run(vlen, test, args, rep) for rep in range(args.repetitions)
                for vlen in args.vlens for test in args.tests]
        (args.out / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
        if not all(row["passed"] for row in rows):
            raise SystemExit("Validation failed; inspect results.json and per-test logs")


if __name__ == "__main__":
    main()
