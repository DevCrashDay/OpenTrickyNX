/*
 * d3d8_gl_draw.c -- drawing on OpenGL (non-Windows).
 *
 *   - Render states -> GL (d3d8_states.c's job): blend, depth, stencil,
 *     cull, fill, scissor (NV2A window clip) and per-stage sampler objects
 *     built from the texture stage states exactly as the D3D11 samplers are.
 *   - The fixed-function path (d3d8_shaders.c ported to GLSL): FVF vertices,
 *     pre-transformed or transformed and lit, and the D3D8 texture-stage
 *     blend ops, fog, specular and alpha test in the pixel shader. The
 *     translator's 2D and fallback draws come through here.
 *   - The NV2A-native path (d3d8_nv2a.c): the perspective-correct pass-
 *     through vertex shader over ProgVertex (116 bytes), and a program cache
 *     keyed by nv2a_psh_key() of the GLSL pixel shaders nv2a_psh.c generates.
 *   - Vertex programs on the GPU: GLSL vertex shaders generated from the
 *     decoded program (d3d8_gl_vsh.c), linked with those pixel shaders, over
 *     the title's own vertex arrays. Point sprites stay on the CPU path.
 *
 * Clip-space conversion: D3D's z in [0, w] becomes GL's [-w, w] with
 * z' = 2z - w in every vertex shader; winding is unchanged since the image
 * is not flipped (see d3d8_gl_device.c), so a D3D clockwise front face is
 * glFrontFace(GL_CW).
 */
#include "d3d8_gl_internal.h"
#include "d3d8_gpuprof.h"
#include "../kernel/xbox_perf.h"
#include "d3d8_nv2a_vsh.h"
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile long g_nv_compiles = 0;
volatile double g_nv_compile_ms = 0;
volatile long g_nv_shc_hits = 0;        /* no shader disk cache on GL */

static int s_ext_aniso, s_ext_mirror_once;
static GLfloat s_max_aniso = 1.0f;
static int s_gl_debug = -1;

static void gl_check(const char *where)
{
    GLenum e;
    static int reported;
    if (s_gl_debug < 0) { const char *v = getenv("XBOX_GL_DEBUG"); s_gl_debug = v && v[0] == '1'; }
    if (!s_gl_debug) return;
    while ((e = glGetError()) != GL_NO_ERROR)
        if (reported++ < 64) fprintf(stderr, "[GL] error 0x%04X after %s\n", e, where);
}

/* ================================================================
 * Window clip (NV2A SET_WINDOW_CLIP) and anisotropy (d3d8_states.c API)
 * ================================================================ */
static int  g_wclip_on;
static RECT g_wclip;

void d3d8_SetWindowClip(int on, long x0, long y0, long x1, long y1)
{
    g_wclip_on = on;
    g_wclip.left = x0; g_wclip.top = y0; g_wclip.right = x1; g_wclip.bottom = y1;
}

int d3d8_WindowClipOn(void) { return g_wclip_on; }

static int g_host_aniso;
void d3d8_SetAnisotropy(int n) { g_host_aniso = (n == 2 || n == 4 || n == 8 || n == 16) ? n : 0; }
int  d3d8_GetAnisotropy(void)  { return g_host_aniso; }

/* ================================================================
 * GL state cache
 * ================================================================ */
static struct {
    int     valid;
    int     blend; GLenum bs, bd, bsa, bda, bop; unsigned cmask;
    int     depth; GLenum zfunc; int zmask;
    int     stencil; GLenum sfunc; GLint sref; GLuint srmask, swmask; GLenum sfail, szfail, spass;
    int     cull; GLenum cull_face, front; int clamp; GLenum poly;
    int     scissor; GLint sc[4];
    GLuint  prog, vao;
    GLuint  tex[GLD_MAX_STAGES], smp[GLD_MAX_STAGES];
} C;

void gld_state_invalidate(void) { C.valid = 0; }

static void set_enable(GLenum cap, int on, int *cur)
{
    if (C.valid && *cur == on) return;
    *cur = on;
    if (on) glEnable(cap); else glDisable(cap);
}

static void begin_state(void)
{
    if (C.valid) return;
    memset(&C, 0, sizeof C);
    gld_bind_draw_target();
    /* force every setter below to issue its call */
    C.blend = C.depth = C.stencil = C.cull = C.clamp = C.scissor = -1;
    C.cmask = 0xFFFFFFFFu; C.zmask = -1; C.poly = 0; C.front = 0; C.cull_face = 0;
    C.bs = C.bd = C.bsa = C.bda = C.bop = 0; C.zfunc = 0; C.sfunc = 0; C.sfail = 0;
    C.valid = 1;
    {   /* setters compare against these; make the first comparison fail */
        int i;
        for (i = 0; i < GLD_MAX_STAGES; i++) { C.tex[i] = 0xFFFFFFFFu; C.smp[i] = 0xFFFFFFFFu; }
        C.prog = 0xFFFFFFFFu; C.vao = 0xFFFFFFFFu;
    }
}

static void use_program(GLuint p) { if (C.prog != p) { C.prog = p; glUseProgram(p); } }
static void bind_vao(GLuint v)    { if (C.vao != v)  { C.vao = v;  glBindVertexArray(v); } }

/* ================================================================
 * Enum translation (d3d8_states.c)
 * ================================================================ */
static GLenum gl_blend(DWORD b)
{
    switch (b) {
    case D3DBLEND_ZERO:         return GL_ZERO;
    case D3DBLEND_ONE:          return GL_ONE;
    case D3DBLEND_SRCCOLOR:     return GL_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR:  return GL_ONE_MINUS_SRC_COLOR;
    case D3DBLEND_SRCALPHA:     return GL_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA:  return GL_ONE_MINUS_SRC_ALPHA;
    case D3DBLEND_DESTALPHA:    return GL_DST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return GL_ONE_MINUS_DST_ALPHA;
    case D3DBLEND_DESTCOLOR:    return GL_DST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return GL_ONE_MINUS_DST_COLOR;
    case D3DBLEND_SRCALPHASAT:  return GL_SRC_ALPHA_SATURATE;
    default:                    return GL_ONE;
    }
}

/* The alpha-channel counterpart of a colour factor (as the D3D11 backend). */
static GLenum gl_blend_alpha(GLenum b)
{
    switch (b) {
    case GL_SRC_COLOR:           return GL_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_COLOR: return GL_ONE_MINUS_SRC_ALPHA;
    case GL_DST_COLOR:           return GL_DST_ALPHA;
    case GL_ONE_MINUS_DST_COLOR: return GL_ONE_MINUS_DST_ALPHA;
    default:                     return b;
    }
}

static GLenum gl_blendop(DWORD op)
{
    switch (op) {
    case 2: return GL_FUNC_SUBTRACT;
    case 3: return GL_FUNC_REVERSE_SUBTRACT;
    case 4: return GL_MIN;
    case 5: return GL_MAX;
    default: return GL_FUNC_ADD;
    }
}

static GLenum gl_cmp(DWORD c)
{
    switch (c) {
    case D3DCMP_NEVER:        return GL_NEVER;
    case D3DCMP_LESS:         return GL_LESS;
    case D3DCMP_EQUAL:        return GL_EQUAL;
    case D3DCMP_LESSEQUAL:    return GL_LEQUAL;
    case D3DCMP_GREATER:      return GL_GREATER;
    case D3DCMP_NOTEQUAL:     return GL_NOTEQUAL;
    case D3DCMP_GREATEREQUAL: return GL_GEQUAL;
    case D3DCMP_ALWAYS:       return GL_ALWAYS;
    default:                  return GL_LEQUAL;
    }
}

static GLenum gl_stencilop(DWORD op)
{
    switch (op) {
    case 2: return GL_ZERO;
    case 3: return GL_REPLACE;
    case 4: return GL_INCR;             /* D3D INCRSAT */
    case 5: return GL_DECR;             /* D3D DECRSAT */
    case 6: return GL_INVERT;
    case 7: return GL_INCR_WRAP;
    case 8: return GL_DECR_WRAP;
    default: return GL_KEEP;
    }
}

static GLenum gl_address(DWORD m)
{
    switch (m) {
    case D3DTADDRESS_MIRROR:     return GL_MIRRORED_REPEAT;
    case D3DTADDRESS_CLAMP:      return GL_CLAMP_TO_EDGE;
    case D3DTADDRESS_BORDER:     return GL_CLAMP_TO_BORDER;
    case D3DTADDRESS_MIRRORONCE: return s_ext_mirror_once ? GL_MIRROR_CLAMP_TO_EDGE : GL_MIRRORED_REPEAT;
    default:                     return GL_REPEAT;
    }
}

/* ================================================================
 * Samplers (d3d8_states_apply_sampler)
 * ================================================================ */
typedef struct {
    GLint min, mag, wrap_s, wrap_t;
    GLfloat aniso, bias, min_lod, max_lod;
} SamplerDesc;

#define SAMPLER_CACHE 256
static struct { SamplerDesc d; GLuint s; } s_smp[SAMPLER_CACHE];
static int s_nsmp;

