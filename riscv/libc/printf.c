/*
 * Small printf/snprintf for the bare-metal target. Output goes to the SBI
 * console. Supports flags (#, 0, -), field width, length (l, ll) and the
 * conversions c s d i u x X p %. No floating point yet (the engine uses very
 * little %f in console paths; added when needed).
 */
#include "libc/libc.h"
#include "boot/runtime.h"

typedef void (*emit_fn)(void *ctx, char c);

static int do_format(emit_fn out, void *ctx, const char *fmt, va_list ap)
{
    int count = 0;

    for (; *fmt; ++fmt) {
        if (*fmt != '%') { out(ctx, *fmt); ++count; continue; }
        ++fmt;

        int zero = 0, alt = 0, left = 0;
        for (;;) {
            if      (*fmt == '0') zero = 1;
            else if (*fmt == '#') alt  = 1;
            else if (*fmt == '-') left = 1;
            else break;
            ++fmt;
        }

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); ++fmt; }

        int lng = 0;
        while (*fmt == 'l') { ++lng; ++fmt; }

        char spec = *fmt;

        if (spec == 'c') {
            char c = (char)va_arg(ap, int);
            int pad = width - 1;
            if (!left) while (pad-- > 0) { out(ctx, ' '); ++count; }
            out(ctx, c); ++count;
            if (left)  while (pad-- > 0) { out(ctx, ' '); ++count; }
            continue;
        }
        if (spec == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = 0; while (s[len]) ++len;
            int pad = width - len;
            if (!left) while (pad-- > 0) { out(ctx, ' '); ++count; }
            for (int i = 0; i < len; ++i) { out(ctx, s[i]); ++count; }
            if (left)  while (pad-- > 0) { out(ctx, ' '); ++count; }
            continue;
        }
        if (spec == '%') { out(ctx, '%'); ++count; continue; }

        /* integer conversions */
        unsigned long long uv = 0;
        int neg = 0, base = 10, upper = 0;
        const char *prefix = "";

        switch (spec) {
        case 'd': case 'i': {
            long long sv = (lng >= 2) ? va_arg(ap, long long)
                         : (lng == 1) ? va_arg(ap, long)
                                      : va_arg(ap, int);
            if (sv < 0) { neg = 1; uv = (unsigned long long)(-sv); }
            else uv = (unsigned long long)sv;
            break;
        }
        case 'u':
            uv = (lng >= 2) ? va_arg(ap, unsigned long long)
               : (lng == 1) ? va_arg(ap, unsigned long)
                            : va_arg(ap, unsigned int);
            break;
        case 'x': case 'X':
            uv = (lng >= 2) ? va_arg(ap, unsigned long long)
               : (lng == 1) ? va_arg(ap, unsigned long)
                            : va_arg(ap, unsigned int);
            base = 16; upper = (spec == 'X');
            if (alt && uv) prefix = upper ? "0X" : "0x";
            break;
        case 'p':
            uv = (unsigned long long)(uintptr_t)va_arg(ap, void *);
            base = 16; prefix = "0x";
            break;
        default:
            out(ctx, '%'); ++count;
            if (spec) { out(ctx, spec); ++count; }
            continue;
        }

        char digits[24];
        int n = 0;
        const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
        if (uv == 0) digits[n++] = '0';
        else while (uv) { digits[n++] = set[uv % base]; uv /= base; }

        int pfx_len = 0; for (const char *q = prefix; *q; ++q) ++pfx_len;
        int total = n + (neg ? 1 : 0) + pfx_len;
        int pad = width - total;

        if (!left && !zero) while (pad-- > 0) { out(ctx, ' '); ++count; }
        if (neg) { out(ctx, '-'); ++count; }
        for (const char *q = prefix; *q; ++q) { out(ctx, *q); ++count; }
        if (!left && zero) while (pad-- > 0) { out(ctx, '0'); ++count; }
        while (n > 0) { out(ctx, digits[--n]); ++count; }
        if (left) while (pad-- > 0) { out(ctx, ' '); ++count; }
    }

    return count;
}

/* ---- console sink ---- */
static void out_console(void *ctx, char c) { (void)ctx; sbi_console_putchar((unsigned char)c); }

int vprintf(const char *fmt, va_list ap) { return do_format(out_console, 0, fmt, ap); }

int printf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = do_format(out_console, 0, fmt, ap);
    va_end(ap);
    return r;
}

int putchar(int c) { sbi_console_putchar(c); return c; }

int puts(const char *s)
{
    while (*s) sbi_console_putchar(*s++);
    sbi_console_putchar('\n');
    return 0;
}

/* ---- buffer sink ---- */
struct buf_ctx { char *buf; size_t cap; size_t len; };

static void out_buf(void *ctx, char c)
{
    struct buf_ctx *b = (struct buf_ctx *)ctx;
    if (b->len + 1 < b->cap) b->buf[b->len] = c;
    ++b->len;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    struct buf_ctx c = { buf, n, 0 };
    int r = do_format(out_buf, &c, fmt, ap);
    if (n) buf[(c.len < n) ? c.len : n - 1] = '\0';
    return r;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
