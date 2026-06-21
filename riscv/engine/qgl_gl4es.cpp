/*
 * qgl_gl4es.cpp — bare-metal replacement GL loader for dhewm3 (Phase 5 / doc 14).
 *
 * SKELETON: correct signatures, includes and the full proc list, with TODOs.
 * Not built into the whole engine yet — compile-oriented only.
 *
 * ---------------------------------------------------------------------------
 * What this file does
 * ---------------------------------------------------------------------------
 * dhewm3 has exactly ONE GL choke point. Every core GL entry point is a
 * function pointer `q<name>` defined by the X-macro in neo/renderer/qgl_proc.h
 * (337 procs) and the small ARB/EXT block in neo/renderer/qgl.h. They are ALL
 * resolved through one call (neo/renderer/RenderSystem_init.cpp:822-827):
 *
 *     #define QGLPROC(name, rettype, args) \
 *         q##name = (rettype(APIENTRYP)args)GLimp_ExtensionPointer(#name); \
 *         if (!q##name) common->FatalError("Unable to initialize OpenGL (%s)", #name);
 *     #include "renderer/qgl_proc.h"
 *
 * So retargeting GL == making GLimp_ExtensionPointer(name) return gl4es's
 * gl<Name> / gl<Name>ARB. We do NOT touch qgl.h / qgl_proc.h / draw_arb2.cpp /
 * RenderSystem_init.cpp. This file provides GLimp_ExtensionPointer; the rest of
 * the GLimp_* surface lives in sys_baremetal.cpp.
 *
 * gl4es exports the *desktop* GL names directly (glBegin, glDrawElements,
 * glProgramStringARB, ...). Two wiring options:
 *
 *   (A) DIRECT SYMBOLS  [this skeleton's default, simplest & verified-style]
 *       Forward-declare each gl<Name> with the SAME signature the engine uses
 *       (reuse the qgl_proc.h X-macro) and take &gl<Name>. The linker resolves
 *       them to gl4es's libGL.a. GLimp_ExtensionPointer is a name->&fn lookup
 *       built from the same X-macro, so it can never drift from the engine.
 *
 *   (B) gl4es_GetProcAddress(name)  [fallback if any symbol is hidden/renamed]
 *       gl4es also exposes a resolver. If a future gl4es build hides some
 *       symbols behind its loader, switch the lookup to call that. See
 *       GL4ES_USE_GETPROCADDRESS below.
 *
 * The softgles2 side is already namespaced sgl2_* and reached only through
 * gl4es's set_getprocaddress() shim (riscv/gl4es-bridge/bridge.c) — this file
 * is strictly the *upper* seam (engine <-> gl4es). It does not touch sgl2_*.
 *
 * ---------------------------------------------------------------------------
 * TODO list (Phase 5)
 * ---------------------------------------------------------------------------
 *  [ ] Build gl4es libGL.a for riscv (riscv/gl4es-bridge/build-gl4es.sh riscv)
 *      and confirm it exports the desktop gl* names this file references
 *      (nm libGL.a | grep ' T glProgramStringARB' etc.).
 *  [ ] Decide option (A) direct vs (B) gl4es_GetProcAddress; set the macro.
 *  [ ] Verify APIENTRY/APIENTRYP expand to nothing on RISC-V (they do; no
 *      calling-convention decoration) so the forward decls match gl4es plain C.
 *  [ ] Confirm the ARB/EXT block matches R_CheckPortableExtensions()
 *      (RenderSystem_init.cpp:392-592) — names below mirror qgl.h:75-135.
 *  [ ] Override GL_EXTENSIONS / GL_VERSION in softgles2's glGetString so the
 *      engine's R_CheckExtension() sees the hard-required set (doc 14 §6):
 *      GL_ARB_multitexture, GL_ARB_texture_env_combine, GL_ARB_texture_cube_map,
 *      GL_ARB_texture_env_dot3 + GL_ARB_vertex_program, GL_ARB_fragment_program,
 *      GL_ARB_vertex_buffer_object (+ GL_EXT_stencil_wrap). GL_VERSION must
 *      atof() to >= 2.0 (RenderSystem_init.cpp:493 glStencilOpSeparate branch).
 *  [ ] Run the ARB-program corpus test (doc 14 §7.3) before trusting a frame.
 *  [ ] GOLDEN-IMAGE test attrib locations 8/9/10/11 (doc 14 §C.7 / doc 00 §C.7).
 */

// --- Match the engine's GL type/macro environment exactly --------------------
// qgl.h pulls <SDL_opengl.h>; on bare metal we instead need the GL typedefs and
// APIENTRY/APIENTRYP that the qgl_proc.h X-macro expands against. The engine
// build supplies these through its normal include path (sys/platform.h ->
// the GL headers used for the qgl table). For a standalone skeleton compile,
// the project's GL headers (or the gl4es ones) provide GLenum/GLfloat/... and
// PFNGL*PROC typedefs used by the ARB block in qgl.h.
//
// IMPORTANT: include the SAME header the engine's RenderSystem_init.cpp sees so
// the signatures are identical. In the engine tree that is just:
#include "sys/platform.h"
#define D3_GLPROF_NO_WRAP
#include "renderer/qgl.h"     // declares q* + GLExtension_t + APIENTRYP, pulls GL types
#undef D3_GLPROF_NO_WRAP

