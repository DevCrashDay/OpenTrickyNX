/*
 * vk_smoke_test.c -- the Vulkan renderer (XBOX_VULKAN) without the game: a
 * device, a clear, fixed-function draws (pre-transformed, textured, blended,
 * depth tested, quads, indexed), an NV2A-native draw with a generated
 * combiner shader, a GPU vertex program, a DXT1 texture, transformed and
 * indexed draws, a present and readbacks of the scene; `msaa` as the first
 * argument runs it all with 4x multisampling. Prints FAIL lines
 * and exits non-zero on a wrong pixel. On Linux with Mesa's lavapipe:
 *
 *   cmake -S xboxrecomp -B build-vk -DXBOX_VULKAN=ON -DXBOX_BUILD_TESTS=ON
 *   cmake --build build-vk --target vk_smoke_test
 *   XBOX_VK_VALIDATE=1 xvfb-run -a build-vk/src/d3d/vk_smoke_test
 */
#include "d3d8_xbox.h"
#include "../nv2a/nv2a_psh.h"
#include "d3d8_nv2a_vsh.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>

/* the parts of xbox_perf.c the D3D layer reads */
int g_perf_on;
double perf_now(void) { return 0.0; }
void perf_add(int zone, double ms) { (void)zone; (void)ms; }
void perf_gpu_draw(int a, unsigned b) { (void)a; (void)b; }
void perf_present_done(void) {}

unsigned d3d8_DebugPeekScene(float fx, float fy);
int d3d8_nv2a_vsh_ready(const Nv2aVshDraw *d);     /* declared where nv2a_pgraph_d3d11.c uses it */

static int fails;

static int near(unsigned a, unsigned b, int tol)
{
    int i;
    for (i = 0; i < 32; i += 8) {
        int d = (int)((a >> i) & 0xFF) - (int)((b >> i) & 0xFF);
        if (d < -tol || d > tol) return 0;
    }
    return 1;
}

static void expect(const char *what, float fx, float fy, unsigned want, int tol)
{
    unsigned got = d3d8_DebugPeekScene(fx, fy) | 0xFF000000u;
    want |= 0xFF000000u;
    if (!near(got, want, tol)) { printf("FAIL %s: (%.2f, %.2f) is %08X, want %08X\n", what, fx, fy, got, want); fails++; }
    else printf("ok   %s: %08X\n", what, got);
}

typedef struct { float x, y, z, rhw; DWORD c; float u, v; } TV;
#define FVF_TV (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static void quad(TV *q, float x0, float y0, float x1, float y1, float z, DWORD c)
{
    TV v[4] = {
        { x0, y0, z, 1.0f, c, 0.0f, 0.0f }, { x1, y0, z, 1.0f, c, 1.0f, 0.0f },
        { x1, y1, z, 1.0f, c, 1.0f, 1.0f }, { x0, y1, z, 1.0f, c, 0.0f, 1.0f },
    };
    memcpy(q, v, sizeof v);
}

/* oDst = src, one MAC MOV from input v[vidx] */
static void vp_mov(vshcpu_insn *i, int vidx, int out)
{
    int k;
    memset(i, 0, sizeof *i);
    i->mac = 1;                         /* MAC_MOV */
    i->vidx = (uint8_t)vidx;
    i->src[0].mux = 2;                  /* MUX_V */
    for (k = 0; k < 4; k++) i->src[0].swz[k] = (uint8_t)k;
    i->o_mask = 0xF;
    i->orb = 1;
    i->out_addr = (uint8_t)out;
}

