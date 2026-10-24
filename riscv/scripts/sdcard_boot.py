#!/usr/bin/env python3
"""Boot the partitioned DoomOS disk using firmware only (no -kernel/-initrd).

Default runs with a visible window and a disposable write snapshot. --check
runs headlessly until the first map frame and exits with a validation result.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
CPU = ('rva23s64,vlen=1024,v=true,vext_spec=v1.0,elen=64,zbc=true,zbkb=true,'
       'zbkc=true,zbkx=true,zfh=true,zvfh=true,zvfbfmin=true,zvfbfwma=true,'
       'zvbc=true,zvkng=true,zvksg=true')
# Scalar rv64imafdc image (BM_VECTOR=0). The optional extensions are turned OFF
# rather than merely unused, so the run actually proves the image needs nothing
# beyond the baseline: anything that slipped through lands as an illegal
# instruction instead of quietly executing.
SCALAR_CPU = 'rv64,v=false,zicboz=false,zfa=false,zba=false,zbb=false,zbs=false'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, default=ROOT / 'build-rv/sdcard/doomos-sdcard.img')
    p.add_argument('--firmware', type=Path, default=ROOT / 'build-rv/sdcard/doomos-firmware.bin')
    p.add_argument('--qemu', default=shutil.which('qemu-system-riscv64') or 'C:/msys64/ucrt64/bin/qemu-system-riscv64.exe')
    p.add_argument('--output', type=Path, default=ROOT / 'build-rv/sdcard/check')
    p.add_argument('--display', default='gtk', choices=('gtk', 'sdl', 'none'))
    p.add_argument('--isa', default='rva23s64', choices=('rva23s64', 'rv64imafdc'),
                   help='rv64imafdc boots a scalar (no-vector) image on a CPU with V disabled')
    p.add_argument('--check', action='store_true')
    p.add_argument('--timeout', type=int, default=600)
    p.add_argument('--hold-seconds', type=int, default=0, help='Keep a successful check alive for visual review')
    p.add_argument('--persist', action='store_true', help='Write saves/config to this image instead of a disposable snapshot')
    p.add_argument('--monitor-port', type=int, default=4488)
    p.add_argument('--gdb-port', type=int, default=4489)
    a = p.parse_args()
    for path in (a.image, a.firmware):
        if not path.is_file():
            p.error(f'Missing {path}')
    a.output.mkdir(parents=True, exist_ok=True)
    a.output = a.output.resolve()
    (a.output / 'result.json').unlink(missing_ok=True)
    log = a.output / 'serial.log'
    scalar = a.isa == 'rv64imafdc'
    cpu = SCALAR_CPU if scalar else CPU
    args = [a.qemu, '-machine', 'virt,acpi=off', '-accel', 'tcg,thread=multi,tb-size=256',
            '-cpu', cpu, '-smp', '4', '-m', '4G', '-bios', a.firmware.resolve().as_posix(),
            '-display', 'none' if a.check else a.display, '-serial', f'file:{log.as_posix()}',
            '-global', 'virtio-mmio.force-legacy=false',
            '-drive', f'file={a.image.resolve().as_posix()},format=raw,id=hd0,if=none',
            '-device', 'virtio-blk-device,drive=hd0', '-device', 'virtio-gpu-device',
            '-device', 'virtio-keyboard-device', '-device', 'virtio-tablet-device',
            '-netdev', 'user,id=net0', '-device', 'virtio-net-device,netdev=net0',
            '-device', 'virtio-rng-pci',
            '-monitor', f'tcp:127.0.0.1:{a.monitor_port},server,nowait',
            '-gdb', f'tcp:127.0.0.1:{a.gdb_port}']
    if not a.persist:
        args.append('-snapshot')
    env = dict(os.environ, TEMP=str(a.output), TMP=str(a.output), TMPDIR=str(a.output))
    (a.output / 'command.json').write_text(json.dumps(args, indent=2) + '\n')
    log.write_text('')
    result = {'success': False, 'firmware_only_boot': True, 'harts': 4,
              'isa': a.isa, 'vlen': None if scalar else 1024, 'cpu': cpu,
              'image': str(a.image.resolve()), 'persistent': a.persist}
    for name, path in [('image', a.image), ('firmware', a.firmware)]:
        with path.open('rb') as f:
            result[name + '_sha256'] = hashlib.file_digest(f, 'sha256').hexdigest()
    start = time.monotonic()
    seen = set()
    with (a.output / 'qemu.stderr').open('w') as err:
        proc = subprocess.Popen(args, env=env, stderr=err)
        try:
            while proc.poll() is None:
                text = log.read_text(errors='replace')
                for marker in ('U-Boot ', '[sdboot] Loading', '[sdboot] Booting', '[disk] DOSFS',
                               'stage=map-ready', 'stage=first-map-present'):
                    if marker in text and marker not in seen:
                        seen.add(marker)
                        result.setdefault('host_observed_seconds', {})[marker] = round(time.monotonic() - start, 3)
                        print(f'{time.monotonic() - start:.2f}s: {marker}', flush=True)
                if a.check and 'stage=first-map-present' in text:
                    required = ('[sdboot] Booting', 'Verifying Checksum ... OK',
                                '[disk] DOSFS volume start LBA=133120',
                                '[sgl-mt] tile-parallel raster: 4 harts',
                                '[doomos-cpu] no V extension (scalar build)' if scalar
                                else '[doomos-cpu] RVV VLEN=1024 bits',
                                'savefs: mounted')
                    result['success'] = all(s in text for s in required)
                    result['boot_markers'] = re.findall(r'\[boot-time\].*', text)
                    result['disk_markers'] = re.findall(r'\[disk\].*', text)
                    print(json.dumps(result, indent=2), flush=True)
                    hold_end = time.monotonic() + a.hold_seconds
                    while proc.poll() is None and time.monotonic() < hold_end:
                        time.sleep(1)
                    break
                if a.check and time.monotonic() - start > a.timeout:
                    result['error'] = 'Timed out before first map frame'
                    break
                time.sleep(0.2)
        except KeyboardInterrupt:
            result['interrupted'] = True
        finally:
            if proc.poll() is None:
                try:
                    with socket.create_connection(('127.0.0.1', a.monitor_port), timeout=3) as s:
                        s.recv(4096)
                        s.sendall(b'quit\n')
                        # Keep the connection open until QEMU handles the command.
                        # Closing immediately can discard the HMP line on Windows.
                        try:
                            while s.recv(4096):
                                pass
                        except ConnectionResetError:
                            pass  # QEMU on Windows may reset the socket on exit.
                    proc.wait(timeout=10)
                except (OSError, subprocess.TimeoutExpired):
                    proc.kill()
                    proc.wait()
            result['qemu_exit_code'] = proc.returncode
            (a.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    if a.check and not result['success']:
        raise SystemExit(f'Boot check failed; see {log}')


if __name__ == '__main__':
    main()