static GLuint sampler_for_stage(DWORD stage)
{
    const DWORD *tss = g_gld.tss[stage];
    SamplerDesc d;
    union { DWORD u; float f; } bias;
    DWORD mag = tss[D3DTSS_MAGFILTER], min = tss[D3DTSS_MINFILTER], mip = tss[D3DTSS_MIPFILTER];
    int min_lin = (min == D3DTEXF_LINEAR || min == D3DTEXF_ANISOTROPIC);
    int mag_lin = (mag == D3DTEXF_LINEAR || mag == D3DTEXF_ANISOTROPIC);
    int mip_lin = (mip == D3DTEXF_LINEAR);
    int aniso_filter = (mag == D3DTEXF_ANISOTROPIC || min == D3DTEXF_ANISOTROPIC);
    UINT aniso = tss[D3DTSS_MAXANISOTROPY] ? tss[D3DTSS_MAXANISOTROPY] : 1;
    int k;
    GLuint s;

    memset(&d, 0, sizeof d);
    if (aniso > 16) aniso = 16;
    if ((g_host_aniso > 1 || aniso > 1) && min != D3DTEXF_POINT && min != D3DTEXF_NONE) {
        aniso_filter = 1;
        if ((UINT)g_host_aniso > aniso) aniso = (UINT)g_host_aniso;
    }
    if (aniso_filter) {                 /* D3D11_FILTER_ANISOTROPIC: all linear */
        d.min = GL_LINEAR_MIPMAP_LINEAR;
        d.mag = GL_LINEAR;
        d.aniso = s_ext_aniso ? (GLfloat)aniso : 1.0f;
        if (d.aniso > s_max_aniso) d.aniso = s_max_aniso;
    } else {
        /* D3D11 has no "no mip" filter: D3DTEXF_NONE is point mip, held to
         * one level by the LOD range below. */
        d.min = min_lin ? (mip_lin ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST)
                        : (mip_lin ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
        d.mag = mag_lin ? GL_LINEAR : GL_NEAREST;
        d.aniso = 1.0f;
    }
    d.wrap_s = (GLint)gl_address(tss[D3DTSS_ADDRESSU] ? tss[D3DTSS_ADDRESSU] : D3DTADDRESS_WRAP);
    d.wrap_t = (GLint)gl_address(tss[D3DTSS_ADDRESSV] ? tss[D3DTSS_ADDRESSV] : D3DTADDRESS_WRAP);
    bias.u = tss[D3DTSS_MIPMAPLODBIAS];
    d.bias = (bias.f == bias.f && bias.f > -16.0f && bias.f < 16.0f) ? bias.f : 0.0f;
    d.min_lod = (GLfloat)tss[D3DTSS_MAXMIPLEVEL];
    d.max_lod = (mip == D3DTEXF_NONE) ? d.min_lod : 1000.0f;

    for (k = 0; k < s_nsmp; k++)
        if (!memcmp(&s_smp[k].d, &d, sizeof d)) return s_smp[k].s;
    glGenSamplers(1, &s);
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, d.min);
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, d.mag);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, d.wrap_s);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_T, d.wrap_t);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_R, GL_REPEAT);
    glSamplerParameterf(s, GL_TEXTURE_LOD_BIAS, d.bias);
    glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, d.min_lod);
    glSamplerParameterf(s, GL_TEXTURE_MAX_LOD, d.max_lod);
    if (s_ext_aniso) glSamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY_EXT, d.aniso);
    if (s_nsmp < SAMPLER_CACHE) { s_smp[s_nsmp].d = d; s_smp[s_nsmp].s = s; s_nsmp++; }
    else {                              /* full: recycle the oldest slot */
        static int next;
        glDeleteSamplers(1, &s_smp[next].s);
        s_smp[next].d = d; s_smp[next].s = s;
        next = (next + 1) % SAMPLER_CACHE;
        gld_state_invalidate();
    }
    return s;
}

/* ================================================================
 * Textures for the four stages
 * ================================================================ */
static GLuint s_dummy_tex;              /* (0,0,0,0): an unbound SRV reads zero */

static void bind_stage_textures(void)
{
    GLuint names[GLD_MAX_STAGES];
    int i;
    /* Uploads first (they bind on a scratch unit), then the bindings. */
    for (i = 0; i < GLD_MAX_STAGES; i++) {
        names[i] = g_gld.tex[i] ? gld_texture_ensure(g_gld.tex[i]) : 0;
        if (!names[i]) names[i] = s_dummy_tex;
    }
    for (i = 0; i < GLD_MAX_STAGES; i++) {
        GLuint smp = sampler_for_stage((DWORD)i);
        if (C.tex[i] != names[i]) {
            glActiveTexture(GL_TEXTURE0 + (GLenum)i);
            glBindTexture(GL_TEXTURE_2D, names[i]);
            C.tex[i] = names[i];
        }
        if (C.smp[i] != smp) { glBindSampler((GLuint)i, smp); C.smp[i] = smp; }
    }
}

/* ================================================================
 * Render states (d3d8_states_apply)
 * ================================================================ */
static void apply_raster(int cull_on, GLenum cull_face, GLenum front, int clamp, GLenum poly)
{
    set_enable(GL_CULL_FACE, cull_on, &C.cull);
    if (cull_on && C.cull_face != cull_face) { C.cull_face = cull_face; glCullFace(cull_face); }
    if (C.front != front) { C.front = front; glFrontFace(front); }
    set_enable(GL_DEPTH_CLAMP, clamp, &C.clamp);
    if (C.poly != poly) { C.poly = poly; glPolygonMode(GL_FRONT_AND_BACK, poly); }
}

void gld_states_apply(void)
{
    const DWORD *rs = g_gld.rs;
    GLenum bs, bd, bop, zf;
    unsigned cm;
    int on;

    begin_state();
    /* Blend */
    set_enable(GL_BLEND, rs[D3DRS_ALPHABLENDENABLE] ? 1 : 0, &C.blend);
    bs = gl_blend(rs[D3DRS_SRCBLEND]);
    bd = gl_blend(rs[D3DRS_DESTBLEND]);
    if (bs != C.bs || bd != C.bd) {
        C.bs = bs; C.bd = bd;
        C.bsa = gl_blend_alpha(bs); C.bda = gl_blend_alpha(bd);
        glBlendFuncSeparate(bs, bd, C.bsa, C.bda);
    }
    bop = gl_blendop(rs[D3DRS_BLENDOP] ? rs[D3DRS_BLENDOP] : 1);
    if (bop != C.bop) { C.bop = bop; glBlendEquation(bop); }
    cm = rs[D3DRS_COLORWRITEENABLE] & 0x0F;
    if (cm != C.cmask) {
        C.cmask = cm;
        glColorMask((cm & 1) != 0, (cm & 2) != 0, (cm & 4) != 0, (cm & 8) != 0);
    }
    /* Depth */
    set_enable(GL_DEPTH_TEST, rs[D3DRS_ZENABLE] ? 1 : 0, &C.depth);
    zf = gl_cmp(rs[D3DRS_ZFUNC]);
    if (zf != C.zfunc) { C.zfunc = zf; glDepthFunc(zf); }
    on = rs[D3DRS_ZWRITEENABLE] ? 1 : 0;
    if (on != C.zmask) { C.zmask = on; glDepthMask(on ? GL_TRUE : GL_FALSE); }
    /* Stencil (both faces alike) */
    set_enable(GL_STENCIL_TEST, rs[D3DRS_STENCILENABLE] ? 1 : 0, &C.stencil);
    {
        GLenum f = gl_cmp(rs[D3DRS_STENCILFUNC]);
        GLint ref = (GLint)(rs[D3DRS_STENCILREF] & 0xFF);
        GLuint rm = rs[D3DRS_STENCILMASK] & 0xFF, wm = rs[D3DRS_STENCILWRITEMASK] & 0xFF;
        GLenum sf = gl_stencilop(rs[D3DRS_STENCILFAIL]), zfail = gl_stencilop(rs[D3DRS_STENCILZFAIL]),
               sp = gl_stencilop(rs[D3DRS_STENCILPASS]);
        if (f != C.sfunc || ref != C.sref || rm != C.srmask) {
            C.sfunc = f; C.sref = ref; C.srmask = rm;
            glStencilFunc(f, ref, rm);
        }
        if (sf != C.sfail || zfail != C.szfail || sp != C.spass) {
            C.sfail = sf; C.szfail = zfail; C.spass = sp;
            glStencilOp(sf, zfail, sp);
        }
        if (wm != C.swmask || C.sfail == 0) { C.swmask = wm; glStencilMask(wm); }
    }
    /* Raster: D3DCULL_CW culls clockwise (front, with front = CW). */
    {
        DWORD cmode = rs[D3DRS_CULLMODE];
        GLenum poly = (rs[D3DRS_FILLMODE] == D3DFILL_POINT || rs[D3DRS_FILLMODE] == D3DFILL_WIREFRAME)
                    ? GL_LINE : GL_FILL;
        apply_raster(cmode != D3DCULL_NONE, cmode == D3DCULL_CW ? GL_FRONT : GL_BACK, GL_CW, 0, poly);
    }
    /* Window clip -> scissor, in title pixels scaled to the scene target. */
    set_enable(GL_SCISSOR_TEST, g_wclip_on ? 1 : 0, &C.scissor);
    if (g_wclip_on) {
        float sx, sy;
        GLint l, t, r, b, sc[4];
        d3d8_GetGuestScale(&sx, &sy);
        l = (GLint)(g_wclip.left   * sx + 0.5f);
        t = (GLint)(g_wclip.top    * sy + 0.5f);
        r = (GLint)(g_wclip.right  * sx + 0.5f);
        b = (GLint)(g_wclip.bottom * sy + 0.5f);
        sc[0] = l; sc[1] = (GLint)g_gld.height - b;
        sc[2] = r > l ? r - l : 0; sc[3] = b > t ? b - t : 0;
        if (memcmp(sc, C.sc, sizeof sc)) {
            memcpy(C.sc, sc, sizeof sc);
            glScissor(sc[0], sc[1], sc[2], sc[3]);
        }
    }
    bind_stage_textures();
}

/* ================================================================
 * Streaming vertex / index rings
 * ================================================================ */
#define VB_RING (32u * 1024u * 1024u)
#define IB_RING (8u * 1024u * 1024u)
static GLuint s_vb_ring, s_ib_ring;
static size_t s_vb_off, s_ib_off;

/* Copy into the ring at a multiple of `align`; returns the offset or -1.
 * The buffer must be bound to `target` by the caller. */
