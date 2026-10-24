#!/usr/bin/env python3
"""Isolated S3TC code-generation experiments; never edits production sources."""
from pathlib import Path
import argparse, json, os, re, statistics, subprocess, time
from rvv_doom_matrix import qemu_run

root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--wait-for',type=Path)
p.add_argument('--runs',type=int,default=4)
a=p.parse_args()
if a.wait_for:
    deadline=time.monotonic()+900
    while time.monotonic()<deadline:
        if a.wait_for.exists() and len(json.loads(a.wait_for.read_text()))>=a.runs: break
        time.sleep(2)
    else: raise TimeoutError('Boot comparison has not finished')
    time.sleep(3)
out=root/'build-rv/dds-decode-experiment';out.mkdir(parents=True,exist_ok=True)
os.environ['PATH']=str(root/'build/toolchain/install/bin')+';C:/msys64/ucrt64/bin;'+os.environ['PATH']
os.environ['TMP']=os.environ['TEMP']=str(out)
s=(root/'riscv/softgles2/src/sg_texture.c').read_text()
# Reconstruct the unrestricted compiler/batch baseline even after the measured
# scalar policy has been adopted. Only these isolated copies are changed.
s=s.replace('static DXT_SMALL_SCALAR ', 'static ')
s=s.replace('const int use_batch = sgl_dxt_vector_decode && ', 'const int use_batch = ')
(out/'baseline.c').write_text(s)
start=s.index('static uint32_t dxt_color_palette(')
end=s.index('#if SG_TEX_RVV && SGL_DXT_RVV',start)
region=s[start:end]
for declaration in ('static uint32_t dxt_color_palette','static uint64_t dxt_alpha_palette','static void decode_dxt_block'):
    region=region.replace(declaration,'__attribute__((optimize("no-tree-vectorize")))\n'+declaration)
(out/'noautovec.c').write_text(s[:start]+region+s[end:])
start=s.index('static void decode_dxt_block(')
end=s.index('\n#if SG_TEX_RVV && SGL_DXT_RVV',start)
function='''static void decode_dxt_block(GLenum format, const uint8_t *block, uint32_t out[16])
{
    int kind=(int)(format-GL_COMPRESSED_RGB_S3TC_DXT1_EXT);
    uint32_t pal[4],alpha[8];
    uint32_t code=dxt_color_palette(block+(kind>=2?8:0),kind<2?kind:2,pal);
    if(kind<2) {
        for(int i=0;i<16;++i,code>>=2) out[i]=pal[code&3u];
    } else if(kind==2) {
        uint64_t bits=(uint64_t)rd32le(block)|((uint64_t)rd32le(block+4)<<32);
        for(int i=0;i<16;++i,code>>=2,bits>>=4)
            out[i]=(pal[code&3u]&0xffffffu)|((uint32_t)((bits&15u)*17)<<24);
    } else {
        uint64_t bits=dxt_alpha_palette(block,alpha);
        for(int i=0;i<16;++i,code>>=2,bits>>=3)
            out[i]=(pal[code&3u]&0xffffffu)|(alpha[bits&7u]<<24);
    }
}
'''
(out/'hoisted.c').write_text(s[:start]+function+s[end:])
variants={'rvv':'baseline','scalar':'baseline','noautovec':'noautovec','hoisted':'hoisted'}
runtime=['boot/start.S','boot/runtime.c','boot/sbi.c','boot/trap.c','boot/fdt.c',
         'libc/string.c','libc/printf.c','libc/malloc.c','softgles2/test/dds_texture_validate.c']
results=[]
for v in (128,256,512,1024):
    for mode,source in variants.items():
        elf=out/f'{mode}-{v}.elf'
        cmd=[str(root/'build/toolchain/install/bin/riscv64-unknown-elf-gcc.exe'),
             f'-march=rva23s64_zvl{v}b','-mabi=lp64d','-mcmodel=medany','-O2',
             '-ffreestanding','-fno-pic','-ffunction-sections','-fdata-sections','-DSGL_BAREMETAL',
             '-I'+str(root/'riscv'),'-I'+str(root/'riscv/softgles2/src'),
             '-I'+str(root/'riscv/softgles2/include'),'-nostdlib','-Wl,--gc-sections',
             '-Wl,-T'+str(root/'riscv/boot/linker.ld'),str(out/(source+'.c'))]
        if mode!='rvv':cmd+=['-DSGL_DXT_RVV=0']
        cmd += [str(root/'riscv'/f) for f in runtime]+['-lgcc','-o',str(elf)]
        r=subprocess.run(cmd,capture_output=True,text=True)
        (out/f'{mode}-{v}-build.log').write_text(r.stdout+r.stderr)
        if r.returncode:raise RuntimeError(r.stderr)
    for repeat in range(3):
        order=list(variants)
        if repeat%2:order.reverse()
        for mode in order:
            cmd=['C:/msys64/ucrt64/bin/qemu-system-riscv64.exe','-M','virt','-cpu',
                 f'rva23s64,vlen={v}','-m','256M','-nographic','-bios','default',
                 '-kernel',str(out/f'{mode}-{v}.elf')]
            r=qemu_run(cmd,60,2);text=r.stdout.decode(errors='replace')
            (out/f'{mode}-{v}-{repeat}.log').write_text(text)
            if '[dds] PASS 601 cases' not in text:raise RuntimeError('DDS validation failed')
            for kind,ns in re.findall(r'\[dds-bench\] kind=(\d+) pixels=524288 ns=(\d+)',text):
                results.append(dict(vlen=v,mode=mode,repeat=repeat,kind=int(kind),ns=int(ns)))
    for mode in variants:
        medians=[statistics.median(r['ns'] for r in results if r['vlen']==v and r['mode']==mode and r['kind']==k) for k in range(4)]
        print(v,mode,medians,flush=True)
    (out/'results.json').write_text(json.dumps(results,indent=2))
