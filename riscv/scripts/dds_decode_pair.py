#!/usr/bin/env python3
"""Compare two texture objects in one QEMU process, alternating order.

Run dds_decode_ab.py first to generate the isolated baseline/noautovec sources.
"""
from pathlib import Path
import json, os, re, statistics, subprocess
from rvv_doom_matrix import qemu_run
root=Path(__file__).resolve().parents[2]
out=root/'build-rv/dds-decode-experiment'
os.environ['TMP']=os.environ['TEMP']=str(out)
os.environ['PATH']=str(root/'build/toolchain/install/bin')+';C:/msys64/ucrt64/bin;'+os.environ['PATH']
prefix=str(root/'build/toolchain/install/bin/riscv64-unknown-elf-')
harness=r'''
#define kmain validate_candidate
#include "softgles2/test/dds_texture_validate.c"
#undef kmain
extern void base_glGenTextures(GLsizei,GLuint *);
extern void base_glBindTexture(GLenum,GLuint);
extern void base_glCompressedTexImage2D(GLenum,GLint,GLenum,GLsizei,GLsizei,GLint,GLsizei,const void*);
extern void base_glCompressedTexSubImage2D(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLsizei,const void*);
extern tex_obj base_g_textures[MAX_TEXTURES];
static unsigned char pair_data[16384];
static const GLenum pair_formats[]={GL_COMPRESSED_RGB_S3TC_DXT1_EXT,GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
    GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,GL_COMPRESSED_RGBA_S3TC_DXT5_EXT};
int kmain(void) {
    if(validate_candidate())return 1;
    for(unsigned i=0;i<sizeof(pair_data);++i)pair_data[i]=random_byte();
    GLuint ids[2];glGenTextures(1,ids);base_glGenTextures(1,ids+1);
    glBindTexture(GL_TEXTURE_2D,ids[0]);base_glBindTexture(GL_TEXTURE_2D,ids[1]);
    for(int k=0;k<4;++k) {
        int bytes=1024*(k<2?8:16);
        glCompressedTexImage2D(GL_TEXTURE_2D,0,pair_formats[k],128,128,0,bytes,pair_data);
        base_glCompressedTexImage2D(GL_TEXTURE_2D,0,pair_formats[k],128,128,0,bytes,pair_data);
        if(memcmp(g_textures[ids[0]].tex.texels,base_g_textures[ids[1]].tex.texels,128*128*4))
            return printf("[pair] FAIL mismatch\n"),1;
        for(int rep=0;rep<9;++rep) for(int order=0;order<2;++order) {
            int candidate=order^(rep&1);
            uint64_t start=timer_ticks();
            for(int n=0;n<128;++n) {
                if(candidate)glCompressedTexSubImage2D(GL_TEXTURE_2D,0,0,0,128,128,pair_formats[k],bytes,pair_data);
                else base_glCompressedTexSubImage2D(GL_TEXTURE_2D,0,0,0,128,128,pair_formats[k],bytes,pair_data);
            }
            if(rep)printf("[pair] kind=%d candidate=%d ns=%llu\n",k,candidate,
                (unsigned long long)timer_ns_from_ticks(timer_ticks()-start));
        }
    }
    printf("[pair] PASS\n");return 0;
}
'''
(out/'pair.c').write_text(harness)
runtime=['boot/start.S','boot/runtime.c','boot/sbi.c','boot/trap.c','boot/fdt.c',
         'libc/string.c','libc/printf.c','libc/malloc.c']
results=[]
for v in (128,256,512,1024):
    flags=[f'-march=rva23s64_zvl{v}b','-mabi=lp64d','-mcmodel=medany','-O2','-ffreestanding',
           '-ffunction-sections','-fdata-sections','-DSGL_BAREMETAL','-I'+str(root/'riscv'),
           '-I'+str(root/'riscv/softgles2/src'),'-I'+str(root/'riscv/softgles2/include')]
    for name,source,extra in [('base','baseline',[]),('candidate','noautovec',['-DSGL_DXT_RVV=0'])]:
        subprocess.run([prefix+'gcc.exe']+flags+extra+['-c',str(out/(source+'.c')),'-o',str(out/(name+'.o'))],check=True)
    defined=subprocess.check_output([prefix+'nm.exe','-g','--defined-only',str(out/'base.o')],text=True)
    (out/'rename.txt').write_text('\n'.join(f'{x.split()[-1]} base_{x.split()[-1]}' for x in defined.splitlines() if len(x.split())==3)+'\n')
    subprocess.run([prefix+'objcopy.exe','--redefine-syms='+str(out/'rename.txt'),str(out/'base.o')],check=True)
    elf=out/f'pair-{v}.elf'
    subprocess.run([prefix+'gcc.exe']+flags+['-nostdlib','-Wl,--gc-sections',
        '-Wl,-T'+str(root/'riscv/boot/linker.ld'),str(out/'pair.c'),str(out/'base.o'),str(out/'candidate.o')]+
        [str(root/'riscv'/x) for x in runtime]+['-lgcc','-o',str(elf)],check=True)
    r=qemu_run(['C:/msys64/ucrt64/bin/qemu-system-riscv64.exe','-M','virt','-cpu',f'rva23s64,vlen={v}',
               '-m','256M','-nographic','-bios','default','-kernel',str(elf)],120,2)
    text=r.stdout.decode(errors='replace');(out/f'pair-{v}.log').write_text(text)
    if '[pair] PASS' not in text:raise RuntimeError(text[-2000:])
    for k,c,n in re.findall(r'\[pair\] kind=(\d+) candidate=(\d+) ns=(\d+)',text):
        results.append(dict(vlen=v,kind=int(k),candidate=int(c),ns=int(n)))
    for c in (0,1):
        print(v,c,[statistics.median(x['ns'] for x in results if x['vlen']==v and x['kind']==k and x['candidate']==c) for k in range(4)],flush=True)
    (out/'paired-results.json').write_text(json.dumps(results,indent=2))

