/* Bounds-checked DDS layout shared by the engine and standalone validation.
 * DDS bytes are little-endian; never cast untrusted headers to native structs. */
#ifndef DHEWM3_DDS_LAYOUT_H
#define DHEWM3_DDS_LAYOUT_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

enum ddsFormat_t { DDS_BC1_RGB, DDS_BC1_RGBA, DDS_BC2, DDS_BC3,
    DDS_BC7, DDS_RGB, DDS_ALPHA, DDS_LUMINANCE, DDS_INDEX };
struct ddsLevel_t { size_t offset, size, pitch; unsigned width, height; };
struct ddsLayout_t {
    ddsFormat_t format;
    unsigned width, height, levels, faces, bits, masks[4];
    bool rxgb;
    ddsLevel_t level[6][16];
};
static inline uint32_t DDS_Read32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static inline bool DDS_MaskValid(uint32_t mask, unsigned bits) {
    if (!mask) return true;
    if (bits < 32 && (mask >> bits)) return false;
    while (!(mask & 1)) mask >>= 1;
    return (mask & (mask + 1)) == 0;
}
static inline bool DDS_ParseLayout(const void *buffer, size_t len, ddsLayout_t &out) {
    if (!buffer || len < 128) return false;
    const unsigned char *p = (const unsigned char *)buffer;
    if (DDS_Read32(p) != 0x20534444 || DDS_Read32(p+4) != 124 || DDS_Read32(p+76) != 32) return false;
    memset(&out, 0, sizeof(out));
    unsigned flags = DDS_Read32(p+8), pf = DDS_Read32(p+80), fourcc = DDS_Read32(p+84);
    unsigned caps2 = DDS_Read32(p+112), depth = DDS_Read32(p+24);
    out.width = DDS_Read32(p+16); out.height = DDS_Read32(p+12);
    if (!out.width || !out.height || out.width > 32768 || out.height > 32768 ||
        (caps2 & 0x200000) || depth > 1) return false; // volumes are not Doom 3 textures
    out.faces = (caps2 & 0x200) ? 6 : 1;
    if (out.faces == 6 && ((caps2 & 0xfc00) != 0xfc00 || out.width != out.height)) return false;
    out.levels = (flags & 0x20000) ? DDS_Read32(p+28) : 1;
    unsigned maxLevels = 1, dim = out.width > out.height ? out.width : out.height;
    while (dim > 1) { dim >>= 1; ++maxLevels; }
    if (!out.levels || out.levels > maxLevels || out.levels > 16) return false;
    out.bits = DDS_Read32(p+88);
    for (unsigned i=0;i<4;++i) out.masks[i] = DDS_Read32(p+92+i*4);
    unsigned blockBytes = 0;
    size_t start = 128;
    if (pf & 4) {
        switch (fourcc) {
        case 0x31545844: out.format = (pf & 1) ? DDS_BC1_RGBA : DDS_BC1_RGB; blockBytes=8; break;
        case 0x33545844: out.format = DDS_BC2; blockBytes=16; break;
        case 0x42475852: out.rxgb=true; out.format=DDS_BC3; blockBytes=16; break; // RXGB: X already in alpha
        case 0x35545844: out.format = DDS_BC3; blockBytes=16; break;
        case 0x30374342: case 0x4c374342: out.format=DDS_BC7; blockBytes=16; break;
        case 0x30315844: {
            if (len < 148 || DDS_Read32(p+132) != 3 || DDS_Read32(p+140) != 1) return false;
            unsigned dxgi = DDS_Read32(p+128);
            if (dxgi == 71) { out.format=DDS_BC1_RGBA; blockBytes=8; }
            else if (dxgi == 74) { out.format=DDS_BC2; blockBytes=16; }
            else if (dxgi == 77) { out.format=DDS_BC3; blockBytes=16; }
            else if (dxgi == 98) { out.format=DDS_BC7; blockBytes=16; }
            else return false;
            if (DDS_Read32(p+136) & 4) out.faces=6;
            if (out.faces == 6 && out.width != out.height) return false;
            start=148; break;
        }
        default: return false;
        }
    } else if (pf & 0x10000000) {
        out.format=DDS_INDEX; out.bits=8; // Doom's shared normal palette, not a DDS palette payload
    } else if ((pf & 0x20000) && (out.bits == 8 || out.bits == 16)) {
        out.format=DDS_LUMINANCE;
        if (!out.masks[0]) out.masks[0]=255;
    } else if ((pf & 0x40) && (out.bits == 16 || out.bits == 24 || out.bits == 32)) {
        out.format=DDS_RGB;
        if (!out.masks[0] || !out.masks[1] || !out.masks[2]) return false;
        if (!(pf & 1)) out.masks[3]=0;
    } else if (out.bits == 8 && (pf & 3)) {
        out.format=DDS_ALPHA; // accepts Doom's historical ALPHAPIXELS-only A8 header
    } else return false;
    if (!blockBytes && out.format != DDS_INDEX && out.format != DDS_ALPHA) {
        uint32_t used=0;
        for (unsigned i=0;i<4;++i) {
            if (!DDS_MaskValid(out.masks[i],out.bits) || (used & out.masks[i])) return false;
            used |= out.masks[i];
        }
    }
    /* Doom writes tightly packed small mips. Also accept legacy DWORD-padded
     * RGB rows when that layout exactly matches the file's payload length. */
    for (unsigned padded=0;padded<2;++padded) {
        size_t offset=start;
        bool fits=true;
        for (unsigned face=0;face<out.faces;++face) {
            unsigned w=out.width,h=out.height;
            for (unsigned mip=0;mip<out.levels;++mip) {
                size_t pitch=blockBytes ? ((w+3)/4)*blockBytes : (size_t)w*(out.bits/8);
                if (!blockBytes && padded) pitch=(pitch+3)&~(size_t)3;
                if (!blockBytes && mip==0 && (flags & 8)) {
                    unsigned declared=DDS_Read32(p+20);
                    if (declared < pitch) { fits=false; break; }
                    pitch=declared;
                }
                size_t size=pitch*(blockBytes ? (h+3)/4 : h);
                if (offset > len || size > len-offset) { fits=false; break; }
                if (!padded || offset+size<=len) {
                    ddsLevel_t &v=out.level[face][mip];
                    v.offset=offset;v.size=size;v.pitch=pitch;v.width=w;v.height=h;
                }
                offset+=size;w=w>1?w/2:1;h=h>1?h/2:1;
            }
            if (!fits) break;
        }
        if (fits && (offset==len || blockBytes || padded)) return true;
        // A trailing payload is accepted only if no alternate padding layout fits.
        if (!padded && fits) {
            // Try padded layout with an exact length in a separate size calculation.
            size_t end=start;
            for(unsigned f=0;f<out.faces;++f) for(unsigned m=0;m<out.levels;++m) {
                const ddsLevel_t &v=out.level[f][m];
                size_t pitch=(v.width*(out.bits/8)+3)&~(size_t)3;
                if(m==0 && (flags&8)) pitch=DDS_Read32(p+20);
                end+=pitch*v.height;
            }
            if(end!=len) return true;
        } else if (!padded) return false;
    }
    return false;
}

