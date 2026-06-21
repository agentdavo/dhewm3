#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdlib.h>
#  include <string.h>
#  include <stdint.h>
#  include <stdio.h>
#endif

#include "sg_state.h"
#include "sg_texture.h"
#include "sg_framebuffer.h"
#include "sg_fb.h"
#include "sg_framebin.h"

fb_obj g_framebuffers[MAX_FRAMEBUFFERS];
rb_obj g_renderbuffers[MAX_RENDERBUFFERS];

static void free_renderbuffer_storage(rb_obj *rb)
{
    free(rb->color_alloc ? rb->color_alloc : rb->color); rb->color = 0; rb->color_alloc = 0;
    free(rb->depth_alloc ? rb->depth_alloc : rb->depth); rb->depth = 0; rb->depth_alloc = 0;
    free(rb->stencil_alloc ? rb->stencil_alloc : rb->stencil); rb->stencil = 0; rb->stencil_alloc = 0;
    rb->w = rb->h = 0;
}

static rb_obj *rb_by_id(GLuint id)
{
    return (id && id < MAX_RENDERBUFFERS && g_renderbuffers[id].used) ? &g_renderbuffers[id] : 0;
}

static fb_obj *fb_by_id(GLuint id)
{
    return (id && id < MAX_FRAMEBUFFERS && g_framebuffers[id].used) ? &g_framebuffers[id] : 0;
}

static void refresh_fbo_view(fb_obj *fb)
{
    memset(&fb->view, 0, sizeof(fb->view));
    sgl_tex *color = tex_for_target(fb->color_tex, fb->color_target ? fb->color_target : GL_TEXTURE_2D);
    rb_obj *depth = rb_by_id(fb->depth_rb);
    rb_obj *stencil = rb_by_id(fb->stencil_rb);
    if (!color || !color->texels) return;
    fb->view.w = color->w;
    fb->view.h = color->h;
    fb->view.color = color->texels;
    if (depth && depth->w == color->w && depth->h == color->h) fb->view.depth = depth->depth;
    if (stencil && stencil->w == color->w && stencil->h == color->h) fb->view.stencil = stencil->stencil;
}

static GLenum current_fb_status(void)
{
    if (!C.framebuffer) return GL_FRAMEBUFFER_COMPLETE;
    fb_obj *fb = fb_by_id(C.framebuffer);
    if (!fb) return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
    refresh_fbo_view(fb);
    if (!fb->view.color) return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
    return GL_FRAMEBUFFER_COMPLETE;
}

void glGenFramebuffers(GLsizei n, GLuint *framebuffers)
{
    if (!framebuffers) return;
    for (GLsizei k = 0; k < n; ++k) {
        framebuffers[k] = 0;
        for (GLuint i = 1; i < MAX_FRAMEBUFFERS; ++i)
            if (!g_framebuffers[i].used) {
                memset(&g_framebuffers[i], 0, sizeof(g_framebuffers[i]));
                g_framebuffers[i].used = 1;
                framebuffers[k] = i;
                break;
            }
    }
}

void glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers)
{
    sg_framebin_flush();   /* pending deferred draws may target these */
    if (!framebuffers) return;
    for (GLsizei k = 0; k < n; ++k) {
        GLuint id = framebuffers[k];
        if (id && id < MAX_FRAMEBUFFERS && g_framebuffers[id].used) {
            if (C.framebuffer == id) { C.framebuffer = 0; C.fb = C.default_fb; }
            memset(&g_framebuffers[id], 0, sizeof(g_framebuffers[id]));
        }
    }
}

GLboolean glIsFramebuffer(GLuint framebuffer)
{
    return fb_by_id(framebuffer) ? GL_TRUE : GL_FALSE;
}

void glBindFramebuffer(GLenum target, GLuint framebuffer)
{
    sg_framebin_flush();   /* render-target switch: keep each target's batch ordered + parallel */
    if (target != GL_FRAMEBUFFER) { set_err(GL_INVALID_ENUM); return; }
    if (framebuffer && !fb_by_id(framebuffer)) { set_err(GL_INVALID_OPERATION); return; }
    C.framebuffer = framebuffer;
    if (!framebuffer) {
        C.fb = C.default_fb;
    } else {
        fb_obj *fb = &g_framebuffers[framebuffer];
        refresh_fbo_view(fb);
        C.fb = &fb->view;
    }
}

