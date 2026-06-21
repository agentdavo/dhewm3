/*
 * SDL3 "dos" video driver — bare-metal RISC-V dOS.
 *
 * dhewm3-private bring-up code authored from riscv/docs/dos/11-sdl3-video-dos-driver.md.
 * NOT contributed upstream to SDL.
 *
 * One translation unit: device bootstrap, VideoInit/Quit, window create/destroy,
 * the software framebuffer path (CreateWindowFramebuffer/Update -> dos_fb_blit),
 * the GL path (GetProcAddress -> gl4es, CreateContext -> sgl_bridge_init,
 * SwapWindow -> dos_gl_present), and a no-op event pump.
 *
 * Naming convention:
 *   DOS_*  = SDL video-driver callbacks in this file.
 *   dos_*  = payload ABI hooks declared in dos_gl.h and implemented outside SDL.
 *
 * GL gating note: SDL_video.c calls _this->GL_* unconditionally (no
 * SDL_VIDEO_OPENGL* macro gate on the function-pointer dispatch — see
 * SDL_GL_LoadLibrary/GetProcAddress/CreateContext/MakeCurrent/SwapWindow). So
 * this driver works with NO SDL_VIDEO_OPENGL* macro defined, avoiding the
 * Khronos EGL headers entirely.
 */
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_DOS

#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"
#include "../../SDL_properties_c.h"

#include "SDL_dosvideo.h"
#include "dos_gl.h"

#include "../../events/SDL_keyboard_c.h"
#include "../../events/SDL_mouse_c.h"
#include "dos_virtio_input.h"
#include "dos_input_keycodes.h"

#define DOSVID_DRIVER_NAME "dos"

/* Desktop GL entry exported by gl4es. SwapWindow must flush gl4es' pending
 * immediate/FPE work before dos_gl_present reads the softgles2 framebuffer. */
extern void glFlush(void);
#define DOS_SURFACE        "SDL.internal.window.surface"

static bool DOS_ShouldTrace(const char *env)
{
    const char *v = SDL_getenv(env);
    return v && v[0] && v[0] != '0';
}

#define DOS_TRACE(...) do { if (DOS_ShouldTrace("DOS_TRACE")) SDL_Log(__VA_ARGS__); } while (0)
#define DOS_TRACE_PROCS(...) do { if (DOS_ShouldTrace("DOS_TRACE_PROCS")) SDL_Log(__VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ events */

static void DOS_PumpEvents(SDL_VideoDevice *_this)
{
    static float s_abs_x = 0.0f, s_abs_y = 0.0f;   /* tablet: accumulate until SYN */
    static int   s_abs_pending = 0;
    SDL_Window  *win = _this ? _this->windows : NULL;
    struct dos_input_event ev;

    while (vinput_poll_event(&ev)) {
        switch (ev.type) {
        case EVT_KEY: {
            Uint8 btn = dos_evdev_button(ev.code);
            if (btn) {
                SDL_SendMouseButton(0, win, SDL_DEFAULT_MOUSE_ID, btn, ev.value != 0);
            } else {
                SDL_Scancode sc = dos_evdev_scancode(ev.code);
                if (sc != SDL_SCANCODE_UNKNOWN) {
                    /* value 1=down, 0=up, 2=autorepeat (-> down) */
                    SDL_SendKeyboardKey(0, SDL_DEFAULT_KEYBOARD_ID, (int)ev.code, sc, ev.value != 0);
                }
            }
            break;
        }
        case EVT_REL:
            if (ev.code == EVREL_X) {
                SDL_SendMouseMotion(0, win, SDL_DEFAULT_MOUSE_ID, true, (float)(Sint32)ev.value, 0.0f);
            } else if (ev.code == EVREL_Y) {
                SDL_SendMouseMotion(0, win, SDL_DEFAULT_MOUSE_ID, true, 0.0f, (float)(Sint32)ev.value);
            } else if (ev.code == EVREL_WHEEL) {
                SDL_SendMouseWheel(0, win, SDL_DEFAULT_MOUSE_ID, 0.0f, (float)(Sint32)ev.value, SDL_MOUSEWHEEL_NORMAL);
            }
            break;
        case EVT_ABS: {
            int w = 0, h = 0;
            if (win) {
                SDL_GetWindowSizeInPixels(win, &w, &h);
            }
            if (ev.code == EVABS_X && w > 0) {
                s_abs_x = (float)((Uint64)ev.value * (Uint32)w / 32767u);   /* QEMU tablet 0..32767 */
                s_abs_pending = 1;
            } else if (ev.code == EVABS_Y && h > 0) {
                s_abs_y = (float)((Uint64)ev.value * (Uint32)h / 32767u);
                s_abs_pending = 1;
            }
            break;
        }
        case EVT_SYN:
            if (ev.code == EVSYN_REPORT && s_abs_pending && win) {
                SDL_SendMouseMotion(0, win, SDL_DEFAULT_MOUSE_ID, false, s_abs_x, s_abs_y);
                s_abs_pending = 0;
            }
            break;
        default:
            break;
        }
    }
}

