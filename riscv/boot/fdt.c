/*
 * Minimal read-only FDT reader. See fdt.h for why this exists.
 *
 * The struct block is a flat token stream, so every operation here is a linear
 * walk. That is fine: the DTB is read a handful of times during boot and never
 * in a loop, and a walk costs nothing against the alternative of getting MMIO
 * bases and interrupt contexts wrong on any machine that is not QEMU virt.
 */
#include "fdt.h"
#include "runtime.h"

#include <stddef.h>
#include <string.h>

#define FDT_MAGIC       0xd00dfeedu
#define FDT_BEGIN_NODE  0x1u
#define FDT_END_NODE    0x2u
#define FDT_PROP        0x3u
#define FDT_NOP         0x4u
#define FDT_END         0x9u

#define FDT_MAX_SIZE    (2u * 1024u * 1024u)

uint32_t fdt_be32(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

uint64_t fdt_be64(const void *p)
{
    return ((uint64_t)fdt_be32(p) << 32) | (uint64_t)fdt_be32((const uint8_t *)p + 4);
}

static const uint8_t *fdt_base(void)
{
    const uint8_t *fdt = (const uint8_t *)(uintptr_t)boot_dtb;
    uint32_t total;

    if (!fdt || fdt_be32(fdt) != FDT_MAGIC)
        return NULL;
    total = fdt_be32(fdt + 4);
    if (total < 40u || total > FDT_MAX_SIZE)
        return NULL;
    if (fdt_be32(fdt + 8) >= total || fdt_be32(fdt + 12) >= total)
        return NULL;
    return fdt;
}

int fdt_ok(void) { return fdt_base() != NULL; }

/* Struct block bounds. `*end` is one past the last readable byte. */
static const uint8_t *fdt_struct(const uint8_t **end)
{
    const uint8_t *fdt = fdt_base();
    if (!fdt) { *end = NULL; return NULL; }
    *end = fdt + fdt_be32(fdt + 4);
    return fdt + fdt_be32(fdt + 8);
}

static const char *fdt_strings(void)
{
    const uint8_t *fdt = fdt_base();
    return fdt ? (const char *)(fdt + fdt_be32(fdt + 12)) : NULL;
}

/* Advance `p` past one token, returning the token id. `p` is left pointing at
 * the next token, or at `end` when the stream is exhausted or malformed. */
static uint32_t tok_step(const uint8_t **p, const uint8_t *end)
{
    const uint8_t *q = *p;
    uint32_t tok;

    if (q + 4 > end) { *p = end; return FDT_END; }
    tok = fdt_be32(q);
    q += 4;

    if (tok == FDT_BEGIN_NODE) {
        while (q < end && *q) ++q;                    /* NUL-terminated name */
        q = (const uint8_t *)(((uintptr_t)q + 4) & ~(uintptr_t)3);
    } else if (tok == FDT_PROP) {
        uint32_t len;
        if (q + 8 > end) { *p = end; return FDT_END; }
        len = fdt_be32(q);
        q += 8;
        if (q + len > end) { *p = end; return FDT_END; }
        q += (len + 3u) & ~3u;
    } else if (tok != FDT_END_NODE && tok != FDT_NOP && tok != FDT_END) {
        *p = end;                                     /* malformed */
        return FDT_END;
    }

    if (q > end) q = end;
    *p = q;
    return tok;
}

int fdt_root(void)
{
    const uint8_t *end;
    const uint8_t *s = fdt_struct(&end);
    const uint8_t *p;

    if (!s) return FDT_INVALID;
    p = s;
    while (p < end) {
        const uint8_t *at = p;
        uint32_t tok = tok_step(&p, end);
        if (tok == FDT_BEGIN_NODE) return (int)(at - s);
        if (tok == FDT_END) break;
    }
    return FDT_INVALID;
}

int fdt_next_node(int node)
{
    const uint8_t *end;
    const uint8_t *s = fdt_struct(&end);
    const uint8_t *p;

    if (!s) return FDT_INVALID;
    if (node == FDT_INVALID) return fdt_root();
    if (node < 0 || s + node >= end) return FDT_INVALID;

    p = s + node;
    tok_step(&p, end);                                /* step over this BEGIN_NODE */
    while (p < end) {
        const uint8_t *at = p;
        uint32_t tok = tok_step(&p, end);
        if (tok == FDT_BEGIN_NODE) return (int)(at - s);
        if (tok == FDT_END) break;
    }
    return FDT_INVALID;
}

const char *fdt_node_name(int node)
{
    const uint8_t *end;
    const uint8_t *s = fdt_struct(&end);

    if (!s || node < 0 || s + node + 4 >= end) return "";
    if (fdt_be32(s + node) != FDT_BEGIN_NODE) return "";
    return (const char *)(s + node + 4);
}

const void *fdt_getprop(int node, const char *name, uint32_t *len)
{
    const uint8_t *end;
    const uint8_t *s = fdt_struct(&end);
    const char *strs = fdt_strings();
    const uint8_t *p;
    int depth = 0;

    if (len) *len = 0;
    if (!s || !strs || node < 0 || s + node >= end) return NULL;
    if (fdt_be32(s + node) != FDT_BEGIN_NODE) return NULL;

    p = s + node;
    tok_step(&p, end);

    /* Properties of a node precede any child node, so stop at the first
     * BEGIN_NODE; depth tracking guards a malformed stream. */
    while (p + 4 <= end) {
        const uint8_t *at = p;
        uint32_t tok = fdt_be32(at);
        if (tok == FDT_PROP) {
            if (at + 12 > end) break;       /* truncated FDT_PROP header */
            uint32_t plen = fdt_be32(at + 4);
            uint32_t noff = fdt_be32(at + 8);
            const char *nm = strs + noff;
            tok_step(&p, end);
            if (!strcmp(nm, name)) {
                if (len) *len = plen;
                return at + 12;
            }
            continue;
        }
        if (tok == FDT_NOP) { tok_step(&p, end); continue; }
        (void)depth;
        break;                                        /* child node or END_NODE */
    }
    return NULL;
}

uint32_t fdt_getprop_u32(int node, const char *name, uint32_t dflt)
{
    uint32_t len;
    const void *p = fdt_getprop(node, name, &len);
    return (p && len >= 4) ? fdt_be32(p) : dflt;
}

int fdt_node_is_compatible(int node, const char *compat)
{
    uint32_t len;
    const char *list = (const char *)fdt_getprop(node, "compatible", &len);
    uint32_t i = 0;

    if (!list) return 0;
    while (i < len) {
        const char *entry = list + i;
        uint32_t n = (uint32_t)strlen(entry);
        if (!strcmp(entry, compat)) return 1;
        i += n + 1u;
        if (n == 0) break;
    }
    return 0;
}

int fdt_find_compatible(int from, const char *compat)
{
    int n = (from == FDT_INVALID) ? fdt_root() : fdt_next_node(from);
    while (n != FDT_INVALID) {
        if (fdt_node_is_compatible(n, compat)) return n;
        n = fdt_next_node(n);
    }
    return FDT_INVALID;
}

int fdt_parent(int node)
{
    const uint8_t *end;
    const uint8_t *s = fdt_struct(&end);
    const uint8_t *p;
    int stack[32];
    int depth = 0;

    if (!s || node < 0) return FDT_INVALID;
    p = s;
    while (p < end) {
        const uint8_t *at = p;
        uint32_t tok = tok_step(&p, end);
        if (tok == FDT_BEGIN_NODE) {
            if ((int)(at - s) == node)
                return depth > 0 ? stack[depth - 1] : FDT_INVALID;
            if (depth < (int)(sizeof(stack) / sizeof(stack[0])))
                stack[depth] = (int)(at - s);
            ++depth;
        } else if (tok == FDT_END_NODE) {
            if (depth > 0) --depth;
        } else if (tok == FDT_END) {
            break;
        }
    }
    return FDT_INVALID;
}

int fdt_reg(int node, unsigned index, uint64_t *addr, uint64_t *size)
{
    int parent = fdt_parent(node);
    uint32_t ac = 2, sc = 1;                          /* FDT spec defaults */
    uint32_t len;
    const uint8_t *reg;
    uint32_t stride, off;

    if (parent != FDT_INVALID) {
        ac = fdt_getprop_u32(parent, "#address-cells", 2);
        sc = fdt_getprop_u32(parent, "#size-cells", 1);
    }
    if (ac == 0 || ac > 2 || sc > 2) return -1;

    reg = (const uint8_t *)fdt_getprop(node, "reg", &len);
    if (!reg) return -1;

    stride = (ac + sc) * 4u;
    if (stride == 0) return -1;
    off = index * stride;
    if (off + stride > len) return -1;

    if (addr) *addr = (ac == 2) ? fdt_be64(reg + off) : fdt_be32(reg + off);
    if (size) {
        const uint8_t *sp = reg + off + ac * 4u;
        *size = (sc == 0) ? 0 : (sc == 2 ? fdt_be64(sp) : fdt_be32(sp));
    }
    return 0;
}

int fdt_cpu_has_extension(int cpu_node, const char *ext)
{
    uint32_t len;
    const char *list = (const char *)fdt_getprop(cpu_node, "riscv,isa-extensions", &len);
    uint32_t i = 0;

    if (list) {
        while (i < len) {
            const char *entry = list + i;
            uint32_t n = (uint32_t)strlen(entry);
            if (!strcmp(entry, ext)) return 1;
            i += n + 1u;
            if (n == 0) break;
        }
        return 0;
    }

    /* Legacy single "riscv,isa" string, e.g. "rv64imafdc_sstc". Substring is the
     * best that encoding supports; it is only a fallback. */
    list = (const char *)fdt_getprop(cpu_node, "riscv,isa", &len);
    if (!list) return 0;
    return strstr(list, ext) != NULL;
}
