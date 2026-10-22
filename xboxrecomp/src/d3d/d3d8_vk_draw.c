/*
 * d3d8_vk_draw.c -- drawing on Vulkan (XBOX_VULKAN).
 *
 * d3d8_gl_draw.c on Vulkan:
 *   - Render states -> the pipeline key (blend, colour mask, fill mode,
 *     depth clamp) and dynamic state (depth, stencil, cull, winding,
 *     topology, viewport, scissor from the NV2A window clip), and per-stage
 *     samplers built from the texture stage states as the D3D11 ones are.
 *   - The fixed-function path (d3d8_shaders.c in GLSL 450): FVF vertices,
 *     pre-transformed or transformed and lit, the D3D8 texture-stage blend
 *     ops, fog, specular and alpha test. The translator's 2D and fallback
 *     draws come through here.
 *   - The NV2A-native path (d3d8_nv2a.c): the perspective-correct pass-
 *     through vertex shader over ProgVertex (116 bytes), and the pixel
 *     shaders nv2a_psh.c generates, cached by nv2a_psh_key().
 *   - Vertex programs on the GPU: d3d8_gl_vsh.c's GLSL over the title's own
 *     vertex arrays (an array with stride 0 is a binding with stride 0).
 *     Point sprites stay on the CPU path.
 *
 * One push-descriptor set for every shader: uniform blocks at bindings 0-2
 * (PshConsts or the fixed-function block, NvConsts, NvParams), textures
 * tex0..tex3 at 3-6; 16 bytes of push constants. All per-draw data goes
 * through the frame's rings (d3d8_vk_core.c).
 *
 * Images are top-down: a D3D clockwise front face is VK_FRONT_FACE_CLOCKWISE
 * and the shaders produce Vulkan's clip space (y down, z in [0, w]).
 *
 * A draw takes everything that can flush first (pipeline, texture uploads,
 * ring space), then records: an allocation never outlives a flush.
 */
#include "d3d8_vk_internal.h"
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
volatile long g_nv_shc_hits = 0;        /* no shader disk cache here (the driver's pipeline cache instead) */

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
 * Dynamic state cache (per command buffer)
 * ================================================================ */
typedef struct {
    int dtest, dwrite, stest;
    VkCompareOp dcmp, scmp;
    VkStencilOp sfail, szfail, spass;
    uint32_t sref, srmask, swmask;
    VkCullModeFlags cull;
    VkFrontFace front;
    VkPrimitiveTopology topo;
    VkViewport vp;
    VkRect2D sc;
} DynState;

static struct {
    unsigned gen;                       /* g_vk.cb_gen it is valid for; 0 none */
    DynState d;
    VkPipeline pipe;
    unsigned long long vi;              /* vertex input layout */
    float pc[4];
} C;

void vkd_state_invalidate(void) { C.gen = 0; }

static void dyn_apply(VkCommandBuffer cb, const DynState *d)
{
    int all = C.gen != g_vk.cb_gen;
    if (all) {
        C.gen = g_vk.cb_gen;
        C.pipe = VK_NULL_HANDLE;
        C.vi = 0;
        memset(C.pc, 0xFF, sizeof C.pc);
    }
    if (all || d->dtest != C.d.dtest) vkCmdSetDepthTestEnable(cb, (VkBool32)d->dtest);
    if (all || d->dwrite != C.d.dwrite) vkCmdSetDepthWriteEnable(cb, (VkBool32)d->dwrite);
    if (all || d->dcmp != C.d.dcmp) vkCmdSetDepthCompareOp(cb, d->dcmp);
    if (all || d->stest != C.d.stest) vkCmdSetStencilTestEnable(cb, (VkBool32)d->stest);
    if (all || d->sfail != C.d.sfail || d->szfail != C.d.szfail || d->spass != C.d.spass || d->scmp != C.d.scmp)
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, d->sfail, d->spass, d->szfail, d->scmp);
    if (all || d->sref != C.d.sref) vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, d->sref);
    if (all || d->srmask != C.d.srmask) vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, d->srmask);
    if (all || d->swmask != C.d.swmask) vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, d->swmask);
    if (all || d->cull != C.d.cull) vkCmdSetCullMode(cb, d->cull);
    if (all || d->front != C.d.front) vkCmdSetFrontFace(cb, d->front);
    if (all || d->topo != C.d.topo) vkCmdSetPrimitiveTopology(cb, d->topo);
    if (all || memcmp(&d->vp, &C.d.vp, sizeof d->vp)) vkCmdSetViewport(cb, 0, 1, &d->vp);
    if (all || memcmp(&d->sc, &C.d.sc, sizeof d->sc)) vkCmdSetScissor(cb, 0, 1, &d->sc);
    C.d = *d;
}

static void bind_pipeline(VkCommandBuffer cb, VkPipeline p)
{
    if (C.pipe != p) { C.pipe = p; vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); }
}

static void push_consts(VkCommandBuffer cb, const float v[4])
{
    if (memcmp(C.pc, v, sizeof C.pc)) {
        memcpy(C.pc, v, sizeof C.pc);
        vkCmdPushConstants(cb, g_vk.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, v);
    }
}

/* ================================================================
 * Enum translation (d3d8_states.c)
 * ================================================================ */
static VkBlendFactor vk_blend(DWORD b)
{
    switch (b) {
    case D3DBLEND_ZERO:         return VK_BLEND_FACTOR_ZERO;
    case D3DBLEND_ONE:          return VK_BLEND_FACTOR_ONE;
    case D3DBLEND_SRCCOLOR:     return VK_BLEND_FACTOR_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case D3DBLEND_SRCALPHA:     return VK_BLEND_FACTOR_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case D3DBLEND_DESTALPHA:    return VK_BLEND_FACTOR_DST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case D3DBLEND_DESTCOLOR:    return VK_BLEND_FACTOR_DST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case D3DBLEND_SRCALPHASAT:  return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default:                    return VK_BLEND_FACTOR_ONE;
    }
}

/* The alpha-channel counterpart of a colour factor (as the D3D11 backend). */
static VkBlendFactor vk_blend_alpha(VkBlendFactor b)
{
    switch (b) {
    case VK_BLEND_FACTOR_SRC_COLOR:           return VK_BLEND_FACTOR_SRC_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case VK_BLEND_FACTOR_DST_COLOR:           return VK_BLEND_FACTOR_DST_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE:  return VK_BLEND_FACTOR_ONE;   /* not valid for alpha */
    default:                                  return b;
    }
}