static bool DOS_SetRelativeMouseMode(bool enabled)
{
    (void)enabled;
    return true;
}

/* ------------------------------------------------------------------ window */

static bool DOS_CreateWindow(SDL_VideoDevice *_this, SDL_Window *window,
                             SDL_PropertiesID create_props)
{
    (void)_this; (void)create_props;
    SDL_WindowData *data = (SDL_WindowData *)SDL_calloc(1, sizeof(*data));
    if (!data) {
        return false;
    }
    data->sdl_window = window;
    data->is_gl = (window->flags & SDL_WINDOW_OPENGL) != 0;
    window->internal = data;
    DOS_TRACE("dos-video: CreateWindow %dx%d gl=%d", window->w, window->h, data->is_gl ? 1 : 0);

    if (window->x == SDL_WINDOWPOS_UNDEFINED) {
        window->x = 0;
    }
    if (window->y == SDL_WINDOWPOS_UNDEFINED) {
        window->y = 0;
    }

    /* No EGL surface: gl4es+softgles2 own the GL surface, created lazily at
     * GL_CreateContext time and sized to the window. */
    SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, window->w, window->h);
    return true;
}

static void DOS_DestroyWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
    (void)_this;
    SDL_WindowData *data = window->internal;
    if (data) {
        if (data->gl_ctx) {
            DOS_TRACE("dos-video: DestroyWindow destroys GL context %p", data->gl_ctx);
            dos_gl_destroy_context(data->gl_ctx);
            data->gl_ctx = NULL;
        }
        SDL_free(data);
        window->internal = NULL;
    }
}

static void DOS_SetWindowSize(SDL_VideoDevice *_this, SDL_Window *window)
{
    (void)_this;
    SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, window->pending.w, window->pending.h);
}

static bool DOS_SetWindowPosition(SDL_VideoDevice *_this, SDL_Window *window)
{
    (void)_this;
    SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_MOVED, window->pending.x, window->pending.y);
    return true;
}

/* ----------------------------------------------------- software framebuffer */

static bool DOS_CreateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window,
                                        SDL_PixelFormat *format, void **pixels, int *pitch)
{
    (void)_this;
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    SDL_Surface *surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ABGR8888);
    if (!surface) {
        return false;
    }
    SDL_SetSurfaceProperty(SDL_GetWindowProperties(window), DOS_SURFACE, surface);
    *format = SDL_PIXELFORMAT_ABGR8888;
    *pixels = surface->pixels;
    *pitch  = surface->pitch;
    DOS_TRACE("dos-video: CreateWindowFramebuffer %dx%d pitch=%d", w, h, *pitch);
    return true;
}

static bool DOS_UpdateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window,
                                        const SDL_Rect *rects, int numrects)
{
    (void)_this;
    SDL_Surface *s = (SDL_Surface *)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), DOS_SURFACE, NULL);
    if (!s) {
        return SDL_SetError("dos: no framebuffer surface for window");
    }

    /* Flatten the SDL_Rect list to (x,y,w,h) int quads so dos_gl.h stays SDL-free. */
    int  stackrects[4 * 16];
    int *iv = stackrects;
    if (numrects > 0) {
        if (numrects > 16) {
            iv = (int *)SDL_malloc((size_t)numrects * 4 * sizeof(int));
            if (!iv) {
                return SDL_OutOfMemory();
            }
        }
        for (int i = 0; i < numrects; ++i) {
            iv[i * 4 + 0] = rects[i].x;
            iv[i * 4 + 1] = rects[i].y;
            iv[i * 4 + 2] = rects[i].w;
            iv[i * 4 + 3] = rects[i].h;
        }
    }

    int rc = dos_fb_blit(s->pixels, s->pitch, s->w, s->h,
                         (numrects > 0) ? iv : NULL, numrects);
    DOS_TRACE("dos-video: UpdateWindowFramebuffer %dx%d rects=%d rc=%d", s->w, s->h, numrects, rc);

    if (iv != stackrects) {
        SDL_free(iv);
    }
    return (rc == 0) ? true : SDL_SetError("dos: framebuffer blit failed");
}