static long ring_put(GLenum target, size_t cap, size_t *off, const void *data, size_t size, size_t align)
{
    size_t o = (*off + align - 1) / align * align;
    void *p;
    if (size > cap) return -1;
    if (o + size > cap) {               /* wrap: orphan the storage */
        glBufferData(target, (GLsizeiptr)cap, NULL, GL_STREAM_DRAW);
        o = 0;
    }
    p = glMapBufferRange(target, (GLintptr)o, (GLsizeiptr)size,
                         GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
    if (!p) return -1;
    memcpy(p, data, size);
    glUnmapBuffer(target);
    *off = o + size;
    return (long)o;
}

/* ================================================================
 * Shaders
 * ================================================================ */
static GLuint compile(GLenum type, const char *src, int len, const char *name)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0, n = len;
    glShaderSource(sh, 1, &src, len >= 0 ? &n : NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        static int failures;
        char log[4096];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        if (failures++ < 6) fprintf(stderr, "[GL] %s shader failed:\n%s\n", name, log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint link(GLuint vs, GLuint fs, const char *name)
{
    GLuint p;
    GLint ok = 0;
    if (!vs || !fs) return 0;
    p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDetachShader(p, vs);
    glDetachShader(p, fs);
    if (!ok) {
        static int failures;
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        if (failures++ < 6) fprintf(stderr, "[GL] %s program link failed:\n%s\n", name, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static void bind_samplers(GLuint prog, const char *const names[4])
{
    int i;
    glUseProgram(prog);
    C.prog = prog;
    for (i = 0; i < 4; i++) {
        GLint loc = glGetUniformLocation(prog, names[i]);
        if (loc >= 0) glUniform1i(loc, i);
    }
}

/* ── Fixed function (d3d8_shaders.c) ──────────────────────────────── */
static const char s_ffp_vs[] =
    "#version 330 core\n"
    "layout(location = 0) in vec4 a_pos;\n"
    "layout(location = 1) in vec3 a_normal;\n"
    "layout(location = 2) in vec4 a_diffuse;\n"
    "layout(location = 3) in vec4 a_specular;\n"
    "layout(location = 4) in vec2 a_tex0;\n"
    "layout(location = 5) in vec2 a_tex1;\n"
    "layout(location = 6) in vec2 a_tex2;\n"
    "layout(location = 7) in vec2 a_tex3;\n"
    "uniform mat4 WorldViewProj;\n"
    "uniform mat4 World;\n"
    "uniform mat4 WorldInvTranspose;\n"
    "uniform vec2 ScreenSize;\n"
    "uniform uint Flags;\n"
    "uniform vec4 EyePos;\n"
    "uniform vec4 FogParams;\n"
    "uniform vec4 GlobalAmbient, MatDiffuse, MatAmbient, MatSpecular, MatEmissive;\n"
    "uniform float MatPower;\n"
    "uniform uint NumLights;\n"
    "uniform vec4 LPosType[8], LDirRange[8], LDiffuse[8], LAmbient[8], LSpecular[8], LAtten[8], LSpot[8];\n"
    "out vec4 v_diffuse;\n"
    "out vec4 v_specular;\n"
    "out vec2 v_tex0, v_tex1, v_tex2, v_tex3;\n"
    "out float v_fog;\n"
    "float compute_fog(float dist) {\n"
    "    uint mode = uint(FogParams.w);\n"
    "    if (mode == 1u) return clamp((FogParams.y - dist) / (FogParams.y - FogParams.x), 0.0, 1.0);\n"
    "    if (mode == 2u) return clamp(exp(-FogParams.z * dist), 0.0, 1.0);\n"
    "    if (mode == 3u) { float e = FogParams.z * dist; return clamp(exp(-e * e), 0.0, 1.0); }\n"
    "    return 1.0;\n"
    "}\n"
    "void main() {\n"
    "    v_tex0 = a_tex0; v_tex1 = a_tex1; v_tex2 = a_tex2; v_tex3 = a_tex3;\n"
    "    v_fog = 1.0;\n"
    "    v_specular = vec4(0.0);\n"
    "    if ((Flags & 1u) != 0u) {\n"
    "        gl_Position = vec4(a_pos.x / ScreenSize.x * 2.0 - 1.0, 1.0 - a_pos.y / ScreenSize.y * 2.0,\n"
    "                           a_pos.z * 2.0 - 1.0, 1.0);\n"
    "        v_diffuse = (Flags & 2u) != 0u ? a_diffuse.bgra : vec4(1.0);\n"
    "        if ((Flags & 4u) != 0u) { v_specular = a_specular.bgra; v_fog = a_specular.a; }\n"
    "        return;\n"
    "    }\n"
    "    vec4 p = WorldViewProj * vec4(a_pos.xyz, 1.0);\n"
    "    gl_Position = vec4(p.xy, 2.0 * p.z - p.w, p.w);\n"
    "    vec4 vertDiffuse = (Flags & 2u) != 0u ? a_diffuse.bgra : vec4(1.0);\n"
    "    vec4 vertSpecular = (Flags & 4u) != 0u ? a_specular.bgra : vec4(0.0);\n"
    "    if ((Flags & 0x10u) != 0u && (Flags & 8u) != 0u) {\n"
    "        vec3 worldPos = (World * vec4(a_pos.xyz, 1.0)).xyz;\n"
    "        vec3 worldNormal = normalize(mat3(WorldInvTranspose) * a_normal);\n"
    "        vec3 viewDir = normalize(EyePos.xyz - worldPos);\n"
    "        vec4 litDiffuse = MatEmissive + MatAmbient * GlobalAmbient;\n"
    "        vec4 litSpecular = vec4(0.0);\n"
    "        for (uint i = 0u; i < NumLights && i < 8u; i++) {\n"
    "            vec3 lightDir;\n"
    "            float atten = 1.0, spotFactor = 1.0;\n"
    "            uint ltype = uint(LPosType[i].w);\n"
    "            if (ltype == 3u) {\n"
    "                lightDir = -normalize(LDirRange[i].xyz);\n"
    "            } else {\n"
    "                vec3 toLight = LPosType[i].xyz - worldPos;\n"
    "                float dist = length(toLight);\n"
    "                if (dist > LDirRange[i].w && LDirRange[i].w > 0.0) continue;\n"
    "                lightDir = toLight / max(dist, 0.0001);\n"
    "                atten = 1.0 / (LAtten[i].x + LAtten[i].y * dist + LAtten[i].z * dist * dist);\n"
    "                if (ltype == 2u) {\n"
    "                    float cosAngle = dot(-lightDir, normalize(LDirRange[i].xyz));\n"
    "                    spotFactor = clamp((cosAngle - LSpot[i].y) / max(LSpot[i].x - LSpot[i].y, 0.0001), 0.0, 1.0);\n"
    "                    spotFactor = pow(spotFactor, LAtten[i].w);\n"
    "                }\n"
    "            }\n"
    "            float NdotL = max(dot(worldNormal, lightDir), 0.0);\n"
    "            litDiffuse += atten * spotFactor * (LAmbient[i] * MatAmbient + NdotL * LDiffuse[i] * MatDiffuse);\n"
    "            if (NdotL > 0.0 && MatPower > 0.0) {\n"
    "                vec3 halfVec = normalize(lightDir + viewDir);\n"
    "                float NdotH = max(dot(worldNormal, halfVec), 0.0);\n"
    "                litSpecular += atten * spotFactor * pow(NdotH, MatPower) * LSpecular[i] * MatSpecular;\n"
    "            }\n"
    "        }\n"
    "        v_diffuse = clamp(litDiffuse, 0.0, 1.0);\n"
    "        v_diffuse.a = MatDiffuse.a;\n"
    "        v_specular = clamp(litSpecular, 0.0, 1.0);\n"
    "    } else {\n"
    "        v_diffuse = vertDiffuse;\n"
    "        v_specular = vertSpecular;\n"
    "    }\n"
    "    if (uint(FogParams.w) != 0u) {\n"
    "        vec3 worldPos = (World * vec4(a_pos.xyz, 1.0)).xyz;\n"
    "        v_fog = compute_fog(length(EyePos.xyz - worldPos));\n"
    "    }\n"
    "}\n";

static const char s_ffp_fs[] =
    "#version 330 core\n"
    "uniform sampler2D tex0, tex1, tex2, tex3;\n"
    "uniform vec4 TexFactor;\n"
    "uniform vec4 FogColor;\n"
    "uniform float AlphaRef;\n"
    "uniform uint AlphaFunc;\n"
    "uniform uint PSFlags;\n"
    "uniform uvec4 StageColor[4];\n"
    "uniform uvec4 StageAlpha[4];\n"
    "in vec4 v_diffuse;\n"
    "in vec4 v_specular;\n"
    "in vec2 v_tex0, v_tex1, v_tex2, v_tex3;\n"
    "in float v_fog;\n"
    "layout(location = 0) out vec4 o_color;\n"
    "vec4 resolve_arg(uint arg, vec4 diffuse, vec4 current, vec4 texel, vec4 tfactor, vec4 specular) {\n"
    "    vec4 val;\n"
    "    uint base = arg & 0x0Fu;\n"
    "    if      (base == 0u) val = diffuse;\n"
    "    else if (base == 1u) val = current;\n"
    "    else if (base == 2u) val = texel;\n"
    "    else if (base == 3u) val = tfactor;\n"
    "    else if (base == 4u) val = specular;\n"
    "    else val = current;\n"
    "    if ((arg & 0x10u) != 0u) val = 1.0 - val;\n"
    "    if ((arg & 0x20u) != 0u) val = val.aaaa;\n"
    "    return val;\n"
    "}\n"
    "float apply_op(uint op, float a1, float a2, float diffuse_a, float tex_a, float cur_a, float factor_a) {\n"
    "    if (op <= 1u) return a1;\n"
    "    if (op == 2u) return a1;\n"
    "    if (op == 3u) return a2;\n"
    "    if (op == 4u) return a1 * a2;\n"
    "    if (op == 5u) return clamp(a1 * a2 * 2.0, 0.0, 1.0);\n"
    "    if (op == 6u) return clamp(a1 * a2 * 4.0, 0.0, 1.0);\n"
    "    if (op == 7u) return clamp(a1 + a2, 0.0, 1.0);\n"
    "    if (op == 8u) return clamp(a1 + a2 - 0.5, 0.0, 1.0);\n"
    "    if (op == 9u) return clamp((a1 + a2 - 0.5) * 2.0, 0.0, 1.0);\n"
    "    if (op == 10u) return clamp(a1 - a2, 0.0, 1.0);\n"
    "    if (op == 11u) return clamp(a1 + a2 - a1 * a2, 0.0, 1.0);\n"
    "    if (op == 12u) return mix(a2, a1, diffuse_a);\n"
    "    if (op == 13u) return mix(a2, a1, tex_a);\n"
    "    if (op == 14u) return mix(a2, a1, factor_a);\n"
    "    if (op == 15u) return mix(a2, a1, cur_a);\n"
    "    if (op == 24u) return clamp(4.0 * ((a1 - 0.5) * (a2 - 0.5)), 0.0, 1.0);\n"
    "    return a1 * a2;\n"
    "}\n"
    "void main() {\n"
    "    vec4 current = v_diffuse;\n"
    "    vec4 texels[4];\n"
    "    texels[0] = texture(tex0, v_tex0);\n"
    "    texels[1] = texture(tex1, v_tex1);\n"
    "    texels[2] = texture(tex2, v_tex2);\n"
    "    texels[3] = texture(tex3, v_tex3);\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "        uint colorop = StageColor[i].x;\n"
    "        if (colorop <= 1u) break;\n"
    "        uint alphaop = StageColor[i].w;\n"
    "        vec4 texel = texels[i];\n"
    "        vec4 carg1 = resolve_arg(StageColor[i].y, v_diffuse, current, texel, TexFactor, v_specular);\n"
    "        vec4 carg2 = resolve_arg(StageColor[i].z, v_diffuse, current, texel, TexFactor, v_specular);\n"
    "        vec3 color;\n"
    "        if (colorop == 24u) {\n"
    "            float d = clamp(4.0 * dot(carg1.rgb - 0.5, carg2.rgb - 0.5), 0.0, 1.0);\n"
    "            color = vec3(d);\n"
    "        } else {\n"
    "            color.r = apply_op(colorop, carg1.r, carg2.r, v_diffuse.a, texel.a, current.a, TexFactor.a);\n"
    "            color.g = apply_op(colorop, carg1.g, carg2.g, v_diffuse.a, texel.a, current.a, TexFactor.a);\n"
    "            color.b = apply_op(colorop, carg1.b, carg2.b, v_diffuse.a, texel.a, current.a, TexFactor.a);\n"
    "        }\n"
    "        vec4 aarg1 = resolve_arg(StageAlpha[i].x, v_diffuse, current, texel, TexFactor, v_specular);\n"
    "        vec4 aarg2 = resolve_arg(StageAlpha[i].y, v_diffuse, current, texel, TexFactor, v_specular);\n"
    "        float alpha;\n"
    "        if (alphaop <= 1u) alpha = current.a;\n"
    "        else alpha = apply_op(alphaop, aarg1.a, aarg2.a, v_diffuse.a, texel.a, current.a, TexFactor.a);\n"
    "        current = vec4(color, alpha);\n"
    "    }\n"
    "    if ((PSFlags & 4u) != 0u) current.rgb = clamp(current.rgb + v_specular.rgb, 0.0, 1.0);\n"
    "    if ((PSFlags & 2u) != 0u) current.rgb = mix(FogColor.rgb, current.rgb, v_fog);\n"
    "    if ((PSFlags & 1u) != 0u) {\n"
    "        bool ok = true;\n"
    "        if      (AlphaFunc == 1u) ok = false;\n"
    "        else if (AlphaFunc == 2u) ok = current.a <  AlphaRef;\n"
    "        else if (AlphaFunc == 3u) ok = current.a == AlphaRef;\n"
    "        else if (AlphaFunc == 4u) ok = current.a <= AlphaRef;\n"
    "        else if (AlphaFunc == 5u) ok = current.a >  AlphaRef;\n"
    "        else if (AlphaFunc == 6u) ok = current.a != AlphaRef;\n"
    "        else if (AlphaFunc == 7u) ok = current.a >= AlphaRef;\n"
    "        if (!ok) discard;\n"
    "    }\n"
    "    o_color = current;\n"
    "}\n";

static GLuint s_ffp_prog, s_ffp_vao;
static struct {
    GLint wvp, world, wit, screen, flags, eye, fog, amb, mdif, mamb, mspec, memis, mpow, nl;
    GLint lpos, ldir, ldif, lamb, lspec, latt, lspot;
    GLint tf, fogc, aref, afunc, psflags, scolor, salpha;
} U;

/* ── NV2A pass-through (d3d8_nv2a.c) ──────────────────────────────── */
static const char s_nv_vs[] =
    "#version 330 core\n"
    "layout(location = 0) in vec4 a_pos;\n"
    "layout(location = 1) in vec4 a_d0;\n"
    "layout(location = 2) in vec4 a_d1;\n"
    "layout(location = 3) in float a_fog;\n"
    "layout(location = 4) in vec4 a_t0;\n"
    "layout(location = 5) in vec4 a_t1;\n"
    "layout(location = 6) in vec4 a_t2;\n"
    "layout(location = 7) in vec4 a_t3;\n"
    "uniform vec4 screen;\n"
    "out NvVary {\n"
    "    vec4 d0;\n"
    "    vec4 d1;\n"
    "    float fog;\n"
    "    vec4 t0;\n"
    "    vec4 t1;\n"
    "    vec4 t2;\n"
    "    vec4 t3;\n"
    "} o;\n"
    "void main() {\n"
    /* Undo the divide by w for perspective-correct interpolation; rhw keeps
     * w's sign, so a vertex behind the eye is clipped at the near plane. */
    "    float w = 1.0 / a_pos.w;\n"
    "    gl_Position = vec4((a_pos.x / screen.x * 2.0 - 1.0) * w,\n"
    "                       (1.0 - a_pos.y / screen.y * 2.0) * w,\n"
    "                       (2.0 * a_pos.z - 1.0) * w, w);\n"
    "    o.d0 = a_d0; o.d1 = a_d1; o.fog = a_fog;\n"
    "    o.t0 = a_t0; o.t1 = a_t1; o.t2 = a_t2; o.t3 = a_t3;\n"
    "}\n";

static GLuint s_nv_vs_sh, s_nv_vao, s_nv_ubo;
static GLsizei s_nv_vao_stride;

#define NV_PS_CACHE 2048
#define NV_PS_SLOTS (NV_PS_CACHE * 2)
static struct { unsigned long long key; GLuint prog, fs; GLint screen; int used; } s_ps[NV_PS_SLOTS];
static int s_ps_count;
static void vp_init(void);
static void vp_shutdown(void);

/* ── Blit ─────────────────────────────────────────────────────────── */
static const char s_blit_vs[] =
    "#version 330 core\n"
    "uniform int flip;\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    uv = vec2(p.x, flip != 0 ? 1.0 - p.y : p.y);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char s_blit_fs[] =
    "#version 330 core\n"
    "uniform sampler2D t;\n"
    "uniform sampler2D lut;\n"
    "uniform int use_lut;\n"
    "in vec2 uv;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "    vec3 c = texture(t, uv).rgb;\n"
    "    if (use_lut != 0) {\n"
    "        ivec3 k = ivec3(round(clamp(c, 0.0, 1.0) * 255.0));\n"
    "        c = vec3(texelFetch(lut, ivec2(k.r, 0), 0).r, texelFetch(lut, ivec2(k.g, 0), 0).g,\n"
    "                 texelFetch(lut, ivec2(k.b, 0), 0).b);\n"
    "    }\n"
    "    o = vec4(c, 1.0);\n"
    "}\n";
static GLuint s_blit_prog, s_blit_vao;
static GLint  s_blit_flip, s_blit_uselut;

/* ================================================================
 * Init / shutdown
 * ================================================================ */
static int has_ext(const char *name)
{
    GLint n = 0, i;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (i = 0; i < n; i++) {
        const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (e && !strcmp(e, name)) return 1;
    }
    return 0;
}

int gld_draw_init(void)
{
    static const char *const ffp_names[4] = { "tex0", "tex1", "tex2", "tex3" };
    static const char *const blit_names[4] = { "t", "lut", "", "" };
    GLuint vs, fs;
    GLint major = 0, minor = 0;
    static const unsigned char zero[4] = { 0, 0, 0, 0 };

    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    s_ext_aniso = has_ext("GL_EXT_texture_filter_anisotropic") || has_ext("GL_ARB_texture_filter_anisotropic");
    if (s_ext_aniso) glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &s_max_aniso);
    s_ext_mirror_once = (major > 4 || (major == 4 && minor >= 4)) ||
                        has_ext("GL_ARB_texture_mirror_clamp_to_edge") ||
                        has_ext("GL_EXT_texture_mirror_clamp") || has_ext("GL_ATI_texture_mirror_once");
    if (!has_ext("GL_EXT_texture_compression_s3tc"))
        fprintf(stderr, "[GL] warning: GL_EXT_texture_compression_s3tc missing; DXT textures will be black\n");

    /* fixed function */
    vs = compile(GL_VERTEX_SHADER, s_ffp_vs, -1, "ffp vertex");
    fs = compile(GL_FRAGMENT_SHADER, s_ffp_fs, -1, "ffp pixel");
    s_ffp_prog = link(vs, fs, "ffp");
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    if (!s_ffp_prog) return 0;
    bind_samplers(s_ffp_prog, ffp_names);
#define L(f, n) U.f = glGetUniformLocation(s_ffp_prog, n)
    L(wvp, "WorldViewProj"); L(world, "World"); L(wit, "WorldInvTranspose"); L(screen, "ScreenSize");
    L(flags, "Flags"); L(eye, "EyePos"); L(fog, "FogParams"); L(amb, "GlobalAmbient");
    L(mdif, "MatDiffuse"); L(mamb, "MatAmbient"); L(mspec, "MatSpecular"); L(memis, "MatEmissive");
    L(mpow, "MatPower"); L(nl, "NumLights"); L(lpos, "LPosType"); L(ldir, "LDirRange");
    L(ldif, "LDiffuse"); L(lamb, "LAmbient"); L(lspec, "LSpecular"); L(latt, "LAtten"); L(lspot, "LSpot");
    L(tf, "TexFactor"); L(fogc, "FogColor"); L(aref, "AlphaRef"); L(afunc, "AlphaFunc");
    L(psflags, "PSFlags"); L(scolor, "StageColor"); L(salpha, "StageAlpha");
#undef L
    glGenVertexArrays(1, &s_ffp_vao);

    /* NV2A pass-through vertex stage */
    s_nv_vs_sh = compile(GL_VERTEX_SHADER, s_nv_vs, -1, "nv2a vertex");
    if (!s_nv_vs_sh) return 0;
    glGenVertexArrays(1, &s_nv_vao);
    glGenBuffers(1, &s_nv_ubo);

    /* blit */
    vs = compile(GL_VERTEX_SHADER, s_blit_vs, -1, "blit vertex");
    fs = compile(GL_FRAGMENT_SHADER, s_blit_fs, -1, "blit pixel");
    s_blit_prog = link(vs, fs, "blit");
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    if (!s_blit_prog) return 0;
    bind_samplers(s_blit_prog, blit_names);
    s_blit_flip = glGetUniformLocation(s_blit_prog, "flip");
    s_blit_uselut = glGetUniformLocation(s_blit_prog, "use_lut");
    glGenVertexArrays(1, &s_blit_vao);

    /* rings: the element ring is bound through each VAO */
    glGenBuffers(1, &s_vb_ring);
    glBindBuffer(GL_ARRAY_BUFFER, s_vb_ring);
    glBufferData(GL_ARRAY_BUFFER, VB_RING, NULL, GL_STREAM_DRAW);
    glGenBuffers(1, &s_ib_ring);
    glBindVertexArray(s_ffp_vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ib_ring);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, IB_RING, NULL, GL_STREAM_DRAW);
    vp_init();
    glBindVertexArray(0);

    glGenTextures(1, &s_dummy_tex);
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, s_dummy_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glActiveTexture(GL_TEXTURE0);

    glDisable(GL_DITHER);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gld_state_invalidate();
    gl_check("init");
    return 1;
}

void gld_draw_shutdown(void)
{
    int i;
    vp_shutdown();
    for (i = 0; i < NV_PS_SLOTS; i++) {
        if (s_ps[i].prog) glDeleteProgram(s_ps[i].prog);
        if (s_ps[i].fs) glDeleteShader(s_ps[i].fs);
    }
    memset(s_ps, 0, sizeof s_ps);
    s_ps_count = 0;
    for (i = 0; i < s_nsmp; i++) glDeleteSamplers(1, &s_smp[i].s);
    s_nsmp = 0;
    if (s_ffp_prog) glDeleteProgram(s_ffp_prog);
    if (s_blit_prog) glDeleteProgram(s_blit_prog);
    if (s_nv_vs_sh) glDeleteShader(s_nv_vs_sh);
    s_ffp_prog = s_blit_prog = s_nv_vs_sh = 0;
    gld_state_invalidate();
}

/* ================================================================
 * Blit (present scaling, video upload)
 * ================================================================ */
void gld_blit(GLuint tex, int x, int y, int w, int h, GLuint lut_tex, int flip_y)
{
    if (!s_blit_prog || !tex || w <= 0 || h <= 0) return;
    glViewport(x, y, w, h);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_CLAMP);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(s_blit_prog);
    glUniform1i(s_blit_flip, flip_y ? 1 : 0);
    glUniform1i(s_blit_uselut, lut_tex ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, lut_tex ? lut_tex : s_dummy_tex);
    glBindSampler(1, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(s_blit_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    gld_state_invalidate();
    gl_check("blit");
}

/* ================================================================
 * Fixed-function draws
 * ================================================================ */
static void mat4_mul(float *out, const float *a, const float *b)
{
    float r[16];
    int i, j, k;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = s;
        }
    memcpy(out, r, sizeof r);
}

static void mat4_transpose(float *out, const float *m)
{
    float r[16];
    int i, j;
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) r[j * 4 + i] = m[i * 4 + j];
    memcpy(out, r, sizeof r);
}

static int mat4_inverse(float *out, const float *m)
{
    float inv[16], det;
    int i;
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f) {
        for (i = 0; i < 16; i++) out[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        return 0;
    }
    det = 1.0f / det;
    for (i = 0; i < 16; i++) out[i] = inv[i] * det;
    return 1;
}

static void argb_to_vec4(DWORD c, float *v)
{
    v[0] = ((c >> 16) & 0xFF) / 255.0f;
    v[1] = ((c >> 8) & 0xFF) / 255.0f;
    v[2] = (c & 0xFF) / 255.0f;
    v[3] = ((c >> 24) & 0xFF) / 255.0f;
}

/* Upload the constants d3d8_shaders_prepare_draw puts in its buffers. D3D
 * matrices are row-major for row vectors; given to GL untransposed, a GLSL
 * `M * v` computes D3D's `v * M`. */
static void ffp_uniforms(DWORD fvf)
{
    const DWORD *rs = g_gld.rs;
    GLuint flags = 0;
    float v[4];
    UINT st;

    if (fvf & D3DFVF_XYZRHW) {
        flags = 1;
        glUniform2f(U.screen, (float)d3d8_GetBackbufferWidth(), (float)d3d8_GetBackbufferHeight());
    } else {
        float wv[16], wvp[16], wit[16], vinv[16];
        const float *world = (const float *)&g_gld.xforms[D3DTS_WORLD];
        mat4_mul(wv, world, (const float *)&g_gld.xforms[D3DTS_VIEW]);
        mat4_mul(wvp, wv, (const float *)&g_gld.xforms[D3DTS_PROJECTION]);
        glUniformMatrix4fv(U.wvp, 1, GL_FALSE, wvp);
        glUniformMatrix4fv(U.world, 1, GL_FALSE, world);
        mat4_inverse(wit, world);
        mat4_transpose(wit, wit);
        glUniformMatrix4fv(U.wit, 1, GL_FALSE, wit);
        mat4_inverse(vinv, (const float *)&g_gld.xforms[D3DTS_VIEW]);
        glUniform4f(U.eye, vinv[12], vinv[13], vinv[14], 1.0f);
        glUniform2f(U.screen, (float)d3d8_GetBackbufferWidth(), (float)d3d8_GetBackbufferHeight());
    }
    if (fvf & D3DFVF_DIFFUSE) flags |= 0x02;
    if (fvf & D3DFVF_SPECULAR) flags |= 0x04;
    if (fvf & D3DFVF_NORMAL) flags |= 0x08;
    if (rs[D3DRS_LIGHTING]) flags |= 0x10;
    glUniform1ui(U.flags, flags);
    if (rs[D3DRS_FOGENABLE]) {
        float fs, fe, fd;
        memcpy(&fs, &rs[D3DRS_FOGSTART], 4);
        memcpy(&fe, &rs[D3DRS_FOGEND], 4);
        memcpy(&fd, &rs[D3DRS_FOGDENSITY], 4);
        glUniform4f(U.fog, fs, fe, fd, (float)rs[D3DRS_FOGTABLEMODE]);
    } else {
        glUniform4f(U.fog, 0.0f, 0.0f, 0.0f, 0.0f);
    }
    /* lighting (only read when the FVF has normals and lighting is on) */
    if ((flags & 0x18) == 0x18) {
        const D3DMATERIAL8 *m = &g_gld.material;
        float pos[8][4], dir[8][4], dif[8][4], amb[8][4], spe[8][4], att[8][4], spot[8][4];
        UINT i, n = 0;
        argb_to_vec4(rs[D3DRS_AMBIENT], v);
        glUniform4fv(U.amb, 1, v);
        glUniform4fv(U.mdif, 1, &m->Diffuse.r);
        glUniform4fv(U.mamb, 1, &m->Ambient.r);
        glUniform4fv(U.mspec, 1, &m->Specular.r);
        glUniform4fv(U.memis, 1, &m->Emissive.r);
        glUniform1f(U.mpow, m->Power);
        for (i = 0; i < GLD_MAX_LIGHTS && n < 8; i++) {
            const D3DLIGHT8 *l = &g_gld.lights[i];
            if (!g_gld.light_enable[i]) continue;
            pos[n][0] = l->Position.x; pos[n][1] = l->Position.y; pos[n][2] = l->Position.z; pos[n][3] = (float)l->Type;
            dir[n][0] = l->Direction.x; dir[n][1] = l->Direction.y; dir[n][2] = l->Direction.z; dir[n][3] = l->Range;
            memcpy(dif[n], &l->Diffuse, 16);
            memcpy(amb[n], &l->Ambient, 16);
            memcpy(spe[n], &l->Specular, 16);
            att[n][0] = l->Attenuation0; att[n][1] = l->Attenuation1; att[n][2] = l->Attenuation2; att[n][3] = l->Falloff;
            spot[n][0] = cosf(l->Theta * 0.5f); spot[n][1] = cosf(l->Phi * 0.5f); spot[n][2] = spot[n][3] = 0.0f;
            n++;
        }
        glUniform1ui(U.nl, n);
        if (n) {
            glUniform4fv(U.lpos, (GLsizei)n, pos[0]);
            glUniform4fv(U.ldir, (GLsizei)n, dir[0]);
            glUniform4fv(U.ldif, (GLsizei)n, dif[0]);
            glUniform4fv(U.lamb, (GLsizei)n, amb[0]);
            glUniform4fv(U.lspec, (GLsizei)n, spe[0]);
            glUniform4fv(U.latt, (GLsizei)n, att[0]);
            glUniform4fv(U.lspot, (GLsizei)n, spot[0]);
        }
    }
    /* pixel constants */
    argb_to_vec4(rs[D3DRS_TEXTUREFACTOR], v);
    glUniform4fv(U.tf, 1, v);
    {
        GLuint psf = 0;
        if (rs[D3DRS_FOGENABLE]) {
            argb_to_vec4(rs[D3DRS_FOGCOLOR], v);
            v[3] = 1.0f;
            glUniform4fv(U.fogc, 1, v);
            psf |= 2;
        }
        if (rs[D3DRS_ALPHATESTENABLE]) {
            glUniform1f(U.aref, (float)rs[D3DRS_ALPHAREF] / 255.0f);
            glUniform1ui(U.afunc, rs[D3DRS_ALPHAFUNC]);
            psf |= 1;
        }
        if (rs[D3DRS_SPECULARENABLE]) psf |= 4;
        glUniform1ui(U.psflags, psf);
    }
    {
        GLuint sc[4][4], sa[4][4];
        memset(sa, 0, sizeof sa);
        for (st = 0; st < 4; st++) {
            const DWORD *tss = g_gld.tss[st];
            sc[st][0] = tss[D3DTSS_COLOROP];
            sc[st][1] = tss[D3DTSS_COLORARG1];
            sc[st][2] = tss[D3DTSS_COLORARG2];
            sc[st][3] = tss[D3DTSS_ALPHAOP];
            sa[st][0] = tss[D3DTSS_ALPHAARG1];
            sa[st][1] = tss[D3DTSS_ALPHAARG2];
        }
        glUniform4uiv(U.scolor, 4, sc[0]);
        glUniform4uiv(U.salpha, 4, sa[0]);
    }
}

/* Attribute layout for an FVF (same packing as d3d8_shaders.c's input layouts). */
static void ffp_layout(DWORD fvf, GLsizei stride)
{
    static DWORD last_fvf = 0xFFFFFFFFu;
    static GLsizei last_stride = -1;
    GLuint off = 0;
    UINT ntex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT, i;
    if (fvf == last_fvf && stride == last_stride) return;
    last_fvf = fvf; last_stride = stride;
    glBindBuffer(GL_ARRAY_BUFFER, s_vb_ring);
    if (fvf & D3DFVF_XYZRHW) {
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, stride, (const void *)(uintptr_t)off);
        off += 16;
    } else if (fvf & D3DFVF_XYZ) {
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void *)(uintptr_t)off);
        off += 12;
    } else {
        glDisableVertexAttribArray(0);
        glVertexAttrib4f(0, 0.0f, 0.0f, 0.0f, 1.0f);
    }
    if (fvf & D3DFVF_NORMAL) {
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (const void *)(uintptr_t)off);
        off += 12;
    } else {
        glDisableVertexAttribArray(1);
        glVertexAttrib4f(1, 0.0f, 0.0f, 1.0f, 0.0f);
    }
    if (fvf & D3DFVF_DIFFUSE) {
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (const void *)(uintptr_t)off);
        off += 4;
    } else {
        glDisableVertexAttribArray(2);
        glVertexAttrib4f(2, 1.0f, 1.0f, 1.0f, 1.0f);
    }
    if (fvf & D3DFVF_SPECULAR) {
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (const void *)(uintptr_t)off);
        off += 4;
    } else {
        glDisableVertexAttribArray(3);
        glVertexAttrib4f(3, 0.0f, 0.0f, 0.0f, 0.0f);
    }
    for (i = 0; i < 4; i++) {
        if (i < ntex) {
            glEnableVertexAttribArray(4 + i);
            glVertexAttribPointer(4 + i, 2, GL_FLOAT, GL_FALSE, stride, (const void *)(uintptr_t)off);
            off += 8;
        } else {
            glDisableVertexAttribArray(4 + i);
            glVertexAttrib4f(4 + i, 0.0f, 0.0f, 0.0f, 1.0f);
        }
    }
}

