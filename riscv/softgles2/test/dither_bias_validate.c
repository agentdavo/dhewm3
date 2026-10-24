/* Include production implementation; unused raster entry points are GC-linked. */
#include "../src/sg_raster.c"
#include "boot/runtime.h"
static float reference[128], actual[128];
int kmain(void) {
    unsigned cases=0;size_t max=__riscv_vsetvlmax_e32m2();
    if(max>128)max=128;
    for(int enabled=0;enabled<2;++enabled) {
        sgl_dither_enabled=enabled;
        for(int y=-4;y<8;++y)for(int x=-4;x<8;++x)for(size_t n=1;n<=max;++n) {
            for(int i=0;i<128;++i)reference[i]=actual[i]=-99.f;
            sgl_dither_table_fast=0;
            __riscv_vse32_v_f32m2(reference,sgl_store_bias_m2(x,y,n),n);
            sgl_dither_table_fast=1;
            __riscv_vse32_v_f32m2(actual,sgl_store_bias_m2(x,y,n),n);
            if(memcmp(reference,actual,sizeof(actual)))return printf("[dither] FAIL\n"),1;
            for(size_t i=0;i<n;++i) {
                float want=enabled?(sgl_bayer4[((y&3)*4)+((x+(int)i)&3)]+.5f)/16.f:.5f;
                if(actual[i]!=want)return printf("[dither] oracle FAIL\n"),1;
            }
            ++cases;
        }
    }
    sgl_dither_enabled=1;
    for(int round=0;round<4;++round)for(int order=0;order<2;++order) {
        sgl_dither_table_fast=order^(round&1);
        uint64_t start=timer_ticks();
        for(int i=0;i<20000;++i) {
            __riscv_vse32_v_f32m2(actual,sgl_store_bias_m2(i,i>>2,max),max);
            __asm__ volatile("" ::: "memory");
        }
        printf("[dither-bench] table=%d ns=%llu\n",sgl_dither_table_fast,
            (unsigned long long)timer_ns_from_ticks(timer_ticks()-start));
    }
    printf("[dither] PASS %u cases\n",cases);return 0;
}
