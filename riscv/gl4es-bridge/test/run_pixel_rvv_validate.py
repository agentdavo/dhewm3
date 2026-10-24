#!/usr/bin/env python3
"""Compile the production ordinary-C swizzle with RV64GCV and test four VLENs."""
from pathlib import Path
import os, subprocess

root = Path(__file__).resolve().parents[3]
out = root / 'build-rv/gl4es-pixel-validation'
out.mkdir(parents=True, exist_ok=True)
os.environ['TMP'] = os.environ['TEMP'] = str(out)
os.environ['PATH'] = str(root/'build/toolchain/install/bin') + ';C:/msys64/ucrt64/bin;' + os.environ['PATH']
source = (root/'riscv/gl4es/src/gl/pixel.c').read_text()
helper = source[source.index('/* Typed contiguous indexing'):source.index('bool pixel_convert(')]
helper = helper.replace('static void pixel_swap_rb_row', '__attribute__((noinline, noclone)) static void pixel_swap_rb_row')
test = r'''
#include "libc/libc.h"
#include "boot/runtime.h"
typedef unsigned GLuint;
HELPER
static unsigned a[4100], b[4100], ref[4100];
static unsigned swap(unsigned v) {
    unsigned char *p=(unsigned char *)&v;
    unsigned char t=p[0];p[0]=p[2];p[2]=t;return v;
}
__attribute__((noinline, optimize("no-tree-vectorize")))
static void reference(const unsigned *in,unsigned *out,unsigned n) {
    for(unsigned j=0;j<n;++j)out[j]=swap(in[j]);
}
int kmain(void) {
    unsigned cases=0;
    for(unsigned n=0;n<=1025;++n) {
        for(unsigned i=0;i<4100;++i)a[i]=b[i]=ref[i]=i*1664525u+1013904223u;
        pixel_swap_rb_row(a+1,b+2,n);reference(a+1,ref+2,n);
        if(memcmp(b,ref,sizeof(b)))return printf("[pixel] FAIL disjoint %u\n",n),1;
        /* Both overlap directions and exact alias preserve forward semantics. */
        for(unsigned off=0;off<3;++off) {
            for(unsigned i=0;i<4100;++i)b[i]=ref[i]=a[i];
            pixel_swap_rb_row(b+1,b+off,n);reference(ref+1,ref+off,n);
            if(memcmp(b,ref,sizeof(b)))return printf("[pixel] FAIL overlap %u/%u\n",n,off),1;
        }
        ++cases;
    }
    for(unsigned pass=0;pass<6;++pass) {
        uint64_t start=timer_ticks();
        for(unsigned rep=0;rep<512;++rep) {
            if(pass&1)pixel_swap_rb_row(a,b,4096);else reference(a,b,4096);
            __asm__ volatile("" ::: "memory");
        }
        printf("[pixel-bench] candidate=%u pixels=2097152 ns=%llu\n",pass&1,
               (unsigned long long)timer_ns_from_ticks(timer_ticks()-start));
    }
    printf("[pixel] PASS %u sizes, disjoint and overlapping\n",cases);return 0;
}
'''.replace('HELPER', helper)
(out/'validate.c').write_text(test)
runtime = ['boot/start.S','boot/runtime.c','boot/sbi.c','boot/trap.c','boot/fdt.c',
           'libc/string.c','libc/printf.c','libc/malloc.c']
for v in (128,256,512,1024):
    elf = out/f'pixel-{v}.elf'
    cmd = [str(root/'build/toolchain/install/bin/riscv64-unknown-elf-gcc.exe'),
           f'-march=rv64gcv_zvl{v}b','-mabi=lp64d','-mcmodel=medany','-O2',
           '-ffreestanding','-ffunction-sections','-fdata-sections','-nostdlib',
           '-I'+str(root/'riscv'),'-Wl,--gc-sections',
           '-Wl,-T'+str(root/'riscv/boot/linker.ld'),
           '-fopt-info-vec-optimized='+str(out/f'vector-{v}.txt')]
    obj = out/f'pixel-{v}.o'
    subprocess.run(cmd + ['-c',str(out/'validate.c'),'-o',str(obj)],check=True)
    # Boot self-tests require RVA23; the actual production loop above uses GCV.
    cmd[1] = f'-march=rva23s64_zvl{v}b'
    subprocess.run(cmd + [str(obj)] + [str(root/'riscv'/p) for p in runtime] +
                   ['-lgcc','-o',str(elf)],check=True)
    p = subprocess.run(['C:/msys64/ucrt64/bin/qemu-system-riscv64.exe','-M','virt',
                        '-cpu',f'rva23s64,vlen={v}','-m','256M','-nographic',
                        '-bios','default','-kernel',str(elf)],capture_output=True,text=True,timeout=60)
    (out/f'run-{v}.log').write_text(p.stdout+p.stderr)
    print(v, '\n'.join(x for x in p.stdout.splitlines() if '[pixel' in x),flush=True)
    assert '[pixel] PASS' in p.stdout