#if defined(SGL_BAREMETAL) || defined(BAREMETAL)
#  include "libc/libc.h"      // strcmp on bare metal (riscv/libc)
#else
#  include <string.h>
#endif

// Choose the resolution strategy (see header comment, options A/B).
#ifndef GL4ES_USE_GETPROCADDRESS
#  define GL4ES_USE_GETPROCADDRESS 0   /* 0 = direct &gl<Name> (option A) */
#endif

extern "C" {

#if GL4ES_USE_GETPROCADDRESS
// Option (B): gl4es's own resolver. Exact symbol name read from the vendored
// gl4es source (gl4es exposes gl4es_GetProcAddress / glXGetProcAddress-style).
// TODO: confirm the symbol name in riscv/gl4es/ before enabling.
void *gl4es_GetProcAddress(const char *name);
#else
// Option (A): forward-declare every CORE gl<Name> with the engine's signature,
// straight from the X-macro. These resolve to gl4es's libGL.a at link time.
// APIENTRY expands to nothing on RISC-V, matching gl4es's plain C symbols.
#  define QGLPROC(name, rettype, args) rettype APIENTRY name args;
#  include "renderer/qgl_proc.h"
#  undef QGLPROC

// The ARB / EXT entry points fetched in R_CheckPortableExtensions() and by
// the multitexture / VBO / ARB-program blocks (mirrors qgl.h:75-135). gl4es
// provides all of these as desktop GL symbols.
// These are FUNCTIONS exported by gl4es (not pointer variables), so they must be
// declared as functions to take a true function address. The GL types here
// (GLsizeiptrARB / GLintptrARB) are the SAME ones the PFNGL*PROC typedefs in
// qgl.h use; they come from the GL headers qgl.h pulls in. TODO: if any future
// header revision changes a width, prefer cross-checking against the matching
// PFNGL*PROC typedef rather than editing these by hand.
extern void      APIENTRY glMultiTexCoord2fARB(GLenum, GLfloat, GLfloat);
extern void      APIENTRY glMultiTexCoord2fvARB(GLenum, const GLfloat *);
extern void      APIENTRY glActiveTextureARB(GLenum);
extern void      APIENTRY glClientActiveTextureARB(GLenum);
extern void      APIENTRY glBindBufferARB(GLenum, GLuint);
extern void      APIENTRY glDeleteBuffersARB(GLsizei, const GLuint *);
extern void      APIENTRY glGenBuffersARB(GLsizei, GLuint *);
extern GLboolean APIENTRY glIsBufferARB(GLuint);
extern void      APIENTRY glBufferDataARB(GLenum, GLsizeiptrARB, const GLvoid *, GLenum);
extern void      APIENTRY glBufferSubDataARB(GLenum, GLintptrARB, GLsizeiptrARB, const GLvoid *);
extern void      APIENTRY glGetBufferSubDataARB(GLenum, GLintptrARB, GLsizeiptrARB, GLvoid *);
extern GLvoid *  APIENTRY glMapBufferARB(GLenum, GLenum);
extern GLboolean APIENTRY glUnmapBufferARB(GLenum);
extern void      APIENTRY glGetBufferParameterivARB(GLenum, GLenum, GLint *);
extern void      APIENTRY glGetBufferPointervARB(GLenum, GLenum, GLvoid **);
extern void      APIENTRY glVertexAttribPointerARB(GLuint, GLint, GLenum, GLboolean, GLsizei, const GLvoid *);
extern void      APIENTRY glEnableVertexAttribArrayARB(GLuint);
extern void      APIENTRY glDisableVertexAttribArrayARB(GLuint);
extern void      APIENTRY glProgramStringARB(GLenum, GLenum, GLsizei, const GLvoid *);
extern void      APIENTRY glBindProgramARB(GLenum, GLuint);
extern void      APIENTRY glGenProgramsARB(GLsizei, GLuint *);
extern void      APIENTRY glProgramEnvParameter4fvARB(GLenum, GLuint, const GLfloat *);
extern void      APIENTRY glProgramLocalParameter4fvARB(GLenum, GLuint, const GLfloat *);
extern void      APIENTRY glStencilOpSeparate(GLenum, GLenum, GLenum, GLenum);   // GLES2 core, exposed by gl4es
extern void      APIENTRY glCompressedTexImage2DARB(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const GLvoid *);
extern void      APIENTRY glGetCompressedTexImageARB(GLenum, GLint, GLvoid *);
#endif // GL4ES_USE_GETPROCADDRESS

} // extern "C"


// --- name -> function-pointer table -----------------------------------------
struct GlProcEntry { const char *name; void *fn; };