GLenum glCheckFramebufferStatus(GLenum target)
{
    if (target != GL_FRAMEBUFFER) { set_err(GL_INVALID_ENUM); return GL_FRAMEBUFFER_UNSUPPORTED; }
    return current_fb_status();
}

void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget,
                            GLuint texture, GLint level)
{
    if (target != GL_FRAMEBUFFER) { set_err(GL_INVALID_ENUM); return; }
    if (!C.framebuffer) { set_err(GL_INVALID_OPERATION); return; }
    if (attachment != GL_COLOR_ATTACHMENT0) { set_err(GL_INVALID_ENUM); return; }
    if (level != 0) { set_err(GL_INVALID_VALUE); return; }
    if (texture && !tex_for_target(texture, textarget)) { set_err(GL_INVALID_OPERATION); return; }
    fb_obj *fb = &g_framebuffers[C.framebuffer];
    fb->color_tex = texture;
    fb->color_target = textarget;
    fb->color_level = level;
    refresh_fbo_view(fb);
    C.fb = &fb->view;
}

void glFramebufferRenderbuffer(GLenum target, GLenum attachment,
                               GLenum renderbuffertarget, GLuint renderbuffer)
{
    if (target != GL_FRAMEBUFFER || renderbuffertarget != GL_RENDERBUFFER) { set_err(GL_INVALID_ENUM); return; }
    if (!C.framebuffer) { set_err(GL_INVALID_OPERATION); return; }
    if (renderbuffer && !rb_by_id(renderbuffer)) { set_err(GL_INVALID_OPERATION); return; }
    fb_obj *fb = &g_framebuffers[C.framebuffer];
    if (attachment == GL_DEPTH_ATTACHMENT) fb->depth_rb = renderbuffer;
    else if (attachment == GL_STENCIL_ATTACHMENT) fb->stencil_rb = renderbuffer;
    else if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) fb->depth_rb = fb->stencil_rb = renderbuffer;
    else { set_err(GL_INVALID_ENUM); return; }
    refresh_fbo_view(fb);
    C.fb = &fb->view;
}

void glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment,
                                           GLenum pname, GLint *params)
{
    if (!params) return;
    *params = 0;
    if (target != GL_FRAMEBUFFER) { set_err(GL_INVALID_ENUM); return; }
    fb_obj *fb = C.framebuffer ? fb_by_id(C.framebuffer) : 0;
    GLuint name = 0;
    GLenum type = GL_NONE;
    if (fb) {
        if (attachment == GL_COLOR_ATTACHMENT0) { name = fb->color_tex; type = name ? GL_TEXTURE_2D : GL_NONE; }
        else if (attachment == GL_DEPTH_ATTACHMENT) { name = fb->depth_rb; type = name ? GL_RENDERBUFFER : GL_NONE; }
        else if (attachment == GL_STENCIL_ATTACHMENT) { name = fb->stencil_rb; type = name ? GL_RENDERBUFFER : GL_NONE; }
        else { set_err(GL_INVALID_ENUM); return; }
    }
    if (pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE) *params = (GLint)type;
    else if (pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME) *params = (GLint)name;
    else if (pname == GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL) *params = 0;
    else if (pname == GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE) *params = 0;
}

void glGenRenderbuffers(GLsizei n, GLuint *renderbuffers)
{
    if (!renderbuffers) return;
    for (GLsizei k = 0; k < n; ++k) {
        renderbuffers[k] = 0;
        for (GLuint i = 1; i < MAX_RENDERBUFFERS; ++i)
            if (!g_renderbuffers[i].used) {
                memset(&g_renderbuffers[i], 0, sizeof(g_renderbuffers[i]));
                g_renderbuffers[i].used = 1;
                renderbuffers[k] = i;
                break;
            }
    }
}

void glDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers)
{
    if (!renderbuffers) return;
    for (GLsizei k = 0; k < n; ++k) {
        GLuint id = renderbuffers[k];
        if (id && id < MAX_RENDERBUFFERS && g_renderbuffers[id].used) {
            if (C.renderbuffer == id) C.renderbuffer = 0;
            free_renderbuffer_storage(&g_renderbuffers[id]);
            memset(&g_renderbuffers[id], 0, sizeof(g_renderbuffers[id]));
        }
    }
}

GLboolean glIsRenderbuffer(GLuint renderbuffer)
{
    return rb_by_id(renderbuffer) ? GL_TRUE : GL_FALSE;
}