static VkBlendOp vk_blendop(DWORD op)
{
    switch (op) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp vk_cmp(DWORD c)
{
    switch (c) {
    case D3DCMP_NEVER:        return VK_COMPARE_OP_NEVER;
    case D3DCMP_LESS:         return VK_COMPARE_OP_LESS;
    case D3DCMP_EQUAL:        return VK_COMPARE_OP_EQUAL;
    case D3DCMP_LESSEQUAL:    return VK_COMPARE_OP_LESS_OR_EQUAL;
    case D3DCMP_GREATER:      return VK_COMPARE_OP_GREATER;
    case D3DCMP_NOTEQUAL:     return VK_COMPARE_OP_NOT_EQUAL;
    case D3DCMP_GREATEREQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case D3DCMP_ALWAYS:       return VK_COMPARE_OP_ALWAYS;
    default:                  return VK_COMPARE_OP_LESS_OR_EQUAL;
    }
}

static VkStencilOp vk_stencilop(DWORD op)
{
    switch (op) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;   /* D3D INCRSAT */
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;   /* D3D DECRSAT */
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static VkSamplerAddressMode vk_address(DWORD m)
{
    switch (m) {
    case D3DTADDRESS_MIRROR:     return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case D3DTADDRESS_CLAMP:      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case D3DTADDRESS_BORDER:     return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case D3DTADDRESS_MIRRORONCE: return g_vk.has_mirror_once ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE
                                                             : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default:                     return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

/* ================================================================
 * Samplers (d3d8_states_apply_sampler)
 * ================================================================ */
typedef struct {
    int min, mag, mip, aniso_on;
    VkSamplerAddressMode u, v;
    float aniso, bias, min_lod, max_lod;
} SamplerDesc;

#define SAMPLER_CACHE 256
static struct { SamplerDesc d; VkSampler s; } s_smp[SAMPLER_CACHE];
static int s_nsmp;
static VkSampler s_blit_linear, s_blit_nearest;

static VkSampler sampler_make(const SamplerDesc *d)
{
    VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkSampler s = VK_NULL_HANDLE;
    float maxb = g_vk.props.limits.maxSamplerLodBias;
    ci.magFilter = d->mag ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = d->min ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = d->mip ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = d->u;
    ci.addressModeV = d->v;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.mipLodBias = d->bias > maxb ? maxb : d->bias < -maxb ? -maxb : d->bias;
    ci.anisotropyEnable = d->aniso_on && g_vk.has_aniso && d->aniso > 1.0f;
    ci.maxAnisotropy = d->aniso;
    ci.minLod = d->min_lod;
    ci.maxLod = d->max_lod;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;   /* GL's default border */
    vkCreateSampler(g_vk.dev, &ci, NULL, &s);
    return s;
}

static VkSampler sampler_for_stage(DWORD stage)
{
    const DWORD *tss = g_vkd.tss[stage];
    SamplerDesc d;
    union { DWORD u; float f; } bias;
    DWORD mag = tss[D3DTSS_MAGFILTER], min = tss[D3DTSS_MINFILTER], mip = tss[D3DTSS_MIPFILTER];
    int min_lin = (min == D3DTEXF_LINEAR || min == D3DTEXF_ANISOTROPIC);
    int mag_lin = (mag == D3DTEXF_LINEAR || mag == D3DTEXF_ANISOTROPIC);
    int mip_lin = (mip == D3DTEXF_LINEAR);
    int aniso_filter = (mag == D3DTEXF_ANISOTROPIC || min == D3DTEXF_ANISOTROPIC);
    UINT aniso = tss[D3DTSS_MAXANISOTROPY] ? tss[D3DTSS_MAXANISOTROPY] : 1;
    int k;
    VkSampler s;

    memset(&d, 0, sizeof d);
    if (aniso > 16) aniso = 16;
    if ((g_host_aniso > 1 || aniso > 1) && min != D3DTEXF_POINT && min != D3DTEXF_NONE) {
        aniso_filter = 1;
        if ((UINT)g_host_aniso > aniso) aniso = (UINT)g_host_aniso;
    }
    if (aniso_filter) {                 /* D3D11_FILTER_ANISOTROPIC: all linear */
        d.min = d.mag = d.mip = 1;
        d.aniso_on = 1;
        d.aniso = (float)aniso;
        if (d.aniso > g_vk.max_aniso) d.aniso = g_vk.max_aniso;
    } else {
        /* D3D11 has no "no mip" filter: D3DTEXF_NONE is point mip, held to
         * one level by the LOD range below. */
        d.min = min_lin;
        d.mag = mag_lin;
        d.mip = mip_lin;
        d.aniso = 1.0f;
    }
    d.u = vk_address(tss[D3DTSS_ADDRESSU] ? tss[D3DTSS_ADDRESSU] : D3DTADDRESS_WRAP);
    d.v = vk_address(tss[D3DTSS_ADDRESSV] ? tss[D3DTSS_ADDRESSV] : D3DTADDRESS_WRAP);
    bias.u = tss[D3DTSS_MIPMAPLODBIAS];
    d.bias = (bias.f == bias.f && bias.f > -16.0f && bias.f < 16.0f) ? bias.f : 0.0f;
    d.min_lod = (float)tss[D3DTSS_MAXMIPLEVEL];
    d.max_lod = (mip == D3DTEXF_NONE) ? d.min_lod : 1000.0f;

    for (k = 0; k < s_nsmp; k++)
        if (!memcmp(&s_smp[k].d, &d, sizeof d)) return s_smp[k].s;
    s = sampler_make(&d);
    if (s_nsmp < SAMPLER_CACHE) { s_smp[s_nsmp].d = d; s_smp[s_nsmp].s = s; s_nsmp++; }
    else {                              /* full: recycle the oldest slot (after the GPU is idle) */
        static int next;
        vkd_flush(1);
        vkDestroySampler(g_vk.dev, s_smp[next].s, NULL);
        s_smp[next].d = d; s_smp[next].s = s;
        next = (next + 1) % SAMPLER_CACHE;
    }
    return s;
}

/* ================================================================
 * Textures for the four stages, descriptors
 * ================================================================ */
static VkdImage s_dummy_img;            /* (0,0,0,0): an unbound SRV reads zero */
static VkBuffer s_dummy_ub, s_const_vb;
static VkdMem   s_dummy_ub_mem, s_const_vb_mem;

typedef struct {
    VkImageView view[VKD_MAX_STAGES];
    VkSampler   smp[VKD_MAX_STAGES];
} StageBind;

/* Uploads happen here (they may end rendering or flush): before any ring
 * allocation of the draw. */
static void stage_textures(StageBind *b)
{
    int i;
    for (i = 0; i < VKD_MAX_STAGES; i++) {
        VkImageView v = g_vkd.tex[i] ? vkd_texture_ensure(g_vkd.tex[i]) : VK_NULL_HANDLE;
        b->view[i] = v ? v : s_dummy_img.view;
    }
    for (i = 0; i < VKD_MAX_STAGES; i++) b->smp[i] = sampler_for_stage((DWORD)i);
}

typedef struct { VkBuffer buf; VkDeviceSize off, range; } UbRef;

static void push_set(VkCommandBuffer cb, const UbRef ub[3], const StageBind *t)
{
    VkDescriptorBufferInfo bi[3];
    VkDescriptorImageInfo ii[VKD_MAX_STAGES];
    VkWriteDescriptorSet w[VKD_NBIND];
    int i;
    memset(w, 0, sizeof w);
    for (i = 0; i < 3; i++) {
        bi[i].buffer = ub && ub[i].buf ? ub[i].buf : s_dummy_ub;
        bi[i].offset = ub && ub[i].buf ? ub[i].off : 0;
        bi[i].range = ub && ub[i].buf ? ub[i].range : 256;
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[i].pBufferInfo = &bi[i];
    }
    for (i = 0; i < VKD_MAX_STAGES; i++) {
        ii[i].sampler = t->smp[i];
        ii[i].imageView = t->view[i];
        ii[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        w[3 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[3 + i].dstBinding = (uint32_t)(VKD_B_TEX0 + i);
        w[3 + i].descriptorCount = 1;
        w[3 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[3 + i].pImageInfo = &ii[i];
    }
    g_vk.push_desc(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_vk.layout, 0, VKD_NBIND, w);
}

/* Uniform data into the ring; *ref for the descriptor. */
static int ub_put(const void *data, size_t n, size_t range, UbRef *ref)
{
    void *p;
    if (!vkd_ring_alloc(VKD_RING_UB, range, (size_t)vkd_ub_align(), &ref->buf, &ref->off, &p)) return 0;
    if (data && n) memcpy(p, data, n < range ? n : range);
    if (n < range) memset((uint8_t *)p + n, 0, range - n);
    ref->range = range;
    return 1;
}

static size_t ub_pad(size_t n)
{
    size_t a = (size_t)vkd_ub_align();
    return (n + a - 1) / a * a;
}

/* ================================================================
 * Render states (d3d8_states_apply): pipeline key + dynamic state
 * ================================================================ */
/* raster < 0: from the render states; else the NV2A bits (0 depth clip,
 * 2:1 cull (1 front, 2 back), 3 front CCW). */
static void states_build(VkdPipeKey *k, DynState *d, int raster)
{
    const DWORD *rs = g_vkd.rs;
    VkBlendFactor bs, bd;
    float sx, sy;
    double x, y, w, h;
    memset(d, 0, sizeof *d);
    k->blend = rs[D3DRS_ALPHABLENDENABLE] ? 1 : 0;
    bs = vk_blend(rs[D3DRS_SRCBLEND]);
    bd = vk_blend(rs[D3DRS_DESTBLEND]);
    k->bs = (uint8_t)bs;
    k->bd = (uint8_t)bd;
    k->bsa = (uint8_t)vk_blend_alpha(bs);
    k->bda = (uint8_t)vk_blend_alpha(bd);
    k->bop = (uint8_t)vk_blendop(rs[D3DRS_BLENDOP] ? rs[D3DRS_BLENDOP] : 1);
    if (!k->blend) k->bs = k->bd = k->bsa = k->bda = k->bop = 0;
    k->cmask = (uint8_t)(rs[D3DRS_COLORWRITEENABLE] & 0x0F);
    k->samples = (uint8_t)(g_vkd.msaa > 1 ? g_vkd.msaa : 1);
    k->target = VKD_TARGET_SCENE;
    d->dtest = rs[D3DRS_ZENABLE] ? 1 : 0;
    d->dwrite = rs[D3DRS_ZWRITEENABLE] ? 1 : 0;
    d->dcmp = vk_cmp(rs[D3DRS_ZFUNC]);
    d->stest = rs[D3DRS_STENCILENABLE] ? 1 : 0;
    d->scmp = vk_cmp(rs[D3DRS_STENCILFUNC]);
    d->sref = rs[D3DRS_STENCILREF] & 0xFF;
    d->srmask = rs[D3DRS_STENCILMASK] & 0xFF;
    d->swmask = rs[D3DRS_STENCILWRITEMASK] & 0xFF;
    d->sfail = vk_stencilop(rs[D3DRS_STENCILFAIL]);
    d->szfail = vk_stencilop(rs[D3DRS_STENCILZFAIL]);
    d->spass = vk_stencilop(rs[D3DRS_STENCILPASS]);
    if (raster < 0) {                   /* D3DCULL_CW culls clockwise (front, with front = CW) */
        DWORD cmode = rs[D3DRS_CULLMODE];
        d->cull = cmode == D3DCULL_NONE ? VK_CULL_MODE_NONE
                : cmode == D3DCULL_CW ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
        d->front = VK_FRONT_FACE_CLOCKWISE;
        k->poly_line = (rs[D3DRS_FILLMODE] == D3DFILL_POINT || rs[D3DRS_FILLMODE] == D3DFILL_WIREFRAME);
        k->depth_clamp = 0;
    } else {
        int cull = (raster >> 1) & 3;
        d->cull = cull == 1 ? VK_CULL_MODE_FRONT_BIT : cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        d->front = (raster & 8) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        k->poly_line = 0;
        k->depth_clamp = (raster & 1) ? 0 : 1;
    }
    /* The title's viewport, scaled to the scene target. */
    d3d8_GetGuestScale(&sx, &sy);
    x = (double)g_vkd.viewport.X * sx;
    y = (double)g_vkd.viewport.Y * sy;
    w = (double)g_vkd.viewport.Width * sx;
    h = (double)g_vkd.viewport.Height * sy;
    d->vp.x = (float)floor(x + 0.5);
    d->vp.y = (float)floor(y + 0.5);
    d->vp.width = (float)floor(w + 0.5);
    d->vp.height = (float)floor(h + 0.5);
    if (d->vp.width < 1.0f) d->vp.width = 1.0f;
    if (d->vp.height < 1.0f) d->vp.height = 1.0f;
    d->vp.minDepth = g_vkd.viewport.MinZ < 0.0f ? 0.0f : g_vkd.viewport.MinZ > 1.0f ? 1.0f : g_vkd.viewport.MinZ;
    d->vp.maxDepth = g_vkd.viewport.MaxZ < 0.0f ? 0.0f : g_vkd.viewport.MaxZ > 1.0f ? 1.0f : g_vkd.viewport.MaxZ;
    /* Window clip -> scissor, in title pixels scaled to the scene target. */
    if (g_wclip_on) {
        LONG l = (LONG)(g_wclip.left * sx + 0.5f), t = (LONG)(g_wclip.top * sy + 0.5f);
        LONG r = (LONG)(g_wclip.right * sx + 0.5f), b = (LONG)(g_wclip.bottom * sy + 0.5f);
        if (l < 0) l = 0;
        if (t < 0) t = 0;
        if (r > (LONG)g_vkd.width) r = (LONG)g_vkd.width;
        if (b > (LONG)g_vkd.height) b = (LONG)g_vkd.height;
        d->sc.offset.x = l;
        d->sc.offset.y = t;
        d->sc.extent.width = r > l ? (uint32_t)(r - l) : 0;
        d->sc.extent.height = b > t ? (uint32_t)(b - t) : 0;
    } else {
        d->sc.extent.width = g_vkd.width;
        d->sc.extent.height = g_vkd.height;
    }
}

/* ================================================================
 * Vertex input (VK_EXT_vertex_input_dynamic_state)
 * ================================================================ */
typedef struct {
    uint32_t nb, na;
    VkVertexInputBindingDescription2EXT b[16];
    VkVertexInputAttributeDescription2EXT a[16];
} VtxLayout;

static void vl_binding(VtxLayout *l, uint32_t binding, uint32_t stride)
{
    VkVertexInputBindingDescription2EXT *b = &l->b[l->nb++];
    memset(b, 0, sizeof *b);
    b->sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT;
    b->binding = binding;
    b->stride = stride;
    b->inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    b->divisor = 1;
}

static void vl_attr(VtxLayout *l, uint32_t loc, uint32_t binding, VkFormat f, uint32_t off)
{
    VkVertexInputAttributeDescription2EXT *a = &l->a[l->na++];
    memset(a, 0, sizeof *a);
    a->sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT;
    a->location = loc;
    a->binding = binding;
    a->format = f;
    a->offset = off;
}

static void vl_apply(VkCommandBuffer cb, const VtxLayout *l)
{
    unsigned long long h = 1469598103934665603ull;
    uint32_t i;
    for (i = 0; i < l->nb; i++) {
        h = (h ^ l->b[i].binding) * 1099511628211ull;
        h = (h ^ l->b[i].stride) * 1099511628211ull;
        h = (h ^ (unsigned long long)l->b[i].inputRate) * 1099511628211ull;
    }
    for (i = 0; i < l->na; i++) {
        h = (h ^ (l->a[i].location | (l->a[i].binding << 8))) * 1099511628211ull;
        h = (h ^ (unsigned long long)l->a[i].format) * 1099511628211ull;
        h = (h ^ l->a[i].offset) * 1099511628211ull;
    }
    h = (h ^ (l->nb | (l->na << 8))) * 1099511628211ull;
    if (!h) h = 1;
    if (C.vi == h) return;
    C.vi = h;
    g_vk.set_vertex_input(cb, l->nb, l->b, l->na, l->a);
}

/* ================================================================
 * Shaders
 * ================================================================ */
/* ── Fixed function (d3d8_shaders.c) ──────────────────────────────── */
#define FFP_UBO_GLSL \
    "layout(std140, set = 0, binding = 0) uniform FfpConsts {\n" \
    "    mat4 WorldViewProj; mat4 World; mat4 WorldInvTranspose;\n" \
    "    vec4 ScreenSize; vec4 EyePos; vec4 FogParams;\n" \
    "    vec4 GlobalAmbient; vec4 MatDiffuse; vec4 MatAmbient; vec4 MatSpecular; vec4 MatEmissive;\n" \
    "    vec4 TexFactor; vec4 FogColor;\n" \
    "    float MatPower; float AlphaRef; uint Flags; uint NumLights;\n" \
    "    uint AlphaFunc; uint PSFlags; uint Pad0; uint Pad1;\n" \
    "    uvec4 StageColor[4]; uvec4 StageAlpha[4];\n" \
    "    vec4 LPosType[8]; vec4 LDirRange[8]; vec4 LDiffuse[8]; vec4 LAmbient[8];\n" \
    "    vec4 LSpecular[8]; vec4 LAtten[8]; vec4 LSpot[8];\n" \
    "};\n"

typedef struct {                        /* FfpConsts, std140 */
    float wvp[16], world[16], wit[16];
    float screen[4], eye[4], fog[4];
    float amb[4], mdif[4], mamb[4], mspec[4], memis[4];
    float tf[4], fogc[4];
    float mpow, aref;
    uint32_t flags, nl;
    uint32_t afunc, psflags, pad0, pad1;
    uint32_t scolor[4][4], salpha[4][4];
    float lpos[8][4], ldir[8][4], ldif[8][4], lamb[8][4], lspec[8][4], latt[8][4], lspot[8][4];
} FfpUbo;

static const char s_ffp_vs[] =
    "#version 450\n"
    FFP_UBO_GLSL
    "layout(location = 0) in vec4 a_pos;\n"
    "layout(location = 1) in vec3 a_normal;\n"
    "layout(location = 2) in vec4 a_diffuse;\n"
    "layout(location = 3) in vec4 a_specular;\n"
    "layout(location = 4) in vec2 a_tex0;\n"
    "layout(location = 5) in vec2 a_tex1;\n"
    "layout(location = 6) in vec2 a_tex2;\n"
    "layout(location = 7) in vec2 a_tex3;\n"
    "layout(location = 0) out vec4 v_diffuse;\n"
    "layout(location = 1) out vec4 v_specular;\n"
    "layout(location = 2) out vec2 v_tex0;\n"
    "layout(location = 3) out vec2 v_tex1;\n"
    "layout(location = 4) out vec2 v_tex2;\n"
    "layout(location = 5) out vec2 v_tex3;\n"
    "layout(location = 6) out float v_fog;\n"
    "float compute_fog(float dist) {\n"
    "    uint mode = uint(FogParams.w);\n"
    "    if (mode == 1u) return clamp((FogParams.y - dist) / (FogParams.y - FogParams.x), 0.0, 1.0);\n"
    "    if (mode == 2u) return clamp(exp(-FogParams.z * dist), 0.0, 1.0);\n"
    "    if (mode == 3u) { float e = FogParams.z * dist; return clamp(exp(-e * e), 0.0, 1.0); }\n"
    "    return 1.0;\n"
    "}\n"
    "void main() {\n"
    "    gl_PointSize = 1.0;\n"
    "    v_tex0 = a_tex0; v_tex1 = a_tex1; v_tex2 = a_tex2; v_tex3 = a_tex3;\n"
    "    v_fog = 1.0;\n"
    "    v_specular = vec4(0.0);\n"
    "    if ((Flags & 1u) != 0u) {\n"
    "        gl_Position = vec4(a_pos.x / ScreenSize.x * 2.0 - 1.0, a_pos.y / ScreenSize.y * 2.0 - 1.0,\n"
    "                           a_pos.z, 1.0);\n"
    "        v_diffuse = (Flags & 2u) != 0u ? a_diffuse.bgra : vec4(1.0);\n"
    "        if ((Flags & 4u) != 0u) { v_specular = a_specular.bgra; v_fog = a_specular.a; }\n"
    "        return;\n"
    "    }\n"
    "    vec4 p = WorldViewProj * vec4(a_pos.xyz, 1.0);\n"
    "    gl_Position = vec4(p.x, -p.y, p.z, p.w);\n"
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
    "#version 450\n"
    FFP_UBO_GLSL
    "layout(set = 0, binding = 3) uniform sampler2D tex0;\n"
    "layout(set = 0, binding = 4) uniform sampler2D tex1;\n"
    "layout(set = 0, binding = 5) uniform sampler2D tex2;\n"
    "layout(set = 0, binding = 6) uniform sampler2D tex3;\n"
    "layout(location = 0) in vec4 v_diffuse;\n"
    "layout(location = 1) in vec4 v_specular;\n"
    "layout(location = 2) in vec2 v_tex0;\n"
    "layout(location = 3) in vec2 v_tex1;\n"
    "layout(location = 4) in vec2 v_tex2;\n"
    "layout(location = 5) in vec2 v_tex3;\n"
    "layout(location = 6) in float v_fog;\n"
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

static VkShaderModule s_ffp_vs_m, s_ffp_fs_m;

/* ── NV2A pass-through (d3d8_nv2a.c) ──────────────────────────────── */
static const char s_nv_vs[] =
    "#version 450\n"
    "layout(location = 0) in vec4 a_pos;\n"
    "layout(location = 1) in vec4 a_d0;\n"
    "layout(location = 2) in vec4 a_d1;\n"
    "layout(location = 3) in float a_fog;\n"
    "layout(location = 4) in vec4 a_t0;\n"
    "layout(location = 5) in vec4 a_t1;\n"
    "layout(location = 6) in vec4 a_t2;\n"
    "layout(location = 7) in vec4 a_t3;\n"
    "layout(push_constant) uniform Pc { vec4 screen; } pc;\n"
    "layout(location = 0) out NvVary {\n"
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
    "    gl_Position = vec4((a_pos.x / pc.screen.x * 2.0 - 1.0) * w,\n"
    "                       (a_pos.y / pc.screen.y * 2.0 - 1.0) * w,\n"
    "                       a_pos.z * w, w);\n"
    "    gl_PointSize = 1.0;\n"
    "    o.d0 = a_d0; o.d1 = a_d1; o.fog = a_fog;\n"
    "    o.t0 = a_t0; o.t1 = a_t1; o.t2 = a_t2; o.t3 = a_t3;\n"
    "}\n";

static VkShaderModule s_nv_vs_m;

#define NV_PS_CACHE 2048
#define NV_PS_SLOTS (NV_PS_CACHE * 2)
static struct { unsigned long long key; VkShaderModule fs; int used; } s_ps[NV_PS_SLOTS];
static int s_ps_count;
static void vp_shutdown(void);

/* ── Blit ─────────────────────────────────────────────────────────── */
static const char s_blit_vs[] =
    "#version 450\n"
    "layout(location = 0) out vec2 uv;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
    "    uv = p;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "    gl_PointSize = 1.0;\n"
    "}\n";
static const char s_blit_fs[] =
    "#version 450\n"
    "layout(set = 0, binding = 3) uniform sampler2D t;\n"
    "layout(set = 0, binding = 4) uniform sampler2D lut;\n"
    "layout(push_constant) uniform Pc { vec4 use_lut; } pc;\n"
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "    vec3 c = texture(t, uv).rgb;\n"
    "    if (pc.use_lut.x != 0.0) {\n"
    "        ivec3 k = ivec3(round(clamp(c, 0.0, 1.0) * 255.0));\n"
    "        c = vec3(texelFetch(lut, ivec2(k.r, 0), 0).r, texelFetch(lut, ivec2(k.g, 0), 0).g,\n"
    "                 texelFetch(lut, ivec2(k.b, 0), 0).b);\n"
    "    }\n"
    "    o = vec4(c, 1.0);\n"
    "}\n";
static VkShaderModule s_blit_vs_m, s_blit_fs_m;

/* Vertex formats the vertex-program path may use (nv2a_dxgi_shim.h numbers). */
static VkFormat vp_vkformat(uint32_t f)
{
    switch (f) {
    case 41: return VK_FORMAT_R32_SFLOAT;
    case 16: return VK_FORMAT_R32G32_SFLOAT;
    case 6:  return VK_FORMAT_R32G32B32_SFLOAT;
    case 2:  return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 61: return VK_FORMAT_R8_UNORM;
    case 49: return VK_FORMAT_R8G8_UNORM;
    case 28: return VK_FORMAT_R8G8B8A8_UNORM;
    case 87: return VK_FORMAT_B8G8R8A8_UNORM;
    case 58: return VK_FORMAT_R16_SNORM;
    case 37: return VK_FORMAT_R16G16_SNORM;
    case 13: return VK_FORMAT_R16G16B16A16_SNORM;
    case 59: return VK_FORMAT_R16_SINT;
    case 38: return VK_FORMAT_R16G16_SINT;
    case 14: return VK_FORMAT_R16G16B16A16_SINT;
    case 42: return VK_FORMAT_R32_UINT;
    default: return VK_FORMAT_UNDEFINED;
    }
}
static unsigned char s_vfmt_ok[128];    /* by nv2a_dxgi_shim number: 1 usable */

/* ================================================================
 * Init / shutdown
 * ================================================================ */
int vkd_draw_init(void)
{
    static const float defaults[16] = {
        0.0f, 0.0f, 0.0f, 1.0f,         /* position, texcoords */
        0.0f, 0.0f, 1.0f, 0.0f,         /* normal */
        1.0f, 1.0f, 1.0f, 1.0f,         /* diffuse */
        0.0f, 0.0f, 0.0f, 0.0f,         /* specular */
    };
    static const unsigned char zero[4] = { 0, 0, 0, 0 };
    SamplerDesc d;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    uint32_t f;

    s_ffp_vs_m = vkd_shader_glsl(0, s_ffp_vs, -1, "ffp vertex");
    s_ffp_fs_m = vkd_shader_glsl(1, s_ffp_fs, -1, "ffp pixel");
    s_nv_vs_m = vkd_shader_glsl(0, s_nv_vs, -1, "nv2a vertex");
    s_blit_vs_m = vkd_shader_glsl(0, s_blit_vs, -1, "blit vertex");
    s_blit_fs_m = vkd_shader_glsl(1, s_blit_fs, -1, "blit pixel");
    if (!s_ffp_vs_m || !s_ffp_fs_m || !s_nv_vs_m || !s_blit_vs_m || !s_blit_fs_m) return 0;

    memset(&d, 0, sizeof d);
    d.min = d.mag = 1;
    d.u = d.v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    d.aniso = 1.0f;
    s_blit_linear = sampler_make(&d);
    d.min = d.mag = 0;
    s_blit_nearest = sampler_make(&d);

    if (!vkd_image_create(&s_dummy_img, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_SAMPLE_COUNT_1_BIT, NULL))
        return 0;
    vkd_image_upload(0, &s_dummy_img, 0, 0, 0, 1, 1, zero, 4, 0);

    {   /* the dummy uniform block and the constant attributes, device-side */
        VkBuffer b;
        VkDeviceSize off;
        void *p;
        VkBufferCopy c;
        VkMemoryRequirements rq;
        bi.size = 4096;
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(g_vk.dev, &bi, NULL, &s_dummy_ub) != VK_SUCCESS) return 0;
        vkGetBufferMemoryRequirements(g_vk.dev, s_dummy_ub, &rq);
        {
            VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            VkPhysicalDeviceMemoryProperties mp;
            uint32_t i;
            vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
            for (i = 0; i < mp.memoryTypeCount; i++)
                if (rq.memoryTypeBits & (1u << i)) break;
            ai.allocationSize = rq.size;
            ai.memoryTypeIndex = i;
            if (vkAllocateMemory(g_vk.dev, &ai, NULL, &s_dummy_ub_mem.mem) != VK_SUCCESS) return 0;
            s_dummy_ub_mem.block = -1;
            vkBindBufferMemory(g_vk.dev, s_dummy_ub, s_dummy_ub_mem.mem, 0);
        }
        s_const_vb = s_dummy_ub;        /* one buffer: the defaults at offset 0 */
        s_const_vb_mem = s_dummy_ub_mem;
        if (!vkd_ring_alloc(VKD_RING_STAGE, 4096, 16, &b, &off, &p)) return 0;
        memset(p, 0, 4096);
        memcpy(p, defaults, sizeof defaults);
        c.srcOffset = off;
        c.dstOffset = 0;
        c.size = 4096;
        vkCmdCopyBuffer(vkd_cmd_pre(), b, s_dummy_ub, 1, &c);
    }

    memset(s_vfmt_ok, 0, sizeof s_vfmt_ok);
    for (f = 0; f < sizeof s_vfmt_ok; f++) {
        VkFormat v = vp_vkformat(f);
        if (v != VK_FORMAT_UNDEFINED && vkd_format_ok(v, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT, 1))
            s_vfmt_ok[f] = 1;
    }
    if (!s_vfmt_ok[87]) fprintf(stderr, "[VK] no B8G8R8A8 vertex format: those programs stay on the CPU\n");
    vkd_state_invalidate();
    return 1;
}

void vkd_draw_shutdown(void)
{
    int i;
    vkDeviceWaitIdle(g_vk.dev);
    vp_shutdown();
    for (i = 0; i < NV_PS_SLOTS; i++) vkd_shader_destroy(s_ps[i].fs);
    memset(s_ps, 0, sizeof s_ps);
    s_ps_count = 0;
    for (i = 0; i < s_nsmp; i++) vkDestroySampler(g_vk.dev, s_smp[i].s, NULL);
    s_nsmp = 0;
    vkd_pipelines_drop();
    vkd_shader_destroy(s_ffp_vs_m);
    vkd_shader_destroy(s_ffp_fs_m);
    vkd_shader_destroy(s_nv_vs_m);
    vkd_shader_destroy(s_blit_vs_m);
    vkd_shader_destroy(s_blit_fs_m);
    s_ffp_vs_m = s_ffp_fs_m = s_nv_vs_m = s_blit_vs_m = s_blit_fs_m = VK_NULL_HANDLE;
    vkd_state_invalidate();
}

/* ================================================================
 * Blit (present scaling, video upload)
 * ================================================================ */
void vkd_blit(VkdImage *src, int x, int y, int w, int h, VkdImage *lut, int target)
{
    VkdPipeKey k;
    DynState d;
    StageBind t;
    VtxLayout vl;
    VkPipeline p;
    VkCommandBuffer cb;
    float pc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    int i;
    if (!s_blit_vs_m || !src || !src->image || w <= 0 || h <= 0) return;
    memset(&k, 0, sizeof k);
    k.vs = s_blit_vs_m;
    k.fs = s_blit_fs_m;
    k.topo = VKD_TOPO_TRI;
    k.cmask = 0x0F;
    k.target = (uint8_t)target;
    k.samples = (uint8_t)(target == VKD_TARGET_SCENE && g_vkd.msaa > 1 ? g_vkd.msaa : 1);
    if (!(p = vkd_pipeline(&k))) return;
    memset(&d, 0, sizeof d);
    d.dcmp = d.scmp = VK_COMPARE_OP_ALWAYS;
    d.cull = VK_CULL_MODE_NONE;
    d.front = VK_FRONT_FACE_CLOCKWISE;
    d.topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    d.vp.x = (float)x;
    d.vp.y = (float)y;
    d.vp.width = (float)w;
    d.vp.height = (float)h;
    d.vp.maxDepth = 1.0f;
    d.sc.offset.x = x < 0 ? 0 : x;
    d.sc.offset.y = y < 0 ? 0 : y;
    d.sc.extent.width = (uint32_t)w;
    d.sc.extent.height = (uint32_t)h;
    for (i = 0; i < VKD_MAX_STAGES; i++) { t.view[i] = s_dummy_img.view; t.smp[i] = s_blit_nearest; }
    t.view[0] = src->view;
    t.smp[0] = s_blit_linear;
    if (lut && lut->image) { t.view[1] = lut->view; pc[0] = 1.0f; }
    cb = target == VKD_TARGET_SCENE ? vkd_cmd_render() : vkd_cmd();
    dyn_apply(cb, &d);
    bind_pipeline(cb, p);
    memset(&vl, 0, sizeof vl);
    vl_apply(cb, &vl);
    push_set(cb, NULL, &t);
    push_consts(cb, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkd_state_invalidate();
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

/* The constants d3d8_shaders_prepare_draw puts in its buffers. D3D matrices
 * are row-major for row vectors; read column-major by std140, a GLSL
 * `M * v` computes D3D's `v * M`. */
static void ffp_uniforms(DWORD fvf, FfpUbo *u)
{
    const DWORD *rs = g_vkd.rs;
    uint32_t flags = 0;
    UINT st;

    memset(u, 0, sizeof *u);
    u->screen[0] = (float)d3d8_GetBackbufferWidth();
    u->screen[1] = (float)d3d8_GetBackbufferHeight();
    if (fvf & D3DFVF_XYZRHW) {
        flags = 1;
    } else {
        float wv[16], vinv[16];
        const float *world = (const float *)&g_vkd.xforms[D3DTS_WORLD];
        mat4_mul(wv, world, (const float *)&g_vkd.xforms[D3DTS_VIEW]);
        mat4_mul(u->wvp, wv, (const float *)&g_vkd.xforms[D3DTS_PROJECTION]);
        memcpy(u->world, world, sizeof u->world);
        mat4_inverse(u->wit, world);
        mat4_transpose(u->wit, u->wit);
        mat4_inverse(vinv, (const float *)&g_vkd.xforms[D3DTS_VIEW]);
        u->eye[0] = vinv[12]; u->eye[1] = vinv[13]; u->eye[2] = vinv[14]; u->eye[3] = 1.0f;
    }
    if (fvf & D3DFVF_DIFFUSE) flags |= 0x02;
    if (fvf & D3DFVF_SPECULAR) flags |= 0x04;
    if (fvf & D3DFVF_NORMAL) flags |= 0x08;
    if (rs[D3DRS_LIGHTING]) flags |= 0x10;
    u->flags = flags;
    if (rs[D3DRS_FOGENABLE]) {
        memcpy(&u->fog[0], &rs[D3DRS_FOGSTART], 4);
        memcpy(&u->fog[1], &rs[D3DRS_FOGEND], 4);
        memcpy(&u->fog[2], &rs[D3DRS_FOGDENSITY], 4);
        u->fog[3] = (float)rs[D3DRS_FOGTABLEMODE];
    }
    if ((flags & 0x18) == 0x18) {
        const D3DMATERIAL8 *m = &g_vkd.material;
        UINT i, n = 0;
        argb_to_vec4(rs[D3DRS_AMBIENT], u->amb);
        memcpy(u->mdif, &m->Diffuse, 16);
        memcpy(u->mamb, &m->Ambient, 16);
        memcpy(u->mspec, &m->Specular, 16);
        memcpy(u->memis, &m->Emissive, 16);
        u->mpow = m->Power;
        for (i = 0; i < VKD_MAX_LIGHTS && n < 8; i++) {
            const D3DLIGHT8 *l = &g_vkd.lights[i];
            if (!g_vkd.light_enable[i]) continue;
            u->lpos[n][0] = l->Position.x; u->lpos[n][1] = l->Position.y; u->lpos[n][2] = l->Position.z;
            u->lpos[n][3] = (float)l->Type;
            u->ldir[n][0] = l->Direction.x; u->ldir[n][1] = l->Direction.y; u->ldir[n][2] = l->Direction.z;
            u->ldir[n][3] = l->Range;
            memcpy(u->ldif[n], &l->Diffuse, 16);
            memcpy(u->lamb[n], &l->Ambient, 16);
            memcpy(u->lspec[n], &l->Specular, 16);
            u->latt[n][0] = l->Attenuation0; u->latt[n][1] = l->Attenuation1; u->latt[n][2] = l->Attenuation2;
            u->latt[n][3] = l->Falloff;
            u->lspot[n][0] = cosf(l->Theta * 0.5f); u->lspot[n][1] = cosf(l->Phi * 0.5f);
            n++;
        }
        u->nl = n;
    }
    argb_to_vec4(rs[D3DRS_TEXTUREFACTOR], u->tf);
    if (rs[D3DRS_FOGENABLE]) {
        argb_to_vec4(rs[D3DRS_FOGCOLOR], u->fogc);
        u->fogc[3] = 1.0f;
        u->psflags |= 2;
    }
    if (rs[D3DRS_ALPHATESTENABLE]) {
        u->aref = (float)rs[D3DRS_ALPHAREF] / 255.0f;
        u->afunc = rs[D3DRS_ALPHAFUNC];
        u->psflags |= 1;
    }
    if (rs[D3DRS_SPECULARENABLE]) u->psflags |= 4;
    for (st = 0; st < 4; st++) {
        const DWORD *tss = g_vkd.tss[st];
        u->scolor[st][0] = tss[D3DTSS_COLOROP];
        u->scolor[st][1] = tss[D3DTSS_COLORARG1];
        u->scolor[st][2] = tss[D3DTSS_COLORARG2];
        u->scolor[st][3] = tss[D3DTSS_ALPHAOP];
        u->salpha[st][0] = tss[D3DTSS_ALPHAARG1];
        u->salpha[st][1] = tss[D3DTSS_ALPHAARG2];
    }
}

/* Attribute layout for an FVF (d3d8_shaders.c's input layouts); what the
 * FVF lacks reads the constant defaults through binding 1 (stride 0). */
static void ffp_layout(DWORD fvf, uint32_t stride, VtxLayout *l)
{
    uint32_t off = 0;
    UINT ntex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT, i;
    memset(l, 0, sizeof *l);
    vl_binding(l, 0, stride);
    vl_binding(l, 1, 0);
    if (fvf & D3DFVF_XYZRHW)     { vl_attr(l, 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, off); off += 16; }
    else if (fvf & D3DFVF_XYZ)   { vl_attr(l, 0, 0, VK_FORMAT_R32G32B32_SFLOAT, off); off += 12; }
    else                         vl_attr(l, 0, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0);
    if (fvf & D3DFVF_NORMAL)     { vl_attr(l, 1, 0, VK_FORMAT_R32G32B32_SFLOAT, off); off += 12; }
    else                         vl_attr(l, 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16);
    if (fvf & D3DFVF_DIFFUSE)    { vl_attr(l, 2, 0, VK_FORMAT_R8G8B8A8_UNORM, off); off += 4; }
    else                         vl_attr(l, 2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32);
    if (fvf & D3DFVF_SPECULAR)   { vl_attr(l, 3, 0, VK_FORMAT_R8G8B8A8_UNORM, off); off += 4; }
    else                         vl_attr(l, 3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48);
    for (i = 0; i < 4; i++) {
        if (i < ntex) { vl_attr(l, 4 + i, 0, VK_FORMAT_R32G32_SFLOAT, off); off += 8; }
        else          vl_attr(l, 4 + i, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0);
    }
}

static int vk_prim(D3DPRIMITIVETYPE pt, UINT count, UINT *nverts, VkPrimitiveTopology *topo)
{
    switch (pt) {
    case D3DPT_TRIANGLELIST:  *nverts = count * 3; *topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  return VKD_TOPO_TRI;
    case D3DPT_TRIANGLESTRIP: *nverts = count + 2; *topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return VKD_TOPO_TRI;
    case D3DPT_TRIANGLEFAN:   *nverts = count + 2; *topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;   return VKD_TOPO_TRI;
    case D3DPT_LINELIST:      *nverts = count * 2; *topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;      return VKD_TOPO_LINE;
    case D3DPT_LINESTRIP:     *nverts = count + 1; *topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;     return VKD_TOPO_LINE;
    case D3DPT_POINTLIST:     *nverts = count;     *topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;     return VKD_TOPO_POINT;
    case D3DPT_QUADLIST:      *nverts = count * 4; *topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  return VKD_TOPO_TRI;
    default:                  *nverts = 0;         *topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;     return VKD_TOPO_POINT;
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

HRESULT vkd_draw_up(D3DPRIMITIVETYPE prim, UINT prim_count, const void *verts, UINT stride,
                    const void *indices, int index32, UINT index_count)
{
    DWORD fvf = g_vkd.vertex_shader;
    UINT nv = 0, nidx = 0, maxv = 0, i;
    VkPrimitiveTopology topo;
    VkdPipeKey k;
    DynState d;
    StageBind tb;
    VtxLayout vl;
    VkPipeline pipe;
    VkCommandBuffer cb;
    VkBuffer vbuf, ibuf = VK_NULL_HANDLE;
    VkDeviceSize voff, ioff = 0, zero = 0;
    UbRef ub[3];
    FfpUbo u;
    void *p, *tmp = NULL;
    const void *idx = indices;
    size_t isz = 0;
    float pc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    if (!g_vkd.initialized || !vkd_on_render_thread() || !verts || !stride || !s_ffp_vs_m) return E_FAIL;
    if (!(fvf & (D3DFVF_XYZ | D3DFVF_XYZRHW))) fvf |= D3DFVF_XYZRHW;   /* not an FVF: as pre-transformed */
    if (fvf_stride(fvf) > stride) return E_INVALIDARG;
    memset(&k, 0, sizeof k);
    k.topo = (uint8_t)vk_prim(prim, prim_count, &nv, &topo);
    if (!nv) return E_INVALIDARG;

    if (idx) {
        nidx = index_count;
        for (i = 0; i < nidx; i++) {
            UINT v = index32 ? ((const uint32_t *)idx)[i] : ((const uint16_t *)idx)[i];
            if (v > maxv) maxv = v;
        }
        if (prim == D3DPT_QUADLIST) {   /* 4 indices per quad -> 6 */
            uint32_t *q = (uint32_t *)malloc((size_t)prim_count * 6 * 4);
            if (!q) return E_OUTOFMEMORY;
            for (i = 0; i < prim_count; i++) {
                UINT b = i * 4, j;
                UINT v4[4];
                for (j = 0; j < 4; j++)
                    v4[j] = index32 ? ((const uint32_t *)idx)[b + j] : ((const uint16_t *)idx)[b + j];
                q[i * 6 + 0] = v4[0]; q[i * 6 + 1] = v4[1]; q[i * 6 + 2] = v4[2];
                q[i * 6 + 3] = v4[0]; q[i * 6 + 4] = v4[2]; q[i * 6 + 5] = v4[3];
            }
            tmp = q; idx = q; index32 = 1; nidx = prim_count * 6;
        }
        nv = maxv + 1;
        isz = index32 ? 4 : 2;
    } else if (prim == D3DPT_QUADLIST) {
        const BYTE *s = (const BYTE *)verts;
        BYTE *dst = (BYTE *)malloc((size_t)prim_count * 6 * stride);
        static const int order[6] = { 0, 1, 2, 0, 2, 3 };
        if (!dst) return E_OUTOFMEMORY;
        for (i = 0; i < prim_count; i++) {
            int j;
            for (j = 0; j < 6; j++)
                memcpy(dst + ((size_t)i * 6 + j) * stride, s + ((size_t)i * 4 + order[j]) * stride, stride);
        }
        tmp = dst; verts = dst; nv = prim_count * 6;
    }

    /* everything that may flush, then the ring space */
    k.vs = s_ffp_vs_m;
    k.fs = s_ffp_fs_m;
    states_build(&k, &d, -1);
    d.topo = topo;
    if (!(pipe = vkd_pipeline(&k))) { free(tmp); return E_FAIL; }
    stage_textures(&tb);
    if (!vkd_ring_need(VKD_RING_VB, (size_t)nv * stride + 16, 4) ||
        (idx && !vkd_ring_need(VKD_RING_IB, nidx * isz + 16, 4)) ||
        !vkd_ring_need(VKD_RING_UB, ub_pad(sizeof u) + (size_t)vkd_ub_align(), (size_t)vkd_ub_align())) {
        free(tmp);
        return E_OUTOFMEMORY;
    }
    if (!vkd_ring_alloc(VKD_RING_VB, (size_t)nv * stride, 4, &vbuf, &voff, &p)) { free(tmp); return E_OUTOFMEMORY; }
    memcpy(p, verts, (size_t)nv * stride);
    if (idx) {
        if (!vkd_ring_alloc(VKD_RING_IB, nidx * isz, 4, &ibuf, &ioff, &p)) { free(tmp); return E_OUTOFMEMORY; }
        memcpy(p, idx, nidx * isz);
    }
    free(tmp);
    ffp_uniforms(fvf, &u);
    memset(ub, 0, sizeof ub);
    if (!ub_put(&u, sizeof u, sizeof u, &ub[0])) return E_OUTOFMEMORY;
    ffp_layout(fvf, stride, &vl);

    cb = vkd_cmd_render();
    dyn_apply(cb, &d);
    bind_pipeline(cb, pipe);
    vl_apply(cb, &vl);
    {
        VkBuffer bufs[2] = { vbuf, s_const_vb };
        VkDeviceSize offs[2] = { voff, zero };
        vkCmdBindVertexBuffers(cb, 0, 2, bufs, offs);
    }
    push_set(cb, ub, &tb);
    push_consts(cb, pc);
    if (idx) {
        vkCmdBindIndexBuffer(cb, ibuf, ioff, index32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cb, nidx, 1, 0, 0, 0);
    } else {
        vkCmdDraw(cb, nv, 1, 0, 0);
    }
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
    return i >= 0 && s_ps[i].fs != VK_NULL_HANDLE;
}

/* `src` is GLSL 330 (nv2a_psh_generate_glsl), not HLSL, on this backend. */
int d3d8_nv2a_add_ps(unsigned long long key, const char *src, int len)
{
    VkShaderModule fs;
    int slot;
    Uint64 t0;

    if (!g_vkd.initialized || !vkd_on_render_thread() || !s_nv_vs_m || s_ps_count >= NV_PS_CACHE) return 0;
    slot = ps_slot(key, 1);
    if (slot < 0) return 0;
    if (s_ps[slot].used && s_ps[slot].fs) return 1;
    t0 = SDL_GetPerformanceCounter();
    fs = vkd_shader_glsl(1, src, len, "nv2a pixel");
    g_nv_compiles++;
    g_nv_compile_ms += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 /
                       (double)SDL_GetPerformanceFrequency();
    if (!fs) {
        static int dumped;
        if (dumped++ < 2) fprintf(stderr, "[NV2A-PSH] pixel shader %016llX source:\n%.*s\n", key, len, src);
        return 0;
    }
    s_ps[slot].key = key;
    s_ps[slot].fs = fs;
    s_ps[slot].used = 1;
    s_ps_count++;
    return 1;
}

static void nv_layout(uint32_t stride, VtxLayout *l)
{
    static const struct { VkFormat f; uint32_t off; } a[8] = {
        { VK_FORMAT_R32G32B32A32_SFLOAT, 0 }, { VK_FORMAT_R32G32B32A32_SFLOAT, 16 },
        { VK_FORMAT_R32G32B32A32_SFLOAT, 32 }, { VK_FORMAT_R32_SFLOAT, 48 },
        { VK_FORMAT_R32G32B32A32_SFLOAT, 52 }, { VK_FORMAT_R32G32B32A32_SFLOAT, 68 },
        { VK_FORMAT_R32G32B32A32_SFLOAT, 84 }, { VK_FORMAT_R32G32B32A32_SFLOAT, 100 },
    };
    uint32_t i;
    memset(l, 0, sizeof *l);
    vl_binding(l, 0, stride);
    for (i = 0; i < 8; i++) vl_attr(l, i, 0, a[i].f, a[i].off);
}

/* PshConsts is 32 vec4s; the translator hands over at most that. */
#define PSH_UB 512u

HRESULT d3d8_nv2a_draw(D3DPRIMITIVETYPE prim, UINT prim_count, const void *verts, UINT stride,
                       unsigned long long ps_key, const void *ps_consts, UINT ps_consts_size,
                       int raster)
{
    int slot = ps_slot(ps_key, 0);
    VkPrimitiveTopology topo;
    VkdPipeKey k;
    DynState d;
    StageBind tb;
    VtxLayout vl;
    VkPipeline pipe;
    VkCommandBuffer cb;
    VkBuffer vbuf;
    VkDeviceSize voff;
    UbRef ub[3];
    UINT nv;
    void *p;
    float pc[4];

    if (!g_vkd.initialized || !vkd_on_render_thread() || slot < 0 || !s_ps[slot].fs) return E_FAIL;
    if (!verts || stride < 116) return E_INVALIDARG;
    memset(&k, 0, sizeof k);
    switch (prim) {
    case D3DPT_TRIANGLELIST: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; k.topo = VKD_TOPO_TRI;   nv = prim_count * 3; break;
    case D3DPT_LINELIST:     topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;     k.topo = VKD_TOPO_LINE;  nv = prim_count * 2; break;
    case D3DPT_LINESTRIP:    topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;    k.topo = VKD_TOPO_LINE;  nv = prim_count + 1; break;
    case D3DPT_POINTLIST:    topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;    k.topo = VKD_TOPO_POINT; nv = prim_count;     break;
    default: return E_INVALIDARG;
    }
    if (!nv) return S_OK;

    k.vs = s_nv_vs_m;
    k.fs = s_ps[slot].fs;
    states_build(&k, &d, raster);       /* blend, depth, stencil, scissor; raster from the NV2A bits */
    d.topo = topo;
    if (!(pipe = vkd_pipeline(&k))) return E_FAIL;
    stage_textures(&tb);
    if (!vkd_ring_need(VKD_RING_VB, (size_t)nv * stride + 16, 4) ||
        !vkd_ring_need(VKD_RING_UB, PSH_UB + (size_t)vkd_ub_align(), (size_t)vkd_ub_align()))
        return E_OUTOFMEMORY;
    if (!vkd_ring_alloc(VKD_RING_VB, (size_t)nv * stride, 4, &vbuf, &voff, &p)) return E_OUTOFMEMORY;
    memcpy(p, verts, (size_t)nv * stride);
    memset(ub, 0, sizeof ub);
    if (!ub_put(ps_consts, ps_consts_size, PSH_UB, &ub[0])) return E_OUTOFMEMORY;
    nv_layout(stride, &vl);
    pc[0] = (float)d3d8_GetBackbufferWidth();
    pc[1] = (float)d3d8_GetBackbufferHeight();
    if (pc[0] <= 0.0f) pc[0] = 640.0f;
    if (pc[1] <= 0.0f) pc[1] = 480.0f;
    pc[2] = pc[3] = 0.0f;

    cb = vkd_cmd_render();
    dyn_apply(cb, &d);
    bind_pipeline(cb, pipe);
    vl_apply(cb, &vl);
    vkCmdBindVertexBuffers(cb, 0, 1, &vbuf, &voff);
    push_set(cb, ub, &tb);
    push_consts(cb, pc);
    if (g_gpuprof_on) gpuprof_draw_begin();
    vkCmdDraw(cb, nv, 1, 0, 0);
    if (g_gpuprof_on) gpuprof_draw_end();
    return S_OK;
}

/* ================================================================
 * Vertex programs on the GPU (d3d8_nv2a.c's job on D3D11)
 *
 * gpu_prepare() in the translator hands over the decoded program and the
 * title's vertex arrays; the program becomes a vertex shader (d3d8_gl_vsh.c
 * GLSL), cached by program and input kinds, drawn with the draw's combiner
 * pixel shader. Its 192 constants and per-draw parameters are uniform
 * blocks at bindings 1 and 2 (the pixel constants keep 0). A point list is
 * drawn as one instance of six vertices (two triangles) per point, its
 * attributes gathered in draw order and read per instance; a program whose
 * shader does not compile stays on the CPU path.
 * ================================================================ */
int nv2a_vsh_glsl_points(const vshcpu_insn *p, int n, uint16_t inputs, const uint8_t kind[16],
                         int points, char *buf, int cap);

#define VP_CACHE 1024
#define VP_SLOTS (VP_CACHE * 2)
static struct { unsigned long long key; VkShaderModule vs; int used; } s_vp[VP_SLOTS];
static int s_nvp;
static float s_vp_c[192][4];
static int s_vp_c_valid;
static UbRef s_vp_c_ref;                        /* where s_vp_c sits in the ring */
static unsigned s_vp_c_gen;                     /* ...and the ring's generation then */
static struct { unsigned long long hash; uint8_t kind[16]; int points, slot; } s_vp_last = { 0, { 0 }, 0, -1 };

static void vp_shutdown(void)
{
    int i;
    for (i = 0; i < VP_SLOTS; i++) vkd_shader_destroy(s_vp[i].vs);
    memset(s_vp, 0, sizeof s_vp);
    s_nvp = 0;
    s_vp_last.slot = -1;
    s_vp_c_valid = 0;
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
static VkShaderModule vp_shader(const Nv2aVshDraw *d)
{
    static char src[262144];
    uint8_t kind[16];
    unsigned long long key;
    unsigned h;
    int a, n, len, points = d->topology == D3DPT_POINTLIST;
    Uint64 t0;

    for (a = 0; a < 16; a++) kind[a] = (d->inputs & (1u << a)) ? d->attr[a].kind : 0xFF;
    if (s_vp_last.slot >= 0 && s_vp_last.hash == d->prog_hash && s_vp_last.points == points &&
        !memcmp(s_vp_last.kind, kind, 16))
        return s_vp[s_vp_last.slot].vs;
    key = vp_fnv(d->prog_hash, kind, 16);
    if (points) key = vp_fnv(key, "pts", 3);
    h = vp_hash(key, VP_SLOTS - 1);
    for (n = 0; n < VP_SLOTS; n++, h = (h + 1) & (VP_SLOTS - 1)) {
        if (!s_vp[h].used) break;
        if (s_vp[h].key == key) goto found;
    }
    if (n == VP_SLOTS || s_nvp >= VP_CACHE) return VK_NULL_HANDLE;
    s_vp[h].key = key;
    s_vp[h].used = 1;
    s_vp[h].vs = VK_NULL_HANDLE;
    s_nvp++;
    len = nv2a_vsh_glsl_points(d->prog, d->prog_len, d->inputs, kind, points, src, (int)sizeof src);
    if (len > 0) {
        t0 = SDL_GetPerformanceCounter();
        s_vp[h].vs = vkd_shader_glsl(0, src, len, "nv2a vertex program");
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
    s_vp_last.points = points;
    memcpy(s_vp_last.kind, kind, 16);
    s_vp_last.slot = (int)h;
    return s_vp[h].vs;
}

static int vp_format_ok(uint32_t f) { return f < sizeof s_vfmt_ok && s_vfmt_ok[f]; }

int d3d8_nv2a_vsh_ready(const Nv2aVshDraw *d)
{
    int a;
    if (!g_vkd.initialized || !vkd_on_render_thread() || !s_nv_vs_m) return 0;
    if (d->topology == D3DPT_POINTLIST && !d3d8_points_gpu_on()) return 0;
    for (a = 0; a < 16; a++)
        if ((d->inputs & (1u << a)) && (d->attr[a].kind & 0x0F) != NV2A_VSH_IN_CONST &&
            !vp_format_ok(d->attr[a].dxgi_format))
            return 0;
    return vp_shader(d) != VK_NULL_HANDLE;
}

int d3d8_nv2a_draw_program_gpu(const Nv2aVshDraw *d)
{
    struct { const uint8_t *base; uint32_t stride, size, gsize; VkBuffer buf; VkDeviceSize off; } grp[16];
    int gi[16];
    struct { float screen[4], fog[4], flags[4], vattr[16][4], point[4], hud[4]; } params;
    int slot = ps_slot(d->ps_key, 0);
    int need_c, a, g, ngrp = 0;
    uint16_t done = 0;
    VkShaderModule vs;
    VkPrimitiveTopology topo;
    VkdPipeKey k;
    DynState ds;
    StageBind tb;
    VtxLayout vl;
    VkPipeline pipe;
    VkCommandBuffer cb;
    VkBuffer ibuf = VK_NULL_HANDLE;
    VkDeviceSize ioff = 0;
    UbRef ub[3];
    void *p;
    size_t total = 0, al = (size_t)vkd_ub_align();
    double pt = 0.0;                    /* XBOX_PERF sub-zones */
    static const float pc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    int points = d->topology == D3DPT_POINTLIST;

    if (!g_vkd.initialized || !vkd_on_render_thread() || slot < 0 || !s_ps[slot].fs || !d->nindices)
        return 0;
    memset(&k, 0, sizeof k);
    switch (d->topology) {
    case D3DPT_TRIANGLELIST: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; k.topo = VKD_TOPO_TRI;  break;
    case D3DPT_LINELIST:     topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;     k.topo = VKD_TOPO_LINE; break;
    case D3DPT_LINESTRIP:    topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;    k.topo = VKD_TOPO_LINE; break;
    case D3DPT_POINTLIST:    topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; k.topo = VKD_TOPO_TRI;  break;
    default: return 0;
    }
    if (!(vs = vp_shader(d))) return 0;

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
    k.vs = vs;
    k.fs = s_ps[slot].fs;
    states_build(&k, &ds, d->raster);
    ds.topo = topo;
    if (!(pipe = vkd_pipeline(&k))) return 0;
    stage_textures(&tb);

    /* Points: each array gathered in draw order, one element per point. */
    if (points)
        for (g = 0; g < ngrp; g++)
            if (grp[g].stride) grp[g].gsize = d->nindices * grp[g].stride;
    /* ring space for all of this draw, so no allocation is flushed away */
    for (g = 0; g < ngrp; g++) total += ((size_t)(points && grp[g].stride ? grp[g].gsize : grp[g].size) + 15u) & ~(size_t)15u;
    if (total + 16 > vkd_ring_cap(VKD_RING_VB)) return 0;
    if (!vkd_ring_need(VKD_RING_VB, total + 16, 16) ||
        !vkd_ring_need(VKD_RING_IB, (size_t)d->nindices * 4 + 16, 4) ||
        !vkd_ring_need(VKD_RING_UB, ub_pad(sizeof params) + ub_pad(PSH_UB) + ub_pad(sizeof s_vp_c) + al, al))
        return 0;
    for (g = 0; g < ngrp; g++) {
        if (points && grp[g].stride) {
            uint32_t j, st = grp[g].stride;
            uint8_t *o;
            if (!vkd_ring_alloc(VKD_RING_VB, grp[g].gsize, 16, &grp[g].buf, &grp[g].off, &p)) return 0;
            for (j = 0, o = (uint8_t *)p; j < d->nindices; j++, o += st) {
                uint32_t at = d->indices[j] * st;
                memcpy(o, grp[g].base + at, at + st <= grp[g].size ? st : grp[g].size - at);
            }
            continue;
        }
        if (!vkd_ring_alloc(VKD_RING_VB, grp[g].size ? grp[g].size : 16, 16, &grp[g].buf, &grp[g].off, &p)) return 0;
        memcpy(p, grp[g].base, grp[g].size);
    }
    memset(&vl, 0, sizeof vl);
    for (g = 0; g < ngrp; g++) {
        vl_binding(&vl, (uint32_t)g, grp[g].stride);
        if (points) vl.b[vl.nb - 1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    }
    for (a = 0; a < 16; a++) {
        if (!(d->inputs & (1u << a)) || (d->attr[a].kind & 0x0F) == NV2A_VSH_IN_CONST) continue;
        if (!vp_format_ok(d->attr[a].dxgi_format)) return 0;
        g = gi[a];
        vl_attr(&vl, (uint32_t)a, (uint32_t)g, vp_vkformat(d->attr[a].dxgi_format),
                (uint32_t)(d->attr[a].base - grp[g].base));
    }
    if (!points) {
        if (!vkd_ring_alloc(VKD_RING_IB, (size_t)d->nindices * 4, 4, &ibuf, &ioff, &p)) return 0;
        memcpy(p, d->indices, (size_t)d->nindices * 4);
    }
    if (g_perf_on) { double t = perf_now(); perf_add(PZ_DUP, t - pt); pt = t; }

    /* constants: uploaded again only when they changed or the ring started over */
    need_c = !s_vp_c_valid || s_vp_c_gen != g_vk.ring_gen[VKD_RING_UB] ||
             memcmp(s_vp_c, d->vconst, sizeof s_vp_c);
    memset(ub, 0, sizeof ub);
    if (need_c) {
        memcpy(s_vp_c, d->vconst, sizeof s_vp_c);
        if (!ub_put(s_vp_c, sizeof s_vp_c, sizeof s_vp_c, &s_vp_c_ref)) { s_vp_c_valid = 0; return 0; }
        s_vp_c_gen = g_vk.ring_gen[VKD_RING_UB];
        s_vp_c_valid = 1;
    }
    ub[1] = s_vp_c_ref;
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
    params.point[0] = d->point_params ? 1.0f : 0.0f;
    params.point[1] = d->point_size;
    params.point[2] = d->point_smooth ? 1.0f : 0.0f;
    params.point[3] = d->point_kx;
    if (d->attr_const) memcpy(params.vattr, d->attr_const, sizeof params.vattr);
    if (!ub_put(&params, sizeof params, sizeof params, &ub[2]) ||
        !ub_put(d->ps_consts, d->ps_consts_size, PSH_UB, &ub[0]))
        return 0;
    if (g_perf_on) { double t = perf_now(); perf_add(PZ_DCB, t - pt); pt = t; }

    cb = vkd_cmd_render();
    dyn_apply(cb, &ds);
    bind_pipeline(cb, pipe);
    vl_apply(cb, &vl);
    for (g = 0; g < ngrp; g++) vkCmdBindVertexBuffers(cb, (uint32_t)g, 1, &grp[g].buf, &grp[g].off);
    if (!points) vkCmdBindIndexBuffer(cb, ibuf, ioff, VK_INDEX_TYPE_UINT32);
    push_set(cb, ub, &tb);
    push_consts(cb, pc);
    if (g_perf_on) { double t = perf_now(); perf_add(PZ_DSTATE, t - pt); pt = t; }
    if (g_gpuprof_on) gpuprof_draw_begin();
    if (points) vkCmdDraw(cb, 6, d->nindices, 0, 0);
    else        vkCmdDrawIndexed(cb, d->nindices, 1, 0, 0, 0);
    if (g_gpuprof_on) gpuprof_draw_end();
    if (g_perf_on) { perf_add(PZ_DDRAW, perf_now() - pt); perf_gpu_draw(0, (unsigned)ngrp); }
    return 1;
}

/* Point sprites on the GPU (instanced squares, above); XBOX_FIX_POINTS_GPU=0
 * puts them back on the CPU path. */
int d3d8_points_gpu_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("XBOX_FIX_POINTS_GPU"); on = !(e && e[0] == '0'); }
    return on;
}
int d3d8_points_check_on(void) { return 0; }     /* diagnostic of the D3D11 point path */
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
