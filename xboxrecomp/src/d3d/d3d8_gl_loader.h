/*
 * d3d8_gl_loader.h -- the one place the OpenGL backend picks its loader.
 *
 *   Linux (and other desktop POSIX):  libepoxy, <epoxy/gl.h>; nothing to load.
 *   Nintendo Switch (__SWITCH__):     glad, <glad/glad.h>; d3d8_gl_load() calls
 *                                     gladLoadGLLoader(SDL_GL_GetProcAddress)
 *                                     once the context exists. Mesa (nouveau)
 *                                     gives desktop GL there, so the same
 *                                     GLSL 330 core shaders are used.
 *
 * glad on Switch is generated for core GL 4.3 without extensions, so the few
 * extension enums the backend uses are defined here when missing; each one
 * is only used after a runtime extension check.
 */
#ifndef D3D8_GL_LOADER_H
#define D3D8_GL_LOADER_H

#if defined(__SWITCH__)
#  include <glad/glad.h>
#else
#  include <epoxy/gl.h>
#endif

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif
#ifndef GL_MIRROR_CLAMP_TO_EDGE
#define GL_MIRROR_CLAMP_TO_EDGE 0x8743
#endif

/* Resolve the GL entry points for the context current on this thread.
 * Returns 1 on success. Defined in d3d8_gl_device.c. */
int d3d8_gl_load(void);

#endif /* D3D8_GL_LOADER_H */