static void DOS_DestroyWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window)
{
    (void)_this;
    SDL_ClearProperty(SDL_GetWindowProperties(window), DOS_SURFACE);
}

/* -------------------------------------------------------------------- GL */

static int g_swap_interval = 0;

static bool DOS_GL_LoadLibrary(SDL_VideoDevice *_this, const char *path)
{
    (void)path;
    /* gl4es + softgles2 are statically linked into the payload; nothing to
     * dlopen. Record a fake path so SDL's "already loaded with a different
     * path" check (SDL_video.c:4735) stays happy. */
    SDL_strlcpy(_this->gl_config.driver_path, "gl4es",
                sizeof(_this->gl_config.driver_path));
    DOS_TRACE("dos-video: GL_LoadLibrary path=%s -> gl4es", path ? path : "(null)");
    return true;
}

static void DOS_GL_UnloadLibrary(SDL_VideoDevice *_this)
{
    (void)_this;
}

static SDL_FunctionPointer DOS_GL_GetProcAddress(SDL_VideoDevice *_this, const char *proc)
{
    (void)_this;
    SDL_FunctionPointer fn = (SDL_FunctionPointer)gl4es_GetProcAddress(proc);
    DOS_TRACE_PROCS("dos-video: GL_GetProcAddress %s -> %p", proc ? proc : "(null)", fn);
    return fn;
}

static SDL_GLContext DOS_GL_CreateContext(SDL_VideoDevice *_this, SDL_Window *window)
{
    (void)_this;
    SDL_WindowData *data = window->internal;
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);

    void *ctx = dos_gl_create_context(w, h);   /* creates fb + sgl_bridge_init, current */
    if (!ctx) {
        SDL_SetError("dos: softgles2 context creation failed");
        return NULL;
    }
    DOS_TRACE("dos-video: GL_CreateContext %dx%d -> %p", w, h, ctx);
    if (data) {
        data->gl_ctx = ctx;
        data->is_gl = true;
    }
    /* SDL marks it current for us (SDL_video.c:5443-5448). */
    return (SDL_GLContext)ctx;
}

static bool DOS_GL_MakeCurrent(SDL_VideoDevice *_this, SDL_Window *window, SDL_GLContext ctx)
{
    (void)_this; (void)window;
    int rc = dos_gl_make_current((void *)ctx);
    DOS_TRACE("dos-video: GL_MakeCurrent %p rc=%d", (void *)ctx, rc);
    return rc == 0;
}

static bool DOS_GL_DestroyContext(SDL_VideoDevice *_this, SDL_GLContext ctx)
{
    (void)_this;
    DOS_TRACE("dos-video: GL_DestroyContext %p", (void *)ctx);
    dos_gl_destroy_context((void *)ctx);
    return true;
}

static bool DOS_GL_SwapWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
    (void)_this;
    void *ctx = window->internal ? ((SDL_WindowData *)window->internal)->gl_ctx : NULL;
    glFlush();
    int rc = dos_gl_present(ctx);
    DOS_TRACE("dos-video: GL_SwapWindow ctx=%p rc=%d", ctx, rc);
    return rc == 0 ? true : SDL_SetError("dos: present failed");
}

static bool DOS_GL_SetSwapInterval(SDL_VideoDevice *_this, int interval)
{
    (void)_this;
    g_swap_interval = interval;
    return true;
}

static bool DOS_GL_GetSwapInterval(SDL_VideoDevice *_this, int *interval)
{
    (void)_this;
    *interval = g_swap_interval;
    return true;
}

/* ------------------------------------------------------------ video init */