static GLenum gl_prim(D3DPRIMITIVETYPE pt, UINT count, UINT *nverts)
{
    switch (pt) {
    case D3DPT_TRIANGLELIST:  *nverts = count * 3; return GL_TRIANGLES;
    case D3DPT_TRIANGLESTRIP: *nverts = count + 2; return GL_TRIANGLE_STRIP;
    case D3DPT_TRIANGLEFAN:   *nverts = count + 2; return GL_TRIANGLE_FAN;
    case D3DPT_LINELIST:      *nverts = count * 2; return GL_LINES;
    case D3DPT_LINESTRIP:     *nverts = count + 1; return GL_LINE_STRIP;
    case D3DPT_POINTLIST:     *nverts = count;     return GL_POINTS;
    case D3DPT_QUADLIST:      *nverts = count * 4; return GL_TRIANGLES;  /* expanded below */
    default:                  *nverts = 0;         return GL_POINTS;
    }
}

static UINT fvf_stride(DWORD fvf)
{
    UINT s = 0;
    if (fvf & D3DFVF_XYZ) s += 12;
    if (fvf & D3DFVF_XYZRHW) s += 16;
    if (fvf & D3DFVF_NORMAL) s += 12;
    if (fvf & D3DFVF_DIFFUSE) s += 4;
    if (fvf & D3DFVF_SPECULAR) s += 4;
    s += ((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT) * 8;
    return s;
}

HRESULT gld_draw_up(D3DPRIMITIVETYPE prim, UINT prim_count, const void *verts, UINT stride,
                    const void *indices, int index32, UINT index_count)
{
    DWORD fvf = g_gld.vertex_shader;
    UINT nv = 0, nidx = 0, maxv = 0, i;
    GLenum mode;
    long voff;
    void *tmp = NULL;
    const void *idx = indices;

    if (!g_gld.initialized || !gld_on_gl_thread() || !verts || !stride || !s_ffp_prog) return E_FAIL;
    if (!(fvf & (D3DFVF_XYZ | D3DFVF_XYZRHW))) fvf |= D3DFVF_XYZRHW;   /* not an FVF: as pre-transformed */
    if (fvf_stride(fvf) > stride) return E_INVALIDARG;
    mode = gl_prim(prim, prim_count, &nv);
    if (!nv) return E_INVALIDARG;

    if (idx) {
        nidx = index_count;
        for (i = 0; i < nidx; i++) {
            UINT k = index32 ? ((const uint32_t *)idx)[i] : ((const uint16_t *)idx)[i];
            if (k > maxv) maxv = k;
        }
        if (prim == D3DPT_QUADLIST) {   /* 4 indices per quad -> 6 */
            uint32_t *q = (uint32_t *)malloc((size_t)prim_count * 6 * 4);
            if (!q) return E_OUTOFMEMORY;
            for (i = 0; i < prim_count; i++) {
                UINT b = i * 4, k;
                UINT v4[4];
                for (k = 0; k < 4; k++)
                    v4[k] = index32 ? ((const uint32_t *)idx)[b + k] : ((const uint16_t *)idx)[b + k];
                q[i * 6 + 0] = v4[0]; q[i * 6 + 1] = v4[1]; q[i * 6 + 2] = v4[2];
                q[i * 6 + 3] = v4[0]; q[i * 6 + 4] = v4[2]; q[i * 6 + 5] = v4[3];
            }
            tmp = q; idx = q; index32 = 1; nidx = prim_count * 6;
        }
        nv = maxv + 1;
    } else if (prim == D3DPT_QUADLIST) {
        const BYTE *s = (const BYTE *)verts;
        BYTE *d = (BYTE *)malloc((size_t)prim_count * 6 * stride);
        static const int order[6] = { 0, 1, 2, 0, 2, 3 };
        if (!d) return E_OUTOFMEMORY;
        for (i = 0; i < prim_count; i++) {
            int k;
            for (k = 0; k < 6; k++)
                memcpy(d + ((size_t)i * 6 + k) * stride, s + ((size_t)i * 4 + order[k]) * stride, stride);
        }
        tmp = d; verts = d; nv = prim_count * 6;
    }

    gld_states_apply();
    use_program(s_ffp_prog);
    bind_vao(s_ffp_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vb_ring);
    voff = ring_put(GL_ARRAY_BUFFER, VB_RING, &s_vb_off, verts, (size_t)nv * stride, stride);
    if (voff < 0) { free(tmp); return E_OUTOFMEMORY; }
    ffp_layout(fvf, (GLsizei)stride);
    ffp_uniforms(fvf);
    if (idx) {
        long ioff;
        size_t isz = index32 ? 4 : 2;
        ioff = ring_put(GL_ELEMENT_ARRAY_BUFFER, IB_RING, &s_ib_off, idx, nidx * isz, 4);
        if (ioff < 0) { free(tmp); return E_OUTOFMEMORY; }
        glDrawElementsBaseVertex(mode, (GLsizei)nidx, index32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                                 (const void *)(uintptr_t)ioff, (GLint)(voff / (long)stride));
    } else {
        glDrawArrays(mode, (GLint)(voff / (long)stride), (GLsizei)nv);
    }
    free(tmp);
    gl_check("draw_up");
    return S_OK;
}

/* ================================================================
 * NV2A-native draws
 * ================================================================ */
static int ps_slot(unsigned long long key, int insert)
{
    unsigned h = (unsigned)((key ^ (key >> 29) ^ (key >> 47)) & (NV_PS_SLOTS - 1));
    int n;
    for (n = 0; n < NV_PS_SLOTS; n++, h = (h + 1) & (NV_PS_SLOTS - 1)) {
        if (!s_ps[h].used) return insert ? (int)h : -1;
        if (s_ps[h].key == key) return (int)h;
    }
    return -1;
}

int d3d8_nv2a_has_ps(unsigned long long key)
{
    int i = ps_slot(key, 0);
    return i >= 0 && s_ps[i].prog != 0;
}

/* `src` is GLSL 330 (nv2a_psh_generate_glsl), not HLSL, on this backend. */
int d3d8_nv2a_add_ps(unsigned long long key, const char *src, int len)
{
    static const char *const names[4] = { "tex0", "tex1", "tex2", "tex3" };
    GLuint fs, prog;
    GLuint blk;
    int slot;
    Uint64 t0;

    if (!g_gld.initialized || !gld_on_gl_thread() || !s_nv_vs_sh || s_ps_count >= NV_PS_CACHE) return 0;
    slot = ps_slot(key, 1);
    if (slot < 0) return 0;
    if (s_ps[slot].used && s_ps[slot].prog) return 1;
    t0 = SDL_GetPerformanceCounter();
    fs = compile(GL_FRAGMENT_SHADER, src, len, "nv2a pixel");
    prog = link(s_nv_vs_sh, fs, "nv2a");
    g_nv_compiles++;
    g_nv_compile_ms += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 /
                       (double)SDL_GetPerformanceFrequency();
    if (!prog) {
        static int dumped;
        if (fs) glDeleteShader(fs);
        if (dumped++ < 2) fprintf(stderr, "[NV2A-PSH] pixel shader %016llX source:\n%.*s\n", key, len, src);
        return 0;
    }
    blk = glGetUniformBlockIndex(prog, "PshConsts");
    if (blk != GL_INVALID_INDEX) glUniformBlockBinding(prog, blk, 0);
    bind_samplers(prog, names);
    s_ps[slot].key = key;
    s_ps[slot].prog = prog;
    s_ps[slot].fs = fs;                 /* kept: linked again with program vertex shaders */
    s_ps[slot].screen = glGetUniformLocation(prog, "screen");
    s_ps[slot].used = 1;
    s_ps_count++;
    gl_check("add_ps");
    return 1;
}

static void nv_layout(GLsizei stride)
{
    static const struct { int size; GLuint off; } a[8] = {
        { 4, 0 }, { 4, 16 }, { 4, 32 }, { 1, 48 }, { 4, 52 }, { 4, 68 }, { 4, 84 }, { 4, 100 },
    };
    int i;
    if (s_nv_vao_stride == stride) return;
    s_nv_vao_stride = stride;
    glBindBuffer(GL_ARRAY_BUFFER, s_vb_ring);
    for (i = 0; i < 8; i++) {
        glEnableVertexAttribArray((GLuint)i);
        glVertexAttribPointer((GLuint)i, a[i].size, GL_FLOAT, GL_FALSE, stride,
                              (const void *)(uintptr_t)a[i].off);
    }
}

HRESULT d3d8_nv2a_draw(D3DPRIMITIVETYPE prim, UINT prim_count, const void *verts, UINT stride,
                       unsigned long long ps_key, const void *ps_consts, UINT ps_consts_size,
                       int raster)
{
    int slot = ps_slot(ps_key, 0);
    GLenum mode;
    UINT nv;
    long off;
    float sw, sh;

    if (!g_gld.initialized || !gld_on_gl_thread() || slot < 0 || !s_ps[slot].prog) return E_FAIL;
    if (!verts || stride < 116) return E_INVALIDARG;
    switch (prim) {
    case D3DPT_TRIANGLELIST: mode = GL_TRIANGLES;  nv = prim_count * 3; break;
    case D3DPT_LINELIST:     mode = GL_LINES;      nv = prim_count * 2; break;
    case D3DPT_LINESTRIP:    mode = GL_LINE_STRIP; nv = prim_count + 1; break;
    case D3DPT_POINTLIST:    mode = GL_POINTS;     nv = prim_count;     break;
    default: return E_INVALIDARG;
    }
    if (!nv) return S_OK;

    gld_states_apply();                 /* blend, depth, stencil, scissor, samplers */
    /* raster bits: 0 depth clip, 2:1 cull (1 front, 2 back), 3 front CCW */
    {
        int cull = (raster >> 1) & 3;
        apply_raster(cull == 1 || cull == 2, cull == 1 ? GL_FRONT : GL_BACK,
                     (raster & 8) ? GL_CCW : GL_CW, (raster & 1) ? 0 : 1, GL_FILL);
    }
    use_program(s_ps[slot].prog);
    sw = (float)d3d8_GetBackbufferWidth();
    sh = (float)d3d8_GetBackbufferHeight();
    if (sw <= 0.0f) sw = 640.0f;
    if (sh <= 0.0f) sh = 480.0f;
    glUniform4f(s_ps[slot].screen, sw, sh, 0.0f, 0.0f);
    glBindBuffer(GL_UNIFORM_BUFFER, s_nv_ubo);
    glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)(ps_consts_size < 512 ? 512 : ps_consts_size), NULL, GL_STREAM_DRAW);
    if (ps_consts && ps_consts_size)
        glBufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)ps_consts_size, ps_consts);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, s_nv_ubo);

    bind_vao(s_nv_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vb_ring);
    off = ring_put(GL_ARRAY_BUFFER, VB_RING, &s_vb_off, verts, (size_t)nv * stride, stride);
    if (off < 0) return E_OUTOFMEMORY;
    nv_layout((GLsizei)stride);
    if (g_gpuprof_on) gpuprof_draw_begin();
    glDrawArrays(mode, (GLint)(off / (long)stride), (GLsizei)nv);
    if (g_gpuprof_on) gpuprof_draw_end();
    gl_check("nv2a_draw");
    return S_OK;
}

