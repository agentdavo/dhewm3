#!/usr/bin/env python3
"""Send a profiling command or capture stable scanout RAM through QEMU's debugger.

Requires run-win.sh GDB_PORT=4469 MONITOR_PORT=4468 VIDEO=gpu and a bare-metal
engine. Commands execute between frames; wait for [debug-command] in the log
and at least one subsequent rendered frame before capturing their effect.
"""
import argparse
from pathlib import Path
import os
import shutil
import socket
import struct
import subprocess
import time
from capture_screendump_win import write_png


class Debugger:
    def __init__(self, port, monitor_port):
        self.monitor_port = monitor_port
        with socket.create_connection(('127.0.0.1', monitor_port), timeout=10) as monitor:
            monitor.recv(8192)
            monitor.sendall(b'stop\n')
            time.sleep(.1)
        self.s = socket.create_connection(('127.0.0.1', port), timeout=10)
        self.packet('?')

    def receive(self):
        while True:
            marker = self.s.recv(1)
            if not marker:
                raise RuntimeError('Debugger disconnected')
            if marker == b'$':
                break
        data = bytearray()
        while True:
            b = self.s.recv(1)
            if not b:
                raise RuntimeError('Debugger disconnected')
            if b == b'#':
                break
            data.extend(b)
        checksum = self.s.recv(2)
        if int(checksum, 16) != sum(data) % 256:
            raise RuntimeError('Debugger checksum mismatch')
        self.s.sendall(b'+')
        return data.decode()

    def packet(self, text):
        data = text.encode()
        self.s.sendall(b'$' + data + b'#' + ('%02x' % (sum(data) % 256)).encode())
        return self.receive()

    def read(self, addr, size):
        return bytes.fromhex(self.packet(f'm{addr:x},{size:x}'))

    def write(self, addr, data):
        if self.packet(f'M{addr:x},{len(data):x}:{data.hex()}') != 'OK':
            raise RuntimeError('Debugger write failed')

    def close(self):
        self.packet('D')
        self.s.close()
        with socket.create_connection(('127.0.0.1', self.monitor_port), timeout=10) as monitor:
            monitor.recv(8192)
            monitor.sendall(b'cont\n')
            time.sleep(.1)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--elf', default=str(Path(__file__).resolve().parents[2] / 'build-rv' / 'dhewm3'))
    p.add_argument('--nm', default=shutil.which('riscv64-unknown-elf-nm') or str(Path(__file__).resolve().parents[2] / 'build/toolchain/install/bin' / ('riscv64-unknown-elf-nm.exe' if os.name == 'nt' else 'riscv64-unknown-elf-nm')))
    p.add_argument('--gdb-port', type=int, default=4469)
    p.add_argument('--monitor-port', type=int, default=4468)
    p.add_argument('--command')
    p.add_argument('--set-int', nargs=2, metavar=('SYMBOL', 'VALUE'),
                   help='Set an integer diagnostic switch while the guest is stopped')
    p.add_argument('--capture', type=Path)
    a = p.parse_args()
    symbols = {}
    scanout_symbols = []
    for line in subprocess.check_output([a.nm, a.elf], text=True).splitlines():
        fields = line.split()
        if len(fields) == 3:
            symbols[fields[2]] = int(fields[0], 16)
            if fields[2] == 'g_scanout':
                scanout_symbols.append(int(fields[0], 16))
    d = Debugger(a.gdb_port, a.monitor_port)
    try:
        if a.set_int:
            name, value = a.set_int
            d.write(symbols[name], struct.pack('<i', int(value, 0)))
            print(f'{name}={value}')
        if a.command:
            data = (a.command + '\n\0').encode()
            if len(data) > 512:
                raise ValueError('Command exceeds mailbox size')
            seq = struct.unpack('<I', d.read(symbols['bm_debug_sequence'], 4))[0] + 1
            d.write(symbols['bm_debug_command'], data)
            d.write(symbols['bm_debug_sequence'], struct.pack('<I', seq))
            print(f'Queued command {seq}')
        if a.capture:
            # dos_present and dos_virtio_gpu both have a file-local g_scanout.
            # The former is null without SSAA; symbol order is not stable.
            pointers = {struct.unpack('<Q', d.read(address, 8))[0]
                        for address in scanout_symbols} - {0}
            if len(pointers) != 1:
                raise RuntimeError(f'Expected one active scanout address, found {len(pointers)}')
            ptr = pointers.pop()
            w = struct.unpack('<I', d.read(symbols['g_ow'], 4))[0]
            h = struct.unpack('<I', d.read(symbols['g_oh'], 4))[0]
            if not ptr or not (0 < w <= 8192 and 0 < h <= 8192):
                raise RuntimeError('Scanout not initialized')
            raw = a.capture.resolve().with_suffix('.rgba')
            raw.parent.mkdir(parents=True, exist_ok=True)
            raw.unlink(missing_ok=True) # Never mistake a previous capture for this dump.
            with socket.create_connection(('127.0.0.1', a.monitor_port), timeout=10) as s:
                s.recv(8192)
                s.sendall(f'pmemsave 0x{ptr:x} {w*h*4} {raw.as_posix()}\n'.encode())
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    if raw.exists() and raw.stat().st_size == w*h*4:
                        break
                    time.sleep(.05)
                else:
                    raise RuntimeError('Scanout memory save failed')
            rgba = raw.read_bytes()
            rgb = bytearray(w*h*3)
            for c in range(3):
                rgb[c::3] = rgba[c::4]
            write_png(a.capture, w, h, rgb)
            print(f'Captured {w}x{h}: {a.capture}')
    finally:
        d.close()


if __name__ == '__main__':
    main()