static bool DOS_VideoInit(SDL_VideoDevice *_this)
{
    SDL_VideoData *vd = _this->internal;
    SDL_DisplayMode mode;

    int w = 800, h = 600;
    dos_fb_get_size(&w, &h);
    if (w <= 0 || h <= 0) { w = 800; h = 600; }
    if (vd) { vd->fb_w = w; vd->fb_h = h; }
    DOS_TRACE("dos-video: VideoInit display=%dx%d", w, h);

    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_XRGB8888;
    mode.w = w;
    mode.h = h;
    if (SDL_AddBasicVideoDisplay(&mode) == 0) {
        return false;
    }

    SDL_GetMouse()->SetRelativeMouseMode = DOS_SetRelativeMouseMode;

    /* dos input: bring up virtio-input and register the SDL keyboard/mouse so the
     * events injected by DOS_PumpEvents are accepted by the event subsystem. */
    if (vinput_init() > 0) {
        SDL_AddKeyboard(SDL_DEFAULT_KEYBOARD_ID, "dos virtio-input keyboard");
        SDL_AddMouse(SDL_DEFAULT_MOUSE_ID, "dos virtio-input mouse");
    }
    return true;
}

static bool DOS_SetDisplayMode(SDL_VideoDevice *_this, SDL_VideoDisplay *display, SDL_DisplayMode *mode)
{
    (void)_this; (void)display; (void)mode;
    return true;
}

static void DOS_VideoQuit(SDL_VideoDevice *_this)
{
    (void)_this;
}

/* ----------------------------------------------------------- bootstrap */

static void DOS_DeleteDevice(SDL_VideoDevice *device)
{
    if (device) {
        SDL_free(device->internal);
        SDL_free(device);
    }
}

static SDL_VideoDevice *DOS_CreateDevice(void)
{
    SDL_VideoDevice *device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        return NULL;
    }
    device->internal = (SDL_VideoData *)SDL_calloc(1, sizeof(SDL_VideoData));
    if (!device->internal) {
        SDL_free(device);
        return NULL;
    }

    /* is_dummy stays false so our framebuffer hooks are actually called. */

    /* core */
    device->VideoInit      = DOS_VideoInit;
    device->VideoQuit      = DOS_VideoQuit;
    device->SetDisplayMode = DOS_SetDisplayMode;
    device->PumpEvents     = DOS_PumpEvents;
    device->free           = DOS_DeleteDevice;

    /* window */
    device->CreateSDLWindow   = DOS_CreateWindow;
    device->DestroyWindow     = DOS_DestroyWindow;
    device->SetWindowSize     = DOS_SetWindowSize;
    device->SetWindowPosition = DOS_SetWindowPosition;

    /* software framebuffer path */
    device->CreateWindowFramebuffer  = DOS_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer  = DOS_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = DOS_DestroyWindowFramebuffer;

    /* GL path (no EGL!) */
    device->GL_LoadLibrary     = DOS_GL_LoadLibrary;
    device->GL_UnloadLibrary   = DOS_GL_UnloadLibrary;
    device->GL_GetProcAddress  = DOS_GL_GetProcAddress;
    device->GL_CreateContext   = DOS_GL_CreateContext;
    device->GL_MakeCurrent     = DOS_GL_MakeCurrent;
    device->GL_DestroyContext  = DOS_GL_DestroyContext;
    device->GL_SwapWindow      = DOS_GL_SwapWindow;
    device->GL_SetSwapInterval = DOS_GL_SetSwapInterval;
    device->GL_GetSwapInterval = DOS_GL_GetSwapInterval;

    return device;
}

VideoBootStrap DOS_bootstrap = {
    DOSVID_DRIVER_NAME, "SDL bare-metal (dOS) video driver",
    DOS_CreateDevice,
    NULL,   /* no ShowMessageBox */
    /* is_preferred = false: registered first in bootstrap[], DOS_CreateDevice
     * unconditionally succeeds, so the no-hint auto-select picks it; and being
     * non-preferred it can also be selected by name (SDL_HINT_VIDEO_DRIVER) as
     * a safety, unlike Wayland_preferred which is_preferred=true blocks. */
    false
};

#endif /* SDL_VIDEO_DRIVER_DOS */