/* ================================================================
 * Vertex programs on the GPU (d3d8_nv2a.c's job on D3D11)
 *
 * gpu_prepare() in the translator hands over the decoded program and the
 * title's vertex arrays; the program becomes a GLSL vertex shader
 * (d3d8_gl_vsh.c), cached by program and input kinds, and is linked with
 * the draw's combiner pixel shader. Its 192 constants and per-draw
 * parameters are std140 blocks (bindings 1 and 2; the pixel constants
 * keep 0). An array with stride 0 (one element for every vertex) is read
 * through an instance divisor, since a GL stride of 0 means "packed".
 * Points answer 0 here and stay on the CPU path, as does any program whose
 * shader does not compile.
 * ================================================================ */
int nv2a_vsh_glsl(const vshcpu_insn *p, int n, uint16_t inputs, const uint8_t kind[16],
                  char *buf, int cap);

#define VP_CACHE 1024
#define VP_SLOTS (VP_CACHE * 2)
#define VP_LINKS 4096
#define VP_LINK_SLOTS (VP_LINKS * 2)
static struct { unsigned long long key; GLuint vs; int used; } s_vp[VP_SLOTS];
static struct { unsigned long long key; GLuint prog; int used; } s_vpl[VP_LINK_SLOTS];
static int s_nvp, s_nvpl;
static GLuint s_vp_vao, s_vp_cubo, s_vp_pubo;
static unsigned s_vp_enabled, s_vp_divisor;     /* attribute arrays on, instanced */
static float s_vp_c[192][4];
static int s_vp_c_valid;
static struct { unsigned long long hash; uint8_t kind[16]; int slot; } s_vp_last = { 0, { 0 }, -1 };
static struct { GLuint vs; int ps; GLuint prog; } s_vpl_last;

