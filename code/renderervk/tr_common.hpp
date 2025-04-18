/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
#ifndef TR_COMMON_HPP
#define TR_COMMON_HPP

#define USE_VULKAN

#include <type_traits>

// #include "surfaceflags.hpp"
extern "C"
{
#include "../qcommon/q_shared.h"
#include "../renderercommon/tr_public.h"
}

constexpr int MAX_TEXTURE_UNITS = 8;
// any change in the LIGHTMAP_* defines here MUST be reflected in
// R_FindShader() in tr_bsp.c
constexpr int LIGHTMAP_2D = -4;		   // shader is for 2D rendering
constexpr int LIGHTMAP_BY_VERTEX = -3; // pre-lit triangle models
constexpr int LIGHTMAP_WHITEIMAGE = -2;
constexpr int LIGHTMAP_NONE = -1;

enum class imgFlags_t : uint16_t
{
	IMGFLAG_NONE = 0x0000,
	IMGFLAG_MIPMAP = 0x0001,
	IMGFLAG_PICMIP = 0x0002,
	IMGFLAG_CLAMPTOEDGE = 0x0004,
	IMGFLAG_CLAMPTOBORDER = 0x0008,
	IMGFLAG_NO_COMPRESSION = 0x0010,
	IMGFLAG_NOLIGHTSCALE = 0x0020,
	IMGFLAG_LIGHTMAP = 0x0040,
	IMGFLAG_NOSCALE = 0x0080,
	IMGFLAG_RGB = 0x0100,
	IMGFLAG_COLORSHIFT = 0x0200,
};

constexpr imgFlags_t operator|(imgFlags_t lhs, imgFlags_t rhs) {
    using underlying = std::underlying_type_t<imgFlags_t>;
    return static_cast<imgFlags_t>(static_cast<underlying>(lhs) | static_cast<underlying>(rhs));
}

constexpr imgFlags_t operator&(imgFlags_t lhs, imgFlags_t rhs) {
    using underlying = std::underlying_type_t<imgFlags_t>;
    return static_cast<imgFlags_t>(static_cast<underlying>(lhs) & static_cast<underlying>(rhs));
}

constexpr imgFlags_t operator~(imgFlags_t flag) {
    using underlying = std::underlying_type_t<imgFlags_t>;
    return static_cast<imgFlags_t>(~static_cast<underlying>(flag));
}

constexpr imgFlags_t& operator|=(imgFlags_t& lhs, imgFlags_t rhs) {
    lhs = lhs | rhs;
    return lhs;
}

constexpr imgFlags_t& operator&=(imgFlags_t& lhs, imgFlags_t rhs) {
    lhs = lhs & rhs;
    return lhs;
}

constexpr bool HasFlag(imgFlags_t mask, imgFlags_t flag) {
    return (static_cast<std::underlying_type_t<imgFlags_t>>(mask) & 
            static_cast<std::underlying_type_t<imgFlags_t>>(flag)) != 0;
}

enum class cullType_t : uint8_t
{
	CT_FRONT_SIDED = 0,
	CT_BACK_SIDED,
	CT_TWO_SIDED
};

typedef struct image_s image_t;

extern glconfig_t glConfig; // outside of TR since it shouldn't be cleared during ref re-init

// These variables should live inside glConfig but can't because of
// compatibility issues to the original ID vms.  If you release a stand-alone
// game and your mod uses tr_types.h from this build you can safely move them
// to the glconfig_t struct.
extern bool textureFilterAnisotropic;
extern int maxAnisotropy;

//
// cvars
//
//extern cvar_t *r_stencilbits; // number of desired stencil bits
extern cvar_t *r_texturebits; // number of desired texture bits
							  // 0 = use framebuffer depth
							  // 16 = use 16-bit textures
							  // 32 = use 32-bit textures
							  // all else = error

extern cvar_t *r_drawBuffer;

extern cvar_t *r_allowExtensions;		  // global enable/disable of OpenGL extensions
extern cvar_t *r_ext_compressed_textures; // these control use of specific extensions
extern cvar_t *r_ext_multitexture;
extern cvar_t *r_ext_compiled_vertex_array;
extern cvar_t *r_ext_texture_env_add;

extern cvar_t *r_ext_texture_filter_anisotropic;
extern cvar_t *r_ext_max_anisotropy;

#ifdef __cplusplus
extern "C"
{
#endif

	float R_NoiseGet4f(float x, float y, float z, double t);
	void R_NoiseInit(void);

	// font stuff
	void R_InitFreeType(void);
	void R_DoneFreeType(void);
	void RE_RegisterFont(const char *fontName, int pointSize, fontInfo_t *font);

	/*
	=============================================================

	IMAGE LOADERS

	=============================================================
	*/

	void R_LoadBMP(const char *name, byte **pic, int *width, int *height);
	void R_LoadJPG(const char *name, byte **pic, int *width, int *height);
	void R_LoadPCX(const char *name, byte **pic, int *width, int *height);
	void R_LoadPNG(const char *name, byte **pic, int *width, int *height);
	void R_LoadTGA(const char *name, byte **pic, int *width, int *height);

#ifdef __cplusplus
}
#endif

/*
====================================================================

IMPLEMENTATION SPECIFIC FUNCTIONS

====================================================================
*/

#endif // TR_COMMON_HPP
