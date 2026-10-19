/*
 * nv2a_dxgi_shim.h -- the DXGI_FORMAT values nv2a_pgraph_d3d11.c names, for
 * hosts without the Windows SDK. Same numbers as <dxgiformat.h>; they only
 * travel in Nv2aVshDraw.attr[].dxgi_format to the D3D layer, which on the
 * OpenGL backend does not take the GPU vertex-program path at all.
 */
#ifndef NV2A_DXGI_SHIM_H
#define NV2A_DXGI_SHIM_H
#ifndef _WIN32
enum {
    DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
    DXGI_FORMAT_R32G32B32_FLOAT    = 6,
    DXGI_FORMAT_R16G16B16A16_SNORM = 13,
    DXGI_FORMAT_R16G16B16A16_SINT  = 14,
    DXGI_FORMAT_R32G32_FLOAT       = 16,
    DXGI_FORMAT_R8G8B8A8_UNORM     = 28,
    DXGI_FORMAT_R16G16_SNORM       = 37,
    DXGI_FORMAT_R16G16_SINT        = 38,
    DXGI_FORMAT_R32_FLOAT          = 41,
    DXGI_FORMAT_R32_UINT           = 42,
    DXGI_FORMAT_R8G8_UNORM         = 49,
    DXGI_FORMAT_R16_SNORM          = 58,
    DXGI_FORMAT_R16_SINT           = 59,
    DXGI_FORMAT_R8_UNORM           = 61,
    DXGI_FORMAT_B8G8R8A8_UNORM     = 87
};
#endif
#endif