static void vp_init(void)
{
    glGenVertexArrays(1, &s_vp_vao);
    glBindVertexArray(s_vp_vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ib_ring);
    glGenBuffers(1, &s_vp_cubo);
    glBindBuffer(GL_UNIFORM_BUFFER, s_vp_cubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof s_vp_c, NULL, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &s_vp_pubo);
    glBindBuffer(GL_UNIFORM_BUFFER, s_vp_pubo);
    glBufferData(GL_UNIFORM_BUFFER, 21 * 16, NULL, GL_STREAM_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    s_vp_enabled = s_vp_divisor = 0;
    s_vp_c_valid = 0;
}

static void vp_shutdown(void)
{
    int i;
    for (i = 0; i < VP_LINK_SLOTS; i++) if (s_vpl[i].prog) glDeleteProgram(s_vpl[i].prog);
    for (i = 0; i < VP_SLOTS; i++) if (s_vp[i].vs) glDeleteShader(s_vp[i].vs);
    memset(s_vp, 0, sizeof s_vp);
    memset(s_vpl, 0, sizeof s_vpl);
    s_nvp = s_nvpl = 0;
    s_vp_last.slot = -1;
    memset(&s_vpl_last, 0, sizeof s_vpl_last);
    if (s_vp_vao) glDeleteVertexArrays(1, &s_vp_vao);
    if (s_vp_cubo) glDeleteBuffers(1, &s_vp_cubo);
    if (s_vp_pubo) glDeleteBuffers(1, &s_vp_pubo);
    s_vp_vao = s_vp_cubo = s_vp_pubo = 0;
}

static unsigned long long vp_fnv(unsigned long long h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--) { h ^= *b++; h *= 1099511628211ull; }
    return h;
}