void glBindRenderbuffer(GLenum target, GLuint renderbuffer)
{
    if (target != GL_RENDERBUFFER) { set_err(GL_INVALID_ENUM); return; }
    if (renderbuffer && !rb_by_id(renderbuffer)) { set_err(GL_INVALID_OPERATION); return; }
    C.renderbuffer = renderbuffer;
}

void glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height)
{
    if (target != GL_RENDERBUFFER) { set_err(GL_INVALID_ENUM); return; }
    rb_obj *rb = rb_by_id(C.renderbuffer);
    if (!rb) { set_err(GL_INVALID_OPERATION); return; }
    if (width < 0 || height < 0) { set_err(GL_INVALID_VALUE); return; }
    free_renderbuffer_storage(rb);
    rb->internalformat = internalformat;
    rb->w = width;
    rb->h = height;
    size_t n = (size_t)width * (size_t)height;
    if (internalformat == GL_DEPTH_COMPONENT || internalformat == GL_DEPTH_COMPONENT16 || internalformat == GL_DEPTH_COMPONENT24) {
        rb->depth = (float *)sgl_alloc_aligned_bytes(n * sizeof(float), 128u, &rb->depth_alloc);
    } else if (internalformat == GL_STENCIL_INDEX || internalformat == GL_STENCIL_INDEX8) {
        rb->stencil = (uint8_t *)sgl_alloc_aligned_bytes(n, 128u, &rb->stencil_alloc);
    } else if (internalformat == GL_DEPTH24_STENCIL8) {
        rb->depth = (float *)sgl_alloc_aligned_bytes(n * sizeof(float), 128u, &rb->depth_alloc);
        rb->stencil = (uint8_t *)sgl_alloc_aligned_bytes(n, 128u, &rb->stencil_alloc);
    } else if (internalformat == GL_RGB || internalformat == GL_RGBA) {
        rb->color = (uint32_t *)sgl_alloc_aligned_bytes(n * sizeof(uint32_t), 128u, &rb->color_alloc);
    } else {
        set_err(GL_INVALID_ENUM);
        return;
    }
    if ((internalformat == GL_DEPTH24_STENCIL8 && (!rb->depth || !rb->stencil)) ||
        ((internalformat == GL_DEPTH_COMPONENT || internalformat == GL_DEPTH_COMPONENT16 || internalformat == GL_DEPTH_COMPONENT24) && !rb->depth) ||
        ((internalformat == GL_STENCIL_INDEX || internalformat == GL_STENCIL_INDEX8) && !rb->stencil) ||
        ((internalformat == GL_RGB || internalformat == GL_RGBA) && !rb->color)) {
        free_renderbuffer_storage(rb);
        set_err(GL_INVALID_OPERATION);
        return;
    }
    if (rb->depth) for (size_t i = 0; i < n; ++i) rb->depth[i] = 1.f;
    if (rb->stencil) memset(rb->stencil, 0, n);
    if (rb->color) memset(rb->color, 0, n * sizeof(uint32_t));
}

void glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint *params)
{
    if (!params) return;
    *params = 0;
    if (target != GL_RENDERBUFFER) { set_err(GL_INVALID_ENUM); return; }
    rb_obj *rb = rb_by_id(C.renderbuffer);
    if (!rb) { set_err(GL_INVALID_OPERATION); return; }
    if (pname == GL_RENDERBUFFER_WIDTH) *params = rb->w;
    else if (pname == GL_RENDERBUFFER_HEIGHT) *params = rb->h;
    else if (pname == GL_RENDERBUFFER_INTERNAL_FORMAT) *params = (GLint)rb->internalformat;
    else if (pname == GL_RENDERBUFFER_RED_SIZE) *params = rb->color ? 8 : 0;
    else if (pname == GL_RENDERBUFFER_GREEN_SIZE) *params = rb->color ? 8 : 0;
    else if (pname == GL_RENDERBUFFER_BLUE_SIZE) *params = rb->color ? 8 : 0;
    else if (pname == GL_RENDERBUFFER_ALPHA_SIZE) *params = rb->color ? 8 : 0;
    else if (pname == GL_RENDERBUFFER_DEPTH_SIZE) *params = rb->depth ? 24 : 0;
    else if (pname == GL_RENDERBUFFER_STENCIL_SIZE) *params = rb->stencil ? 8 : 0;
}
