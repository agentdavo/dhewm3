#ifndef SOFTGLES2_GLSL_INTERNAL_H
#define SOFTGLES2_GLSL_INTERNAL_H

#include "glsl.h"

/* ---------------- types ---------------- */
enum { T_VOID, T_FLOAT, T_VEC2, T_VEC3, T_VEC4, T_MAT4, T_SAMPLER2D, T_INT, T_BOOL, T_SAMPLERCUBE };
enum { Q_NONE, Q_ATTRIB, Q_UNIFORM, Q_VARYING, Q_CONST };

/* ---------------- AST ---------------- */
enum { N_NUM, N_VAR, N_SWZ, N_NEG, N_NOT, N_BIN, N_CMP, N_CALL, N_TERN, N_INDEX };

typedef struct node node;
struct node {
    int   k;
    float num;
    char  name[40];
    int   op;
    node *a, *b, *c;
    int   swz[4], swzn;
    node *args[8]; int nargs;
};

/* ---------------- arena allocator ---------------- */
#define SGL_ARENA_BLOCK_SIZE 16384
typedef struct arena_block {
    struct arena_block *next;
    long  used;
    char  data[SGL_ARENA_BLOCK_SIZE];
} arena_block;
typedef struct { arena_block *head; } arena;

enum { S_DECL, S_ASSIGN, S_DISCARD, S_NOP };
typedef struct {
    int   k;
    int   type;
    char  name[40];
    int   idx;
    int   swz[4], swzn;
    node *expr;
} stmt;

typedef struct { char name[40]; int type, qual, location, uoffset, arr; } glvar;
typedef struct { char name[40]; char param[4][40]; int nparam; node *ret; int body_pos; } func_def;

typedef struct {
    glvar globals[64]; int nglob;
    stmt  body[256];   int nbody;
    func_def funcs[8]; int nfuncs;
    int   has_main;
    arena ast_arena;
} shader;

/* ---------------- flat bytecode IR (A2) ---------------- */
enum { IR_LOADK, IR_LOADV, IR_LOADU, IR_LOADS, IR_LOADZ, IR_MOV,
       IR_SWZ, IR_NEG, IR_NOT, IR_INDEX, IR_BIN, IR_CMP, IR_TERN,
       IR_CONSTRUCT, IR_BUILTIN, IR_STOREL, IR_STOREF, IR_DISCARD,
       IR_LOADA, IR_STORECLIP, IR_STOREV };
#define IR_MAXREG  96
#define IR_MAXINST 1024
typedef struct {
    short op, dst;
    short a, b, c;
    short args[8]; short nargs;
    int   i0, i1;
    signed char swz[4], swzn;
    float k;
} ir_inst;
typedef struct {
    int ok, ninst, nreg, nlocal;
    int simt_ok;
    ir_inst *inst;
} ir_prog;

struct glsl_program;
/* Optional register-resident specialised fragment kernel (Carmack lever): when
 * set at link time by glsl_fs_classify(), the SIMT executor calls it instead of
 * walking the per-op VM. Same SoA contract as glsl_run_fragment_simt_u. */
typedef int (*glsl_fs_kernel_fn)(struct glsl_program *p, const float *varys, int vl,
                                 const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                                 const float *ustore, float *out);

struct glsl_program {
    shader vs, fs;
    struct { char name[40]; int comp, offset, arr; } vary[16]; int nvary, nvaryfloats;
    struct { char name[40]; int type, offset, is_sampler, unit; } uni[GLSL_MAX_UNIFORMS]; int nuni;
    struct { char name[40]; int index; } abind[GLSL_MAX_ATTRIBS]; int nabind;
    float ustore[GLSL_USTORE_FLOATS];
    int   linked;
    ir_prog fs_ir;
    ir_prog vs_ir;
    glsl_fs_kernel_fn fs_kernel;   /* 0 = use the VM */
    int   fs_has_discard;          /* FS uses discard -> SIMT must emit a per-lane kill mask */
    /* Set by glsl_fs_classify when the FS output alpha is provably 0 wherever the
     * incoming vertex-colour alpha (vary[3]) is 0 — i.e. gl_FragColor.a is a product
     * that includes Color.a (the gl4es 2D modulate/overlay archetype). Lets the draw
     * path skip a fully-transparent layer under standard alpha blending (no-op). */
    int   fs_alpha_zero_if_color_zero;
    /* Shader-workload profiling (active when glsl_link_dump is set): per-program
     * fragment counts split by path, so the archetype-specialisation work-list is
     * ranked by REAL frame cost, not guesswork. dbg_kernel_name is the bound
     * kernel ("interaction"/"overlay") or "VM". */
    unsigned long dbg_frags_kernel, dbg_frags_vm;
    const char   *dbg_kernel_name;
    int           dbg_id;
    /* per-program draw counts by blend class (glsl_link_dump) — resolves whether a
     * shader is opaque surface shading vs additive light overdraw. */
    unsigned long dbg_draws_opaque, dbg_draws_add, dbg_draws_alpha;
};

void glsl_fs_classify(struct glsl_program *p);   /* set fs_kernel if FS matches an archetype */

/* ---------------- scalar value ---------------- */
typedef struct { float v[16]; int n; } gval;

/* ---------------- lexer types ---------------- */
enum { TK_EOF, TK_ID, TK_NUM, TK_OP };
#define MAX_TOK 4096
typedef struct { int kind; char text[40]; float num; } token;
typedef struct { token t[MAX_TOK]; int n, pos; } toklist;

/* ---------------- builtin IDs ---------------- */
enum { BI_TEXTURE2D, BI_DOT, BI_LENGTH, BI_DISTANCE, BI_NORMALIZE, BI_CROSS, BI_MIX,
       BI_CLAMP, BI_MIN, BI_MAX, BI_ABS, BI_FLOOR, BI_CEIL, BI_FRACT, BI_SIGN, BI_SQRT,
       BI_INVERSESQRT, BI_MOD, BI_STEP, BI_SMOOTHSTEP, BI_REFLECT, BI_POW,
       BI_TEXTURE2DPROJ, BI_TEXTURECUBE, BI_COUNT };

/* ---- cross-file function declarations ---- */
int   lex(const char *s, toklist *tl);
int   kw_type(const char *s, int *t);
int   parse_shader(shader *sh, const char *src, char *err, int errlen);
int   type_comp(int t);
void *arena_alloc(arena *a, int size);
void  arena_free(arena *a);
glvar *find_global(shader *sh, const char *name);
int   vary_slot(glsl_program *p, const char *name, int index, int *comp);
int   find_uniform(glsl_program *p, const char *name);
gval  ir_do_builtin(int id, const gval *a, int nargs, const sgl_tex *const *samplers);
void  sample_cube_tex(const sgl_tex *t, float dx, float dy, float dz, float out[4]);
int   ir_run_fragment(glsl_program *p, const float *varying, const sgl_tex *const samplers[GLSL_MAX_SAMPLERS], float rgba[4]);
int   ir_run_fragment_u(glsl_program *p, const float *varying, const sgl_tex *const samplers[GLSL_MAX_SAMPLERS], const float *ustore, float rgba[4]);
int   glsl_run_fragment_ref_u(glsl_program *p, const float *varying, const sgl_tex *const samplers[GLSL_MAX_SAMPLERS], const float *ustore, float rgba[4]);
void  ir_free(ir_prog *ir);
void  ir_set_simt_ok(ir_prog *ir);
void  ir_compile_fs(glsl_program *p);
void  ir_compile_vs(glsl_program *p);

#endif /* SOFTGLES2_GLSL_INTERNAL_H */