static unsigned vp_hash(unsigned long long key, unsigned mask)
{
    return (unsigned)((key ^ (key >> 29) ^ (key >> 47)) & mask);
}

/* The vertex shader of this draw's program and input kinds; 0 if it does
 * not compile (remembered, so the program stays on the CPU). */
static GLuint vp_shader(const Nv2aVshDraw *d)
{
    static char src[262144];
    uint8_t kind[16];
    unsigned long long key;
    unsigned h;
    int a, n, len;
    Uint64 t0;

    for (a = 0; a < 16; a++) kind[a] = (d->inputs & (1u << a)) ? d->attr[a].kind : 0xFF;
    if (s_vp_last.slot >= 0 && s_vp_last.hash == d->prog_hash && !memcmp(s_vp_last.kind, kind, 16))
        return s_vp[s_vp_last.slot].vs;
    key = vp_fnv(d->prog_hash, kind, 16);
    h = vp_hash(key, VP_SLOTS - 1);
    for (n = 0; n < VP_SLOTS; n++, h = (h + 1) & (VP_SLOTS - 1)) {
        if (!s_vp[h].used) break;
        if (s_vp[h].key == key) goto found;
    }
    if (n == VP_SLOTS || s_nvp >= VP_CACHE) return 0;
    s_vp[h].key = key;
    s_vp[h].used = 1;
    s_vp[h].vs = 0;
    s_nvp++;
    len = nv2a_vsh_glsl(d->prog, d->prog_len, d->inputs, kind, src, (int)sizeof src);
    if (len > 0) {
        t0 = SDL_GetPerformanceCounter();
        s_vp[h].vs = compile(GL_VERTEX_SHADER, src, len, "nv2a vertex program");
        g_nv_compiles++;
        g_nv_compile_ms += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 /
                           (double)SDL_GetPerformanceFrequency();
        if (!s_vp[h].vs) {
            static int dumped;
            if (dumped++ < 2) fprintf(stderr, "[NV2A-VSH] vertex program %016llX source:\n%.*s\n", key, len, src);
        }
    }
found:
    s_vp_last.hash = d->prog_hash;
    memcpy(s_vp_last.kind, kind, 16);
    s_vp_last.slot = (int)h;
    return s_vp[h].vs;
}

/* The program of vertex shader `vs` and pixel-shader slot `ps`. */
static GLuint vp_program(GLuint vs, int ps)
{
    static const char *const names[4] = { "tex0", "tex1", "tex2", "tex3" };
    unsigned long long key = ((unsigned long long)vs << 32) ^ (unsigned long long)(unsigned)ps;
    unsigned h = vp_hash(key * 0x9E3779B97F4A7C15ull, VP_LINK_SLOTS - 1);
    GLuint prog, blk;
    int n;

    if (s_vpl_last.prog && s_vpl_last.vs == vs && s_vpl_last.ps == ps) return s_vpl_last.prog;
    for (n = 0; n < VP_LINK_SLOTS; n++, h = (h + 1) & (VP_LINK_SLOTS - 1)) {
        if (!s_vpl[h].used) break;
        if (s_vpl[h].key == key) {
            prog = s_vpl[h].prog;
            goto done;
        }
    }
    if (n == VP_LINK_SLOTS || s_nvpl >= VP_LINKS) return 0;
    prog = link(vs, s_ps[ps].fs, "nv2a program");
    s_vpl[h].key = key;
    s_vpl[h].prog = prog;
    s_vpl[h].used = 1;
    s_nvpl++;
    if (prog) {
        if ((blk = glGetUniformBlockIndex(prog, "PshConsts")) != GL_INVALID_INDEX) glUniformBlockBinding(prog, blk, 0);
        if ((blk = glGetUniformBlockIndex(prog, "NvConsts")) != GL_INVALID_INDEX) glUniformBlockBinding(prog, blk, 1);
        if ((blk = glGetUniformBlockIndex(prog, "NvParams")) != GL_INVALID_INDEX) glUniformBlockBinding(prog, blk, 2);
        bind_samplers(prog, names);
    }
done:
    if (prog) { s_vpl_last.vs = vs; s_vpl_last.ps = ps; s_vpl_last.prog = prog; }
    return prog;
}

/* GL attribute layout of a DXGI element format (nv2a_dxgi_shim.h numbers). */
static int vp_format(uint32_t f, GLint *size, GLenum *type, GLboolean *norm, int *integer)
{
    *norm = GL_FALSE;
    *integer = 0;
    switch (f) {
    case 41: *size = 1; *type = GL_FLOAT; break;                 /* R32_FLOAT */
    case 16: *size = 2; *type = GL_FLOAT; break;                 /* R32G32_FLOAT */
    case 6:  *size = 3; *type = GL_FLOAT; break;                 /* R32G32B32_FLOAT */
    case 2:  *size = 4; *type = GL_FLOAT; break;                 /* R32G32B32A32_FLOAT */
    case 61: *size = 1; *type = GL_UNSIGNED_BYTE; *norm = GL_TRUE; break;   /* R8_UNORM */
    case 49: *size = 2; *type = GL_UNSIGNED_BYTE; *norm = GL_TRUE; break;   /* R8G8_UNORM */
    case 28: *size = 4; *type = GL_UNSIGNED_BYTE; *norm = GL_TRUE; break;   /* R8G8B8A8_UNORM */
    case 87: *size = GL_BGRA; *type = GL_UNSIGNED_BYTE; *norm = GL_TRUE; break; /* B8G8R8A8_UNORM */
    case 58: *size = 1; *type = GL_SHORT; *norm = GL_TRUE; break;           /* R16_SNORM */
    case 37: *size = 2; *type = GL_SHORT; *norm = GL_TRUE; break;           /* R16G16_SNORM */
    case 13: *size = 4; *type = GL_SHORT; *norm = GL_TRUE; break;           /* R16G16B16A16_SNORM */
    case 59: *size = 1; *type = GL_SHORT; *integer = 1; break;              /* R16_SINT */
    case 38: *size = 2; *type = GL_SHORT; *integer = 1; break;              /* R16G16_SINT */
    case 14: *size = 4; *type = GL_SHORT; *integer = 1; break;              /* R16G16B16A16_SINT */
    case 42: *size = 1; *type = GL_UNSIGNED_INT; *integer = 1; break;       /* R32_UINT */
    default: return 0;
    }
    return 1;
}

