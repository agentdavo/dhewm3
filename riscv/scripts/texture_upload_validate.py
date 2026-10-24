#!/usr/bin/env python3
"""Windows/MSYS2: validate production uploads at all supported VLENs.

Uses the repo-local bare-metal toolchain; outputs stay in build-rv.
"""
from pathlib import Path
import os,subprocess,sys
root=Path(__file__).resolve().parents[2]
scalar='--scalar' in sys.argv
out=root/'build-rv'/('texture-upload-scalar' if scalar else 'texture-upload-validation');out.mkdir(parents=True,exist_ok=True)
os.environ['PATH']=str(root/'build/toolchain/install/bin')+';C:/msys64/ucrt64/bin;'+os.environ['PATH']
cc=root/'build/toolchain/install/bin/riscv64-unknown-elf-gcc.exe'
rt=['boot/start.S','boot/runtime.c','boot/sbi.c','boot/trap.c','boot/fdt.c','libc/string.c','libc/printf.c','libc/malloc.c','softgles2/src/sg_texture.c','softgles2/test/copy_texture_validate.c']
for v in [128,256,512,1024]:
 elf=out/f'upload-{v}.elf'
 cmd=[str(cc),f'-march=rva23s64_zvl{v}b','-mabi=lp64d','-mcmodel=medany','-O2','-ffreestanding','-fno-pic','-ffunction-sections','-fdata-sections','-DSGL_BAREMETAL','-I'+str(root/'riscv'),'-I'+str(root/'riscv/softgles2/src'),'-I'+str(root/'riscv/softgles2/include'),'-nostdlib','-Wl,--gc-sections','-Wl,-T'+str(root/'riscv/boot/linker.ld')]+[str(root/'riscv'/x) for x in rt]+['-lgcc','-o',str(elf)]
 if scalar: cmd.insert(1,'-DSGL_UPLOAD_RVV=0')
 r=subprocess.run(cmd,capture_output=True,text=True);(out/f'build-{v}.log').write_text(r.stdout+r.stderr)
 if r.returncode: raise RuntimeError(r.stderr[-1500:])
 r=subprocess.run(['C:/msys64/ucrt64/bin/qemu-system-riscv64.exe','-M','virt','-cpu',f'rva23s64,vlen={v}','-m','256M','-nographic','-bios','default','-kernel',str(elf)],capture_output=True,text=True,timeout=60)
 (out/f'run-{v}.log').write_text(r.stdout+r.stderr)
 print(v,[l for l in r.stdout.splitlines() if '[copy]' in l or '[upload-bench]' in l],flush=True)
 if '[copy] PASS' not in r.stdout: raise RuntimeError('validation failed')
