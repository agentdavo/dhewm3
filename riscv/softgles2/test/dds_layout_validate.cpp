#include "../../../neo/renderer/DdsLayout.h"
#include <vector>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

static void wr(std::vector<unsigned char> &b, unsigned off, unsigned value) {
    for(unsigned i=0;i<4;++i)b[off+i]=(unsigned char)(value>>(i*8));
}
static std::vector<unsigned char> make(unsigned w,unsigned h,unsigned levels,unsigned faces,unsigned fourcc) {
    size_t size=128;unsigned bytes=fourcc==0x31545844?8:16;
    for(unsigned f=0;f<faces;++f)for(unsigned m=0,x=w,y=h;m<levels;++m,x=x>1?x/2:1,y=y>1?y/2:1)size+=((x+3)/4)*((y+3)/4)*bytes;
    std::vector<unsigned char>b(size,0);
    wr(b,0,0x20534444);wr(b,4,124);wr(b,8,0x21007);wr(b,12,h);wr(b,16,w);wr(b,28,levels);
    wr(b,76,32);wr(b,80,5);wr(b,84,fourcc);wr(b,108,0x401008);wr(b,112,faces==6?0xfe00:0);
    return b;
}
#define CHECK(x) do { ++cases; if(!(x)){std::printf("[dds-layout] FAIL line=%d\n",__LINE__);return 1;} }while(0)
int main(int argc,char **argv) {
    unsigned cases=0;ddsLayout_t d;
    if(argc>1 && !strcmp(argv[1],"--stream")) {
#ifdef _WIN32
        // Binary stdin: the test stream contains arbitrary DDS pixel bytes.
        _setmode(_fileno(stdin),_O_BINARY);
#endif
        unsigned char length[4];unsigned failures=0;
        while(std::fread(length,1,4,stdin)==4) {
            unsigned n=DDS_Read32(length);std::vector<unsigned char>b(n);
            if(std::fread(b.data(),1,n,stdin)!=n)return 2;
            if(!DDS_ParseLayout(b.data(),n,d)){++failures;std::printf("[dds-layout] invalid record %u\n",cases);}
            ++cases;
        }
        std::printf("[dds-layout] assets=%u failures=%u\n",cases,failures);return failures?1:0;
    }
    const unsigned formats[]={0x31545844,0x33545844,0x35545844,0x42475852};
    for(unsigned f=0;f<4;++f) {
        std::vector<unsigned char>b=make(257,129,9,1,formats[f]);
        CHECK(DDS_ParseLayout(b.data(),b.size(),d));CHECK(d.levels==9 && d.level[0][8].width==1 && d.level[0][8].height==1);
        CHECK(d.level[0][8].offset+d.level[0][8].size==b.size());
        CHECK(d.rxgb==(f==3));CHECK(!DDS_ParseLayout(b.data(),b.size()-1,d));
        b=make(8,8,4,6,formats[f]);CHECK(DDS_ParseLayout(b.data(),b.size(),d));
        CHECK(d.faces==6 && d.level[5][3].offset+d.level[5][3].size==b.size());
        wr(b,112,0x600);CHECK(!DDS_ParseLayout(b.data(),b.size(),d));
    }
    std::vector<unsigned char>b=make(4,4,1,1,0x31545844),good=b;
    for(size_t n=0;n<b.size();++n) CHECK(!DDS_ParseLayout(b.data(),n,d));
    const unsigned offsets[]={0,4,76,12,16,28};
    for(unsigned i=0;i<6;++i){b=good;wr(b,offsets[i],0);CHECK(!DDS_ParseLayout(b.data(),b.size(),d));}
    b=good;wr(b,28,17);CHECK(!DDS_ParseLayout(b.data(),b.size(),d));
    b=good;wr(b,84,0x30315844);CHECK(!DDS_ParseLayout(b.data(),b.size(),d)); // truncated DX10
    b=good;b.resize(156);wr(b,84,0x30315844);wr(b,128,71);wr(b,132,3);wr(b,140,1);
    CHECK(DDS_ParseLayout(b.data(),b.size(),d));wr(b,140,2);CHECK(!DDS_ParseLayout(b.data(),b.size(),d));
    // Padded BGR24 rows, channel order and padding must not leak into pixels.
    b=good;b.resize(152);wr(b,8,0x100f);wr(b,12,2);wr(b,16,3);wr(b,20,12);
    wr(b,80,64);wr(b,84,0);wr(b,88,24);wr(b,92,0xff0000);wr(b,96,0xff00);wr(b,100,0xff);
    for(unsigned i=128;i<b.size();++i)b[i]=(unsigned char)i;
    CHECK(DDS_ParseLayout(b.data(),b.size(),d));unsigned char out[24];
    DDS_ExpandRGBA(b.data()+128,d,d.level[0][0],out,NULL);
    CHECK(out[0]==130 && out[1]==129 && out[2]==128 && out[3]==255 && out[12]==142);
    wr(b,96,0xff0000);CHECK(!DDS_ParseLayout(b.data(),b.size(),d)); // overlapping masks
    b=good;b.resize(130);wr(b,8,0x1007);wr(b,12,1);wr(b,16,1);wr(b,80,64);wr(b,84,0);wr(b,88,16);
    wr(b,92,0xf800);wr(b,96,0x7e0);wr(b,100,31);b[128]=0xe0;b[129]=7;
    CHECK(DDS_ParseLayout(b.data(),b.size(),d));DDS_ExpandRGBA(b.data()+128,d,d.level[0][0],out,NULL);
    CHECK(out[0]==0 && out[1]==255 && out[2]==0 && out[3]==255);
    wr(b,80,1);wr(b,88,8);b.resize(129);b[128]=73;
    CHECK(DDS_ParseLayout(b.data(),b.size(),d));DDS_ExpandRGBA(b.data()+128,d,d.level[0][0],out,NULL);
    CHECK(out[0]==0 && out[3]==73);
    unsigned char palette[768];for(unsigned i=0;i<768;++i)palette[i]=(unsigned char)i;
    wr(b,80,0x10000040);CHECK(DDS_ParseLayout(b.data(),b.size(),d));DDS_ExpandRGBA(b.data()+128,d,d.level[0][0],out,palette);
    CHECK(out[0]==219 && out[1]==220 && out[2]==221 && out[3]==255);
    std::printf("[dds-layout] PASS %u cases\n",cases);return 0;
}