int d3d8_nv2a_vsh_ready(const Nv2aVshDraw *d)
{
    int a;
    GLint size;
    GLenum type;
    GLboolean norm;
    int integer;
    if (!g_gld.initialized || !gld_on_gl_thread() || !s_vp_vao) return 0;
    if (d->topology == D3DPT_POINTLIST) return 0;
    for (a = 0; a < 16; a++)
        if ((d->inputs & (1u << a)) && (d->attr[a].kind & 0x0F) != NV2A_VSH_IN_CONST &&
            !vp_format(d->attr[a].dxgi_format, &size, &type, &norm, &integer))
            return 0;
    return vp_shader(d) != 0;
}

int d3d8_nv2a_draw_program_gpu(const Nv2aVshDraw *d)
{
    struct { const uint8_t *base; uint32_t stride, size; long off; } grp[16];
    int gi[16];
    struct { float screen[4], fog[4], flags[4], vattr[16][4], point[4], hud[4]; } params;
    int slot = ps_slot(d->ps_key, 0);
    int a, g, ngrp = 0;
    unsigned enabled = 0, divisor = 0;
    uint16_t done = 0;
    GLuint vs, prog;
    GLenum mode;
    long ioff;
    double pt = 0.0;                    /* XBOX_PERF sub-zones */

    if (!g_gld.initialized || !gld_on_gl_thread() || !s_vp_vao || slot < 0 || !s_ps[slot].fs ||
        !d->nindices)
        return 0;
    switch (d->topology) {
    case D3DPT_TRIANGLELIST: mode = GL_TRIANGLES;  break;
    case D3DPT_LINELIST:     mode = GL_LINES;      break;
    case D3DPT_LINESTRIP:    mode = GL_LINE_STRIP; break;
    default: return 0;
    }
    if (!(vs = vp_shader(d)) || !(prog = vp_program(vs, slot))) return 0;

    /* Arrays interleaved in one (same stride, within one vertex of each
     * other) share an upload; taken in address order so each group starts
     * at its lowest element. */
    for (;;) {
        int best = -1;
        for (a = 0; a < 16; a++)
            if ((d->inputs & (1u << a)) && !(done & (1u << a)) &&
                (d->attr[a].kind & 0x0F) != NV2A_VSH_IN_CONST &&
                (best < 0 || d->attr[a].base < d->attr[best].base))
                best = a;
        if (best < 0) break;
        done |= (uint16_t)(1u << best);
        a = best;
        for (g = 0; g < ngrp; g++)
            if (grp[g].stride == d->attr[a].stride && grp[g].stride &&
                d->attr[a].base >= grp[g].base &&
                (uint32_t)(d->attr[a].base - grp[g].base) < grp[g].stride)
                break;
        if (g == ngrp) {
            grp[g].base = d->attr[a].base;
            grp[g].stride = d->attr[a].stride;
            grp[g].size = 0;
            ngrp++;
        }
        {
            uint32_t end = (uint32_t)(d->attr[a].base - grp[g].base) + d->attr[a].bytes;
            if (end > grp[g].size) grp[g].size = end;
        }
        gi[a] = g;
    }

    if (g_perf_on) pt = perf_now();
    gld_states_apply();                 /* blend, depth, stencil, scissor, samplers */
    {
        int cull = (d->raster >> 1) & 3;
        apply_raster(cull == 1 || cull == 2, cull == 1 ? GL_FRONT : GL_BACK,
                     (d->raster & 8) ? GL_CCW : GL_CW, (d->raster & 1) ? 0 : 1, GL_FILL);
    }
    use_program(prog);
    bind_vao(s_vp_vao);

    if (g_perf_on) { double t = perf_now(); perf_add(PZ_DSTATE, t - pt); pt = t; }
    glBindBuffer(GL_ARRAY_BUFFER, s_vb_ring);
    {   /* all groups in one stretch of the ring: a wrap between them would
         * leave the earlier ones in the orphaned storage */
        size_t total = 0;
        for (g = 0; g < ngrp; g++) total += ((size_t)grp[g].size + 15u) & ~(size_t)15u;
        if (total > VB_RING) return 0;
        if (((s_vb_off + 15u) & ~(size_t)15u) + total > VB_RING) {
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)VB_RING, NULL, GL_STREAM_DRAW);
            s_vb_off = 0;
        }
    }
    for (g = 0; g < ngrp; g++) {
        grp[g].off = ring_put(GL_ARRAY_BUFFER, VB_RING, &s_vb_off, grp[g].base, grp[g].size, 16);
        if (grp[g].off < 0) return 0;
    }
    for (a = 0; a < 16; a++) {
        GLint size;
        GLenum type;
        GLboolean norm;
        int integer;
        const void *ptr;
        if (!(d->inputs & (1u << a)) || (d->attr[a].kind & 0x0F) == NV2A_VSH_IN_CONST) continue;
        if (!vp_format(d->attr[a].dxgi_format, &size, &type, &norm, &integer)) return 0;
        g = gi[a];
        ptr = (const void *)(uintptr_t)(grp[g].off + (d->attr[a].base - grp[g].base));
        if (integer) glVertexAttribIPointer((GLuint)a, size, type, (GLsizei)d->attr[a].stride, ptr);
        else glVertexAttribPointer((GLuint)a, size, type, norm, (GLsizei)d->attr[a].stride, ptr);
        enabled |= 1u << a;
        if (!d->attr[a].stride) divisor |= 1u << a;
    }
    for (a = 0; a < 16; a++) {
        unsigned bit = 1u << a;
        if ((enabled ^ s_vp_enabled) & bit) {
            if (enabled & bit) glEnableVertexAttribArray((GLuint)a);
            else glDisableVertexAttribArray((GLuint)a);
        }
        if ((divisor ^ s_vp_divisor) & bit) glVertexAttribDivisor((GLuint)a, (divisor & bit) ? 1 : 0);
    }
    s_vp_enabled = enabled;
    s_vp_divisor = divisor;
    ioff = ring_put(GL_ELEMENT_ARRAY_BUFFER, IB_RING, &s_ib_off, d->indices, (size_t)d->nindices * 4, 4);
    if (ioff < 0) return 0;
    if (g_perf_on) { double t = perf_now(); perf_add(PZ_DUP, t - pt); pt = t; }

    if (!s_vp_c_valid || memcmp(s_vp_c, d->vconst, sizeof s_vp_c)) {
        memcpy(s_vp_c, d->vconst, sizeof s_vp_c);
        glBindBuffer(GL_UNIFORM_BUFFER, s_vp_cubo);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof s_vp_c, s_vp_c);
        s_vp_c_valid = 1;
    }
    memset(&params, 0, sizeof params);
    params.screen[0] = d->screen_w;
    params.screen[1] = d->screen_h;
    params.screen[2] = d->clip_max;
    params.screen[3] = d->point_zoom;
    params.fog[0] = (float)d->fog_mode;
    params.fog[1] = d->fog_p0;
    params.fog[2] = d->fog_p1;
    params.flags[0] = d->specular ? 1.0f : 0.0f;
    params.flags[1] = d->spec_alpha ? 1.0f : 0.0f;
    params.hud[0] = d->hud_kx;
    params.hud[1] = d->hud_ax;
    params.hud[2] = d->hud_ky;
    params.hud[3] = d->hud_ay;
    if (d->attr_const) memcpy(params.vattr, d->attr_const, sizeof params.vattr);
    glBindBuffer(GL_UNIFORM_BUFFER, s_vp_pubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof params, &params, GL_STREAM_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, s_nv_ubo);
    glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)(d->ps_consts_size < 512 ? 512 : d->ps_consts_size), NULL, GL_STREAM_DRAW);
    if (d->ps_consts && d->ps_consts_size)
        glBufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)d->ps_consts_size, d->ps_consts);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, s_nv_ubo);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, s_vp_cubo);
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, s_vp_pubo);
    if (g_perf_on) { double t = perf_now(); perf_add(PZ_DCB, t - pt); pt = t; }

    if (g_gpuprof_on) gpuprof_draw_begin();
    if (divisor)
        glDrawElementsInstanced(mode, (GLsizei)d->nindices, GL_UNSIGNED_INT, (const void *)(uintptr_t)ioff, 1);
    else
        glDrawElements(mode, (GLsizei)d->nindices, GL_UNSIGNED_INT, (const void *)(uintptr_t)ioff);
    if (g_gpuprof_on) gpuprof_draw_end();
    gl_check("nv2a_draw_program_gpu");
    if (g_perf_on) { perf_add(PZ_DDRAW, perf_now() - pt); perf_gpu_draw(0, (unsigned)ngrp); }
    return 1;
}

int d3d8_points_gpu_on(void) { return 0; }       /* point sprites from the CPU path */
int d3d8_points_check_on(void) { return 0; }     /* diagnostic of the GPU point path */
void d3d8_nv2a_points_expect(const void *verts, unsigned n, unsigned stride)
{ (void)verts; (void)n; (void)stride; }

/* ================================================================
 * Pump optimisation switches (shared with the translator)
 * ================================================================ */
static unsigned g_pump_frame;
static int pump_alt_off(void)
{
    static int alt = -1;
    if (alt < 0) { const char *e = getenv("XBOX_FIX_PUMP_ALT"); alt = e && e[0] == '1'; }
    return alt && (g_pump_frame & 1u);
}
void d3d8_pump_frame_tick(void) { g_pump_frame++; }
int  d3d8_pump_alt_off(void) { return pump_alt_off(); }

int d3d8_pump_cache_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("XBOX_FIX_PUMP_CACHE"); on = !(e && e[0] == '0'); }
    return on && !pump_alt_off();
}