static const GlProcEntry g_procs[] = {
#if !GL4ES_USE_GETPROCADDRESS
	// CORE 337 procs, auto-generated from the engine's own X-macro so this can
	// never drift from qgl_proc.h. If id ever adds a proc, this rebuilds.
	#define QGLPROC(name, rettype, args) { #name, (void *)&name },
	#include "renderer/qgl_proc.h"
	#undef QGLPROC

	// --- multitexture (qgl.h:75-78) ---
	{ "glMultiTexCoord2fARB",         (void *)&glMultiTexCoord2fARB },
	{ "glMultiTexCoord2fvARB",        (void *)&glMultiTexCoord2fvARB },
	{ "glActiveTextureARB",           (void *)&glActiveTextureARB },
	{ "glClientActiveTextureARB",     (void *)&glClientActiveTextureARB },

	// --- ARB_vertex_buffer_object (qgl.h:81-91) ---
	{ "glBindBufferARB",              (void *)&glBindBufferARB },
	{ "glDeleteBuffersARB",           (void *)&glDeleteBuffersARB },
	{ "glGenBuffersARB",              (void *)&glGenBuffersARB },
	{ "glIsBufferARB",                (void *)&glIsBufferARB },
	{ "glBufferDataARB",              (void *)&glBufferDataARB },
	{ "glBufferSubDataARB",           (void *)&glBufferSubDataARB },
	{ "glGetBufferSubDataARB",        (void *)&glGetBufferSubDataARB },
	{ "glMapBufferARB",               (void *)&glMapBufferARB },
	{ "glUnmapBufferARB",             (void *)&glUnmapBufferARB },
	{ "glGetBufferParameterivARB",    (void *)&glGetBufferParameterivARB },
	{ "glGetBufferPointervARB",       (void *)&glGetBufferPointervARB },

	// --- ARB_vertex_program / ARB_fragment_program (qgl.h:122-129) ---
	{ "glVertexAttribPointerARB",     (void *)&glVertexAttribPointerARB },
	{ "glEnableVertexAttribArrayARB", (void *)&glEnableVertexAttribArrayARB },
	{ "glDisableVertexAttribArrayARB",(void *)&glDisableVertexAttribArrayARB },
	{ "glProgramStringARB",           (void *)&glProgramStringARB },
	{ "glBindProgramARB",             (void *)&glBindProgramARB },
	{ "glGenProgramsARB",             (void *)&glGenProgramsARB },
	{ "glProgramEnvParameter4fvARB",  (void *)&glProgramEnvParameter4fvARB },
	{ "glProgramLocalParameter4fvARB",(void *)&glProgramLocalParameter4fvARB },

	// --- GL2.0 core, used by the >=2.0 stencil branch (qgl.h:107) ---
	{ "glStencilOpSeparate",          (void *)&glStencilOpSeparate },

	// --- ARB_texture_compression (qgl.h:110-111) ---
	{ "glCompressedTexImage2DARB",    (void *)&glCompressedTexImage2DARB },
	{ "glGetCompressedTexImageARB",   (void *)&glGetCompressedTexImageARB },

	// --- Deliberately ABSENT extensions: return 0. These are only fetched
	//     AFTER a passing R_CheckExtension(), which we gate by NOT listing them
	//     in the GL_EXTENSIONS string (softgles2 override). Listed here for the
	//     record; the engine handles a 0 for these (qgl.h:94-135). ---
	{ "glTexImage3D",                 0 },  // EXT_texture3D — unused on GLES2
	{ "glColorTableEXT",              0 },  // EXT_shared_texture_palette — unused
	{ "glActiveStencilFaceEXT",       0 },  // EXT_stencil_two_side — use glStencilOpSeparate
	{ "glDepthBoundsEXT",             0 },  // EXT_depth_bounds_test — optional
	{ "glDebugMessageCallbackARB",    0 },  // only with a debug context
#endif // !GL4ES_USE_GETPROCADDRESS
};


/*
===================
GLimp_ExtensionPointer

The ONE retarget seam. Replaces neo/sys/glimp.cpp:1242 (SDL_GL_GetProcAddress).

  - CORE proc resolving to 0  => engine FatalError (correct: real coverage gap).
  - EXT  proc resolving to 0  => fine, only fetched after R_CheckExtension passes
                                 and we omit unbackable exts from GL_EXTENSIONS.
===================
*/
extern "C" GLExtension_t GLimp_ExtensionPointer(const char *name) {
	if (!name)
		return 0;

#if GL4ES_USE_GETPROCADDRESS
	// Option (B): delegate straight to gl4es's resolver.
	return (GLExtension_t)gl4es_GetProcAddress(name);
#else
	// Option (A): linear scan of the name->fn table. 337 + ~28 entries; called
	// ~365 times total at init only (RenderSystem_init.cpp loop), so O(n) is
	// fine. A perfect hash (sgl_fnv1a is already in the bridge) is a later
	// optimisation if init time ever matters.
	for (unsigned i = 0; i < sizeof(g_procs) / sizeof(g_procs[0]); ++i) {
		if (strcmp(g_procs[i].name, name) == 0)
			return (GLExtension_t)g_procs[i].fn;   // may be 0 for an absent EXT
	}
	// Unknown name: 0. For a core proc the engine FatalErrors (good signal).
	return 0;
#endif
}
