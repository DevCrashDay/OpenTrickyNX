/*
 * gl_vsh_test.c -- the GLSL vertex programs of d3d8_gl_vsh.c against the CPU
 * interpreter (nv2a_vsh_cpu.c): random programs over random vertices, the
 * GPU's outputs read back with transform feedback. Needs an EGL driver that
 * runs without a window (Mesa's llvmpipe will do):
 *
 *   cc -O1 -o gl_vsh_test gl_vsh_test.c ../d3d8_gl_vsh.c ../../nv2a/nv2a_vsh_cpu.c \
 *      -I.. -I../../nv2a -lEGL -lGL -lm
 *   EGL_PLATFORM=surfaceless ./gl_vsh_test 1000
 *
 * GLSL leaves NaN, infinity and the sign of zero to the implementation, so
 * vertices whose program goes through them can differ (about 0.1 % of these
 * random ones on llvmpipe); every other output must match. SANE=1 keeps to
 * the ILU ops that make fewer of them, TRIAL=n prints program n and its GLSL.
 */
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3d8_nv2a_vsh.h"

int nv2a_vsh_glsl(const vshcpu_insn *p, int n, uint16_t inputs, const uint8_t kind[16], char *buf, int cap);

static unsigned rs = 12345;
static unsigned rnd(void) { rs = rs * 1103515245u + 12345u; return rs >> 8; }
static float frnd(float lo, float hi) { return lo + (hi - lo) * (rnd() % 100000) / 100000.0f; }

#define NV 64
static int same(float a, float b)
{
    if (isnan(a) && isnan(b)) return 1;
    if (isinf(a) || isinf(b)) return a == b;
    float d = fabsf(a - b), m = fmaxf(fabsf(a), fabsf(b));
    return d <= 1e-3f * fmaxf(m, 1.0f);
}