static inline unsigned char DDS_Channel(uint32_t pixel, uint32_t mask, unsigned char fallback) {
    if (!mask) return fallback;
    while (!(mask & 1)) { mask >>= 1; pixel >>= 1; }
    return (unsigned char)(((uint64_t)(pixel & mask)*255 + mask/2)/mask);
}
static inline void DDS_ExpandRGBA(const unsigned char *source, const ddsLayout_t &d,
                                 const ddsLevel_t &v, unsigned char *out,
                                 const unsigned char *normalPalette) {
    for (unsigned y=0;y<v.height;++y) for (unsigned x=0;x<v.width;++x) {
        const unsigned char *s=source+y*v.pitch+x*(d.bits/8);
        unsigned char *dst=out+((size_t)y*v.width+x)*4;
        if(d.format==DDS_INDEX) {
            for(unsigned c=0;c<3;++c) dst[c]=normalPalette[s[0]*3+c];
            dst[3]=255;
        } else if(d.format==DDS_ALPHA) {
            dst[0]=dst[1]=dst[2]=0;dst[3]=s[0];
        } else {
            uint32_t pixel=0;for(unsigned b=0;b<d.bits/8;++b) pixel|=(uint32_t)s[b]<<(b*8);
            dst[0]=DDS_Channel(pixel,d.masks[0],0);
            dst[1]=d.format==DDS_LUMINANCE?dst[0]:DDS_Channel(pixel,d.masks[1],0);
            dst[2]=d.format==DDS_LUMINANCE?dst[0]:DDS_Channel(pixel,d.masks[2],0);
            dst[3]=DDS_Channel(pixel,d.masks[3],255);
        }
    }
}
#endif
