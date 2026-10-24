#!/usr/bin/env python3
"""Fresh, sequential Doom boots: source textures versus legacy/native DDS.

Uses one ELF and read-only backing disk with temporary snapshot writes inside
build-rv. Stops after first-map-present; logs retain the inclusive texture timers.
No saved game/configuration or normal startup defaults are changed.
"""
from pathlib import Path
import argparse, hashlib, json, os, re, socket, struct, subprocess, time
from visual_probe import Debugger

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('modes', nargs='*', default=['source','legacy','native','source'])
parser.add_argument('--label', default='dds-boot-ab')
parser.add_argument('--timeout', type=int, default=600)
parser.add_argument('--harts', type=int, choices=(1,2,4,8), default=4)
parser.add_argument('--host-cpus', nargs='+', type=int, help='Restrict QEMU to these host logical CPUs (optional controlled benchmark)')
parser.add_argument('--vcpu-cpus', nargs='+', type=int, help='Pin only guest CPU threads, one host CPU per hart; leave helper threads unrestricted')
parser.add_argument('--elf', type=Path, default=root/'build-rv/dhewm3', help='Engine ELF to measure')
parser.add_argument('--hold-seconds', type=int, default=0, help='Keep guest running after measurement for optional visual review')
parser.add_argument('--sample-first-frame', action='store_true', help='Diagnostic PC/return-address samples on all harts; not a timing run')
parser.add_argument('--sample-stacks', action='store_true', help='Also walk frame-pointer chains; diagnostic only')
args = parser.parse_args()
if args.sample_stacks: args.sample_first_frame = True
if args.host_cpus and args.vcpu_cpus: parser.error('choose process or vCPU affinity, not both')
if args.vcpu_cpus and (len(args.vcpu_cpus)!=args.harts or len(set(args.vcpu_cpus))!=args.harts):
    parser.error('provide one distinct host CPU per hart')
if any(c < 0 or c >= min(os.cpu_count() or 1,64) for c in (args.host_cpus or args.vcpu_cpus or [])):
    parser.error('host CPU index outside supported affinity mask')
if any(m not in ('source','legacy','native') for m in args.modes):
    parser.error('modes: source, legacy, native')
if not re.fullmatch('[a-z0-9-]+',args.label): parser.error('invalid label')
out = root/'build-rv'/args.label
out.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
env['TMP'] = env['TEMP'] = env['TMPDIR'] = str(out)
elf = args.elf.resolve()
elf_sha256 = hashlib.sha256(elf.read_bytes()).hexdigest()
nm = root/'build/toolchain/install/bin/riscv64-unknown-elf-nm.exe'
symbols = {f[2]:int(f[0],16) for line in subprocess.check_output([str(nm),str(elf)],text=True).splitlines()
           if len(f:=line.split())==3}
for name in ('kmain','bm_texture_boot_dds','gl4es_native_s3tc'):
    if name not in symbols: raise RuntimeError(f'Missing {name}; rebuild the engine')
monitor_port, gdb_port = 4478,4479
for port in (monitor_port,gdb_port):
    with socket.socket() as s: s.bind(('127.0.0.1',port))