static float fogf_c(float d, float p0, float p1)
{   /* mode 1 (linear) */
    float f;
    if (isinf(d)) d = 0;
    f = p0 + d * p1 - 1.0f;
    if (isnan(f)) return 1;
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

int main(int argc, char **argv)
{
    int trials = argc > 1 ? atoi(argv[1]) : 300, t, bad = 0, nanpos = 0, compared = 0;
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj, min;
    static const EGLint cattr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                                    EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
    EGLConfig cfg; EGLint ncfg; static const EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
    if (!eglInitialize(dpy, &maj, &min)) { printf("no egl\n"); return 1; }
    eglBindAPI(EGL_OPENGL_API);
    eglChooseConfig(dpy, ca, &cfg, 1, &ncfg);
    EGLContext ctx = eglCreateContext(dpy, ncfg ? cfg : NULL, EGL_NO_CONTEXT, cattr);
    if (!ctx || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { printf("no ctx\n"); return 1; }
    printf("GL %s / %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    GLuint vao, vbo[3], tfb, cubo, pubo, fbo, rb;
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 4, 4);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(3, vbo); glGenBuffers(1, &tfb); glGenBuffers(1, &cubo); glGenBuffers(1, &pubo);

    for (t = 0; t < trials; t++) {
        vshcpu_insn p[24];
        int n = 3 + rnd() % 20, k, a, v;
        uint8_t kind[16];
        uint16_t inputs = 0;
        static float c[192][4], in[NV][16][4], cpu[NV][16][4];
        static uint32_t cmp[NV]; static int16_t s16[NV][4];
        static char src[262144];
        memset(p, 0, sizeof p);
        for (k = 0; k < n; k++) {
            vshcpu_insn *i = &p[k];
            i->mac = rnd() % 14; i->ilu = rnd() % 8;
            if (rnd() % 4 == 0) i->ilu = 0;
            if (getenv("SANE")) { static const int ok[4] = { 0, 1, 5, 3 }; i->ilu = ok[rnd() % 4]; }
            i->cidx = rnd() % 200; i->vidx = rnd() % 16;
            for (a = 0; a < 3; a++) {
                i->src[a].mux = 1 + rnd() % 3; i->src[a].reg = rnd() % 16; i->src[a].neg = rnd() % 2;
                for (v = 0; v < 4; v++) i->src[a].swz[v] = rnd() % 4;
            }
            i->mac_mask = rnd() % 16; i->ilu_mask = rnd() % 16; i->out_r = rnd() % 16;
            i->o_mask = rnd() % 16; i->orb = 1; i->out_addr = rnd() % 16; i->out_mux = rnd() % 2;
            i->a0x = (rnd() % 5 == 0);
        }
        /* end with a position write so most vertices land */
        p[n - 1].final = 1;
        for (a = 0; a < 16; a++) {
            kind[a] = 0xFF;
            if (rnd() % 3) { inputs |= 1u << a; kind[a] = NV2A_VSH_IN_FLOAT; }
        }
        inputs |= 1u << 1; kind[1] = NV2A_VSH_IN_CMP;
        inputs |= 1u << 2; kind[2] = NV2A_VSH_IN_INT;
        inputs |= 1u << 3; kind[3] = NV2A_VSH_IN_CONST;
        for (k = 0; k < 192; k++) for (v = 0; v < 4; v++) c[k][v] = frnd(-3, 3);
        float vattr[16][4] = { { 0 } };
        for (v = 0; v < 4; v++) vattr[3][v] = frnd(-2, 2);
        for (v = 0; v < NV; v++) {
            for (a = 0; a < 16; a++) {
                int j;
                for (j = 0; j < 4; j++) in[v][a][j] = (inputs & (1u << a)) ? frnd(-4, 4) : (j == 3 ? 1.0f : 0.0f);
            }
            cmp[v] = rnd() ^ (rnd() << 16);
            in[v][1][0] = (float)(((int32_t)(cmp[v] << 21)) >> 21) / 1023.0f;
            in[v][1][1] = (float)(((int32_t)(cmp[v] << 10)) >> 21) / 1023.0f;
            in[v][1][2] = (float)(((int32_t)cmp[v]) >> 22) / 511.0f;
            in[v][1][3] = 1.0f;
            for (a = 0; a < 4; a++) { s16[v][a] = (int16_t)(rnd() % 2000 - 1000); in[v][2][a] = s16[v][a]; }
            memcpy(in[v][3], vattr[3], 16);
            float cc[192][4]; memcpy(cc, c, sizeof cc);
            vshcpu_run(p, n, (const float (*)[4])in[v], cc, cpu[v]);
        }
        int len = nv2a_vsh_glsl(p, n, inputs, kind, src, sizeof src);
        if (len <= 0) { printf("gen fail\n"); return 1; }
        if (getenv("TRIAL") && atoi(getenv("TRIAL")) == t) {
            char l[256]; for (k = 0; k < n; k++) { vshcpu_format(&p[k], l, sizeof l); printf("%2d %s\n", k, l); }
            printf("%s\n", src);
            printf("v0 cpu O3 %g %g %g %g\n", cpu[0][3][0], cpu[0][3][1], cpu[0][3][2], cpu[0][3][3]);
        }
        const char *s = src;
        GLuint sh = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(sh, 1, &s, &len); glCompileShader(sh);
        GLint ok; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[4096]; glGetShaderInfoLog(sh, sizeof log, NULL, log); printf("compile:\n%s\n%s\n", log, src); return 1; }
        GLuint pr = glCreateProgram(); glAttachShader(pr, sh);
        static const char *vary[] = { "gl_Position", "NvVary.d0", "NvVary.d1", "NvVary.fog", "NvVary.t0", "NvVary.t1", "NvVary.t2", "NvVary.t3" };
        glTransformFeedbackVaryings(pr, 8, vary, GL_INTERLEAVED_ATTRIBS);
        glLinkProgram(pr); glGetProgramiv(pr, GL_LINK_STATUS, &ok);
        if (!ok) { char log[4096]; glGetProgramInfoLog(pr, sizeof log, NULL, log); printf("link:\n%s\n", log); return 1; }
        glUniformBlockBinding(pr, glGetUniformBlockIndex(pr, "NvConsts"), 1);
        glUniformBlockBinding(pr, glGetUniformBlockIndex(pr, "NvParams"), 2);
        glUseProgram(pr);
        struct { float screen[4], fog[4], flags[4], vattr[16][4], point[4], hud[4]; } par;
        memset(&par, 0, sizeof par);
        par.screen[0] = 640; par.screen[1] = 480; par.screen[2] = 16777215.0f; par.screen[3] = 1;
        par.fog[0] = 1; par.fog[1] = 0.7f; par.fog[2] = 0.01f; par.flags[0] = 1; par.flags[1] = 1;
        memcpy(par.vattr, vattr, sizeof vattr);
        glBindBuffer(GL_UNIFORM_BUFFER, cubo); glBufferData(GL_UNIFORM_BUFFER, sizeof c, c, GL_STATIC_DRAW);
        glBindBuffer(GL_UNIFORM_BUFFER, pubo); glBufferData(GL_UNIFORM_BUFFER, sizeof par, &par, GL_STATIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 1, cubo); glBindBufferBase(GL_UNIFORM_BUFFER, 2, pubo);
        /* float inputs: 16 vec4 per vertex in vbo0 */
        static float fl[NV][16][4]; memcpy(fl, in, sizeof fl);
        glBindBuffer(GL_ARRAY_BUFFER, vbo[0]); glBufferData(GL_ARRAY_BUFFER, sizeof fl, fl, GL_STATIC_DRAW);
        for (a = 0; a < 16; a++) {
            if (!(inputs & (1u << a)) || kind[a] != NV2A_VSH_IN_FLOAT) continue;
            glEnableVertexAttribArray(a);
            glVertexAttribPointer(a, 4, GL_FLOAT, GL_FALSE, 256, (void *)(uintptr_t)(a * 16));
        }
        glBindBuffer(GL_ARRAY_BUFFER, vbo[1]); glBufferData(GL_ARRAY_BUFFER, sizeof cmp, cmp, GL_STATIC_DRAW);
        glEnableVertexAttribArray(1); glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, 4, 0);
        glBindBuffer(GL_ARRAY_BUFFER, vbo[2]); glBufferData(GL_ARRAY_BUFFER, sizeof s16, s16, GL_STATIC_DRAW);
        glEnableVertexAttribArray(2); glVertexAttribIPointer(2, 4, GL_SHORT, 8, 0);
        glDisableVertexAttribArray(3);
        static float out[NV][29];
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, tfb);
        glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, sizeof out, NULL, GL_STREAM_READ);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, tfb);
        glEnable(GL_RASTERIZER_DISCARD);
        glBeginTransformFeedback(GL_POINTS); glDrawArrays(GL_POINTS, 0, NV); glEndTransformFeedback();
        { GLenum er = glGetError(); if (er && t < 3) printf("glerr %x\n", er); }
        glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof out, out);
        for (a = 0; a < 16; a++) glDisableVertexAttribArray(a);
        for (v = 0; v < NV; v++) {
            float (*O)[4] = cpu[v], e[29];
            float x = truncf(O[0][0] * 16) / 16, y = truncf(O[0][1] * 16) / 16;
            float z = O[0][2] / par.screen[2], w = O[0][3], rhw, wc;
            int j;
            if (w >= 0) w = fminf(fmaxf(w, 5.421011e-20f), 1.8446744e19f); else w = fminf(fmaxf(w, -1.8446744e19f), -5.421011e-20f);
            rhw = 1.0f / w; wc = 1.0f / rhw;
            e[0] = (x / 640 * 2 - 1) * wc; e[1] = (1 - y / 480 * 2) * wc; e[2] = (2 * z - 1) * wc; e[3] = wc;
            if (!(isfinite(x) && isfinite(y) && isfinite(z) && isfinite(rhw))) { e[0] = e[1] = e[2] = e[3] = NAN; nanpos++; }
            for (j = 0; j < 4; j++) { e[4 + j] = isnan(O[3][j]) ? 1 : O[3][j]; e[8 + j] = isnan(O[4][j]) ? 1 : O[4][j]; }
            e[12] = fogf_c(O[5][0], 0.7f, 0.01f);
            for (j = 0; j < 4; j++) { e[13 + j] = O[9][j]; e[17 + j] = O[10][j]; e[21 + j] = O[11][j]; e[25 + j] = O[12][j]; }
            if (isnan(e[0])) {      /* culled: only the position matters */
                if (!isnan(out[v][0])) { if (bad++ < 10) printf("trial %d v%d: pos should be NaN, got %g\n", t, v, out[v][0]); }
                continue;
            }
            compared++;
            for (j = 0; j < 29; j++)
                if (!same(e[j], out[v][j])) {
                    if (!isfinite(e[j]) || !isfinite(out[v][j])) { static int nf; nf++; if (getenv("NF")) printf("nonfinite t%d v%d o%d cpu %g gpu %g\n", t, v, j, e[j], out[v][j]); break; }
                    if (bad++ < 10) printf("trial %d v%d out %d: cpu %g gpu %g\n", t, v, j, e[j], out[v][j]);
                    break;
                }
        }
        glDeleteProgram(pr); glDeleteShader(sh);
    }
    printf("%d trials, %d vertices compared, %d culled, %d mismatches\n", trials, compared, nanpos, bad);
    return bad != 0;
}
