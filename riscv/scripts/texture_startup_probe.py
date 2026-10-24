#!/usr/bin/env python3
"""Windows four-hart texture startup probe: validate, serial, or parallel.

Uses build-rv/dhewm3 and doom-truecolour.img, VLEN1024, ports4468/4469.
Requires those ports/disk free. Stops at kmain, after BSS initialization,
sets diagnostic switches, then leaves QEMU running for capture or inspection.
Validation runs headless; measured modes use SDL. Quit via the monitor before
starting another run. Byte comparisons are disabled in measured modes.
"""
from pathlib import Path
import subprocess,sys,time,struct,socket,re
root=Path(__file__).resolve().parents[2];sys.path.insert(0,str(root/'riscv/scripts'))
from visual_probe import Debugger
mode=sys.argv[1] if len(sys.argv)>1 else 'validate'
if mode not in ('validate','serial','parallel'): raise SystemExit('Use validate, serial, or parallel')
label=sys.argv[2] if len(sys.argv)>2 else mode
if not re.fullmatch('[a-z0-9-]+',label): raise SystemExit('Label must use lowercase letters, digits or hyphens')
out=root/'build-rv'; log=out/f'doom-texture-{label}.log'
for required in ('dhewm3', 'doom-truecolour.img'):
    if not (out/required).is_file(): raise SystemExit(f'Missing {out/required}')
for port in (4468,4469):
    with socket.socket() as check:
        try: check.bind(('127.0.0.1',port))
        except OSError: raise SystemExit(f'Port {port} is occupied; quit the previous QEMU first')
q=['C:/msys64/ucrt64/bin/qemu-system-riscv64.exe','-S','-display','none' if mode=='validate' else 'sdl','-serial','file:'+log.as_posix(),'-machine','virt,acpi=off','-accel','tcg,thread=multi,tb-size=256','-cpu','rva23s64,vlen=1024,v=true,vext_spec=v1.0,elen=64,zbc=true,zbkb=true,zbkc=true,zbkx=true,zfh=true,zvfh=true,zvfbfmin=true,zvfbfwma=true,zvbc=true,zvkng=true,zvksg=true','-smp','4','-m','4G','-bios','default','-kernel',(out/'dhewm3').as_posix(),'-global','virtio-mmio.force-legacy=false','-netdev','user,id=net0','-device','virtio-net-device,netdev=net0','-device','virtio-rng-pci','-device','virtio-keyboard-device','-device','virtio-tablet-device','-drive',f'file={(out/"doom-truecolour.img").as_posix()},format=raw,id=hd0,if=none','-device','virtio-blk-device,drive=hd0','-device','virtio-gpu-device','-monitor','tcp:127.0.0.1:4468,server,nowait','-gdb','tcp:127.0.0.1:4469']
if mode == 'parallel': q.remove('-S')
p=subprocess.Popen(q,stdout=(out/f'qemu-{label}.stdout').open('w'),stderr=(out/f'qemu-{label}.stderr').open('w'))
(out/'texture-qemu.pid').write_text(str(p.pid))
time.sleep(1)
if p.poll() is not None:
    raise SystemExit(f'QEMU failed; inspect {out / ("qemu-"+label+".stderr")}')
if mode == 'parallel':
    print('Running',mode,'pid',p.pid,flush=True)
    raise SystemExit(0)
nm=root/'build/toolchain/install/bin/riscv64-unknown-elf-nm.exe'
symbols={f[2]:int(f[0],16) for l in subprocess.check_output([str(nm),str(out/'dhewm3')],text=True).splitlines() if len(f:=l.split())==3}
d=Debugger(4469,4468)
addr=symbols['kmain']
assert d.packet(f'Z0,{addr:x},4')=='OK'
print('break',d.packet('c'),flush=True)
assert d.packet(f'z0,{addr:x},4')=='OK'
d.write(symbols['bm_texture_validate'],struct.pack('<i',mode=='validate'))
d.write(symbols['bm_texture_parallel'],struct.pack('<i',mode!='serial'))
d.close()
print('Running',mode,'pid',p.pid,flush=True)