cpu = 'rva23s64,vlen=1024,v=true,vext_spec=v1.0,elen=64,zbc=true,zbkb=true,zbkc=true,zbkx=true,zfh=true,zvfh=true,zvfbfmin=true,zvfbfwma=true,zvbc=true,zvkng=true,zvksg=true'
results=[]
for run, mode in enumerate(args.modes):
    log=out/f'{run+1}-{mode}.log'
    cmd=['C:/msys64/ucrt64/bin/qemu-system-riscv64.exe','-S','-display','none',
         '-serial','file:'+log.as_posix(),'-machine','virt,acpi=off',
         '-accel','tcg,thread=multi,tb-size=256','-cpu',cpu,'-smp',str(args.harts),'-m','4G',
         '-bios','default','-kernel',str(elf),'-global','virtio-mmio.force-legacy=false',
         '-netdev','user,id=net0','-device','virtio-net-device,netdev=net0',
         '-device','virtio-rng-pci','-device','virtio-keyboard-device',
         '-device','virtio-tablet-device','-snapshot',
         '-drive',f'file={(root/"build-rv/doom-truecolour.img").as_posix()},format=raw,id=hd0,if=none',
         '-device','virtio-blk-device,drive=hd0','-device','virtio-gpu-device',
         '-monitor',f'tcp:127.0.0.1:{monitor_port},server,nowait',
         '-gdb',f'tcp:127.0.0.1:{gdb_port}']
    with (out/f'{run+1}-{mode}.stderr').open('w') as errors:
        p=subprocess.Popen(cmd,stdout=errors,stderr=errors,env=env)
        print(f'START {run+1} {mode} pid={p.pid}',flush=True)
        try:
            if args.host_cpus:
                if os.name == 'nt':
                    import ctypes
                    from ctypes import wintypes
                    k=ctypes.WinDLL('kernel32',use_last_error=True)
                    k.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]
                    k.OpenProcess.restype=wintypes.HANDLE
                    k.SetProcessAffinityMask.argtypes=[wintypes.HANDLE,ctypes.c_size_t]
                    k.CloseHandle.argtypes=[wintypes.HANDLE]
                    handle=k.OpenProcess(0x0200,False,p.pid)
                    if not handle: raise ctypes.WinError(ctypes.get_last_error())
                    try:
                        if not k.SetProcessAffinityMask(handle,sum(1<<c for c in set(args.host_cpus))):
                            raise ctypes.WinError(ctypes.get_last_error())
                    finally: k.CloseHandle(handle)
                else: os.sched_setaffinity(p.pid,set(args.host_cpus))
                print(f'HOST CPUS: {args.host_cpus}',flush=True)
            time.sleep(1)
            if p.poll() is not None: raise RuntimeError('QEMU failed to start')
            if args.vcpu_cpus:
                with socket.create_connection(('127.0.0.1',monitor_port),timeout=10) as monitor:
                    monitor.recv(8192);monitor.sendall(b'info cpus\n');reply=''
                    while True:
                        reply+=monitor.recv(16384).decode(errors='replace')
                        cpu_threads={int(i):int(t) for i,t in re.findall(r'CPU #(\d+): thread_id=(\d+)',reply)}
                        if len(cpu_threads)==args.harts: break
                if os.name=='nt':
                    import ctypes
                    from ctypes import wintypes
                    k=ctypes.WinDLL('kernel32',use_last_error=True)
                    k.OpenThread.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]
                    k.OpenThread.restype=wintypes.HANDLE
                    k.SetThreadAffinityMask.argtypes=[wintypes.HANDLE,ctypes.c_size_t]
                    k.SetThreadAffinityMask.restype=ctypes.c_size_t
                    k.CloseHandle.argtypes=[wintypes.HANDLE]
                for hart,host_cpu in enumerate(args.vcpu_cpus):
                    tid=cpu_threads[hart]
                    if os.name=='nt':
                        handle=k.OpenThread(0x60,False,tid)  # query + set information
                        if not handle: raise ctypes.WinError(ctypes.get_last_error())
                        try:
                            if not k.SetThreadAffinityMask(handle,1<<host_cpu): raise ctypes.WinError(ctypes.get_last_error())
                        finally:k.CloseHandle(handle)
                    else:os.sched_setaffinity(tid,{host_cpu})
                    print(f'AFFINITY hart={hart} host_thread={tid} host_cpu={host_cpu}',flush=True)
            d=Debugger(gdb_port,monitor_port)
            address=symbols['kmain']
            assert d.packet(f'Z0,{address:x},4')=='OK'
            d.packet('c')
            assert d.packet(f'z0,{address:x},4')=='OK'
            d.write(symbols['bm_texture_boot_dds'],struct.pack('<i',mode!='source'))
            d.write(symbols['gl4es_native_s3tc'],struct.pack('<i',mode!='legacy'))
            d.close()
            start=time.monotonic();seen=0;samples=[]
            while time.monotonic()-start < args.timeout:
                text=log.read_text(errors='replace') if log.exists() else ''
                stages=re.findall(r'\[boot-time\] total_ms=(\d+) delta_ms=(\d+) stage=(\S+)',text)
                for total,delta,stage in stages[seen:]:
                    print(f'{mode}: {stage} {total} ms',flush=True)
                seen=len(stages)
                if any(stage=='first-map-present' for _,_,stage in stages): break
                if p.poll() is not None: raise RuntimeError('QEMU exited before first frame')
                if args.sample_first_frame and any(s=='map-ready' for _,_,s in stages):
                    d=Debugger(gdb_port,monitor_port)
                    d.s.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
                    threads=[];reply=d.packet('qfThreadInfo')
                    while reply.startswith('m'):
                        threads.extend(reply[1:].split(','));reply=d.packet('qsThreadInfo')
                    for thread in threads:
                        d.packet('Hg'+thread)
                        pc=int.from_bytes(bytes.fromhex(d.packet('p20')),'little')
                        ra=int.from_bytes(bytes.fromhex(d.packet('p1')),'little')
                        sample=dict(thread=thread,pc=pc,ra=ra)
                        if args.sample_stacks:
                            fp=int.from_bytes(bytes.fromhex(d.packet('p8')),'little')
                            stack=[]
                            for depth in range(32):
                                if fp & 15 or not 0x80000010 <= fp < 0x180000000: break
                                try: previous,caller=struct.unpack('<QQ',d.read(fp-16,16))
                                except (ValueError,struct.error): break
                                if not 0x80000000 <= caller < 0x90000000: break
                                stack.append(caller)
                                if not fp < previous <= fp+16*1024*1024: break
                                fp=previous
                            sample['stack']=stack
                        samples.append(sample)
                    d.close()
                    time.sleep(.15)
                else: time.sleep(2)
            else: raise TimeoutError(f'{mode} boot did not complete')
            # Let the synchronous serial texture report finish before pausing.
            time.sleep(.5)
            d=Debugger(gdb_port,monitor_port)
            counters={name:struct.unpack('<Q',d.read(symbols[name],8))[0]
                      for name in ('sgl_dbg_dds_uploads','sgl_dbg_dds_mips','sgl_dbg_dds_bytes')}
            d.close()
            text=log.read_text(errors='replace')
            result={'mode':mode,'run':run+1,'harts':args.harts,'host_cpus':args.host_cpus,'vcpu_cpus':args.vcpu_cpus,'elf':str(elf),'elf_sha256':elf_sha256,
                    'sampled':args.sample_first_frame,'stages':{s:int(t) for t,_,s in stages},
                    'dds':counters,'log':str(log)}
            workers=re.search(r'\[sgl-mt\] tile-parallel raster: (\d+) harts',text)
            result['renderer_harts']=int(workers[1]) if workers else None
            first_frame=re.search(r'frame:0 all:.*?\n.*?(views:[^\r\n]+)',text,re.S)
            result['first_frame_workload']=first_frame[1] if first_frame else None
            sections=text.split('[texture-time] phase=')
            result['texture_reports']={s.splitlines()[0]:{
                name:{'calls':int(c),'us':int(u),'bytes':int(b)}
                for name,c,u,b in re.findall(r'bucket=(\S+) calls=(\d+) us=(\d+) bytes=(\d+)',s)}
                for s in sections[1:]}
            results.append(result)
            if args.sample_first_frame:
                (out/f'{run+1}-{mode}-samples.json').write_text(json.dumps(samples))
            (out/'results.json').write_text(json.dumps(results,indent=2))
            print('DONE '+json.dumps(result['stages'])+' '+json.dumps(counters),flush=True)
            if args.hold_seconds:
                print(f'REVIEW: guest available for {args.hold_seconds} seconds', flush=True)
                deadline=time.monotonic()+args.hold_seconds
                while p.poll() is None and time.monotonic()<deadline: time.sleep(1)
        finally:
            try:
                with socket.create_connection(('127.0.0.1',monitor_port),timeout=2) as s:
                    s.recv(8192);s.sendall(b'quit\n');time.sleep(.5)
                p.wait(timeout=5)
            except (OSError,subprocess.TimeoutExpired):
                p.kill();p.wait()
            time.sleep(1)
print(f'Results: {out / "results.json"}',flush=True)