int main(int argc, char **argv)
{
    IDirect3D8 *d3d;
    IDirect3DDevice8 *dev = NULL;
    IDirect3DTexture8 *tex = NULL;
    D3DPRESENT_PARAMETERS pp;
    D3DLOCKED_RECT lr;
    IDirect3DTexture8 *dxt = NULL;
    TV q[4];
    unsigned long long comb_key = 0;
    HRESULT hr;
    int frame, x, y;

    if (argc > 1 && !strcmp(argv[1], "msaa")) d3d8_SetMsaa(4);
    d3d8_SetHostDisplay(640, 480, 0, 0);
    d3d = xbox_Direct3DCreate8(0);
    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = 640;
    pp.BackBufferHeight = 480;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.BackBufferCount = 1;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.Windowed = TRUE;
    hr = d3d->lpVtbl->CreateDevice(d3d, 0, 1, NULL, 0, &pp, &dev);
    if (hr != S_OK || !dev) { printf("FAIL CreateDevice %08lX\n", (unsigned long)hr); return 1; }

    /* a 4x4 texture: left half red, right half blue (linear, not swizzled) */
    hr = dev->lpVtbl->CreateTexture(dev, 4, 4, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_MANAGED, &tex);
    if (hr != S_OK || !tex) { printf("FAIL CreateTexture %08lX\n", (unsigned long)hr); return 1; }
    tex->lpVtbl->LockRect(tex, 0, &lr, NULL, 0);
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            ((DWORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = x < 2 ? 0xFFFF0000u : 0xFF0000FFu;
    tex->lpVtbl->UnlockRect(tex, 0);
    /* a DXT1 block, all colour 0 = pure green */
    hr = dev->lpVtbl->CreateTexture(dev, 4, 4, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &dxt);
    if (hr != S_OK || !dxt) { printf("FAIL CreateTexture DXT1 %08lX\n", (unsigned long)hr); return 1; }
    dxt->lpVtbl->LockRect(dxt, 0, &lr, NULL, 0);
    {
        static const unsigned char blk[8] = { 0xE0, 0x07, 0x00, 0x00, 0, 0, 0, 0 };
        memcpy(lr.pBits, blk, 8);
    }
    dxt->lpVtbl->UnlockRect(dxt, 0);

    for (frame = 0; frame < 3; frame++) {
        dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF204060u, 1.0f, 0);
        if (frame == 2) expect("clear", 0.05f, 0.05f, 0x204060, 2);
        dev->lpVtbl->SetVertexShader(dev, FVF_TV);
        dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, TRUE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING, FALSE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);

        /* untextured green quad, top left */
        dev->lpVtbl->SetTexture(dev, 0, NULL);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        quad(q, 0, 0, 320, 240, 0.5f, 0xFF00FF00u);
        dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_QUADLIST, 1, q, sizeof(TV));

        /* a nearer-z quad behind it in draw order but in front in depth: wins */
        quad(q, 0, 0, 160, 120, 0.25f, 0xFFFFFF00u);
        dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLEFAN, 2, q, sizeof(TV));
        /* a farther one: loses */
        quad(q, 0, 0, 80, 60, 0.75f, 0xFFFF00FFu);
        dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLEFAN, 2, q, sizeof(TV));

        /* textured quad, top right, point sampled */
        dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture8 *)tex);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_MINFILTER, D3DTEXF_POINT);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
        dev->lpVtbl->SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        quad(q, 320, 0, 640, 240, 0.5f, 0xFFFFFFFFu);
        dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLEFAN, 2, q, sizeof(TV));

        /* 50 % white blended over the bottom-left clear colour */
        dev->lpVtbl->SetTexture(dev, 0, NULL);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        quad(q, 0, 240, 320, 480, 0.5f, 0x80FFFFFFu);
        dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLEFAN, 2, q, sizeof(TV));
        dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);

        /* NV2A-native: final combiner out = v0 (diffuse), bottom right */
        {
            Nv2aPshState st;
            Nv2aPshConsts pc;
            static char src[65536];
            unsigned long long key;
            float v[6][29];
            static const float corner[6][2] = { {320,240},{640,240},{640,480},{320,240},{640,480},{320,480} };
            int i, len;
            memset(&st, 0, sizeof st);
            st.simple = 0;
            st.combiner_control = 1;          /* one general stage, its output unused */
            /* final combiner: A=0 B=0 C=0 D=v0 (reg 4) -> out = D */
            st.final0 = 0x00000004u;
            st.final1 = 0;
            key = nv2a_psh_key(&st);
            comb_key = key;
            if (!d3d8_nv2a_has_ps(key)) {
                len = nv2a_psh_generate_glsl(&st, src, (int)sizeof src);
                if (len <= 0 || !d3d8_nv2a_add_ps(key, src, len)) { printf("FAIL nv2a pixel shader\n"); fails++; }
            }
            memset(&pc, 0, sizeof pc);
            memset(v, 0, sizeof v);
            for (i = 0; i < 6; i++) {
                v[i][0] = corner[i][0]; v[i][1] = corner[i][1]; v[i][2] = 0.5f; v[i][3] = 1.0f;
                v[i][4] = 1.0f; v[i][5] = 0.5f; v[i][6] = 0.0f; v[i][7] = 1.0f;   /* d0 orange */
            }
            dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
            hr = d3d8_nv2a_draw(D3DPT_TRIANGLELIST, 2, v, 116, key, &pc, sizeof pc, 0x01);
            if (hr != S_OK) { printf("FAIL nv2a draw %08lX\n", (unsigned long)hr); fails++; }
        }

        /* DXT1 texture, top-right strip */
        dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
        dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture8 *)dxt);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        quad(q, 480, 0, 640, 60, 0.5f, 0xFFFFFFFFu);
        dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLEFAN, 2, q, sizeof(TV));
        dev->lpVtbl->SetTexture(dev, 0, NULL);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);

        /* transformed, indexed: clip-space square x,y in [0.1, 0.3] (y up), identity matrices */
        {
            struct { float x, y, z; DWORD c; } tv[4] = {
                { 0.1f, 0.1f, 0.5f, 0xFF800080u }, { 0.3f, 0.1f, 0.5f, 0xFF800080u },
                { 0.3f, 0.3f, 0.5f, 0xFF800080u }, { 0.1f, 0.3f, 0.5f, 0xFF800080u },
            };
            static const WORD idx[6] = { 0, 1, 2, 0, 2, 3 };
            D3DMATRIX m;
            memset(&m, 0, sizeof m);
            ((float *)&m)[0] = ((float *)&m)[5] = ((float *)&m)[10] = ((float *)&m)[15] = 1.0f;
            dev->lpVtbl->SetTransform(dev, D3DTS_WORLD, &m);
            dev->lpVtbl->SetTransform(dev, D3DTS_VIEW, &m);
            dev->lpVtbl->SetTransform(dev, D3DTS_PROJECTION, &m);
            dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZ | D3DFVF_DIFFUSE);
            hr = dev->lpVtbl->DrawIndexedPrimitiveUP(dev, D3DPT_TRIANGLELIST, 0, 4, 2, idx, D3DFMT_INDEX16, tv, 16);
            if (hr != S_OK) { printf("FAIL indexed draw %08lX\n", (unsigned long)hr); fails++; }
            dev->lpVtbl->SetVertexShader(dev, FVF_TV);
        }

        /* GPU vertex program: oPos = v0 (screen pixels), oD0 = v3 (B8G8R8A8) */
        {
            static vshcpu_insn prog[2];
            static const float vconst[192][4];
            static const float vattr[16][4];
            static struct { float x, y, z, w; DWORD c; } pv[4] = {
                { 200, 300, 8000000, 1, 0xFF00FFFFu }, { 300, 300, 8000000, 1, 0xFF00FFFFu },
                { 300, 400, 8000000, 1, 0xFF00FFFFu }, { 200, 400, 8000000, 1, 0xFF00FFFFu },
            };
            static const uint32_t pidx[6] = { 0, 1, 2, 0, 2, 3 };
            static Nv2aPshConsts pc;
            Nv2aVshDraw d;
            vp_mov(&prog[0], 3, 3);         /* oD0 = v3 */
            vp_mov(&prog[1], 0, 0);         /* oPos = v0 */
            prog[1].final = 1;
            memset(&d, 0, sizeof d);
            d.prog = prog;
            d.prog_len = 2;
            d.prog_hash = 0x1234;
            d.inputs = (1u << 0) | (1u << 3);
            d.attr[0].kind = NV2A_VSH_IN_FLOAT; d.attr[0].dxgi_format = 2;  d.attr[0].stride = 20;
            d.attr[0].base = (const uint8_t *)&pv[0].x; d.attr[0].bytes = sizeof pv;
            d.attr[3].kind = NV2A_VSH_IN_FLOAT; d.attr[3].dxgi_format = 87; d.attr[3].stride = 20;
            d.attr[3].base = (const uint8_t *)&pv[0].c; d.attr[3].bytes = sizeof pv - 16;
            d.attr_const = vattr;
            d.vconst = vconst;
            d.screen_w = 640; d.screen_h = 480; d.clip_max = 16777215.0f; d.point_zoom = 1.0f;
            d.topology = D3DPT_TRIANGLELIST;
            d.indices = pidx; d.nindices = 6;
            d.ps_key = comb_key;
            d.ps_consts = &pc; d.ps_consts_size = sizeof pc;
            d.raster = 0x01;
            if (!d3d8_nv2a_vsh_ready(&d)) { printf("FAIL vertex program not ready\n"); fails++; }
            else if (!d3d8_nv2a_draw_program_gpu(&d)) { printf("FAIL vertex program draw\n"); fails++; }

            /* the same program over two points, indexed out of order: 40-pixel
             * squares, drawn as instances */
            {
                static struct { float x, y, z, w; DWORD c; } pt[2] = {
                    { 64, 240, 8000000, 1, 0xFF20C040u }, { 576, 288, 8000000, 1, 0xFFC02060u },
                };
                static const uint32_t ptidx[2] = { 1, 0 };
                d.attr[0].base = (const uint8_t *)&pt[0].x; d.attr[0].bytes = sizeof pt;
                d.attr[3].base = (const uint8_t *)&pt[0].c; d.attr[3].bytes = sizeof pt - 16;
                d.topology = D3DPT_POINTLIST;
                d.indices = ptidx; d.nindices = 2;
                d.point_size = 40.0f; d.point_kx = 1.0f;
                if (!d3d8_nv2a_vsh_ready(&d)) { printf("FAIL points not ready\n"); fails++; }
                else if (!d3d8_nv2a_draw_program_gpu(&d)) { printf("FAIL points draw\n"); fails++; }
            }
        }

        if (frame == 2) {
            expect("dxt1 texture", 0.9f, 0.05f, 0x00FF00, 2);
            expect("transformed indexed", 0.6f, 0.4f, 0x800080, 2);
            expect("vertex program", 0.39f, 0.73f, 0x00FFFF, 2);
            expect("point sprite a", 0.1f + 15.0f / 640.0f, 0.5f - 15.0f / 480.0f, 0x20C040, 2);
            expect("point sprite b", 0.9f - 15.0f / 640.0f, 0.6f + 15.0f / 480.0f, 0xC02060, 2);
            {   /* and no wider than its 40 pixels */
                unsigned got = d3d8_DebugPeekScene(0.1f + 23.0f / 640.0f, 0.5f) | 0xFF000000u;
                if (near(got, 0xFF20C040u, 2)) { printf("FAIL point sprite a too wide\n"); fails++; }
                else printf("ok   point sprite a size\n");
            }
            expect("depth-nearer quad", 0.05f, 0.05f, 0xFFFF00, 2);
            expect("ffp quad", 0.4f, 0.4f, 0x00FF00, 2);
            expect("texture left texel", 0.55f, 0.2f, 0xFF0000, 2);
            expect("texture right texel", 0.95f, 0.2f, 0x0000FF, 2);
            expect("alpha blend", 0.2f, 0.8f, 0x90A0B0, 4);
            expect("nv2a combiner", 0.8f, 0.8f, 0xFF8000, 3);
        }
        d3d8_PresentFrame();
    }
    tex->lpVtbl->Release(tex);
    dxt->lpVtbl->Release(dxt);
    dev->lpVtbl->Release(dev);
    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
