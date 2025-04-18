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
// tr_models.c -- model loading and caching

#include "string_operations.hpp"
#include "tr_model.hpp"
#include "tr_animation.hpp"
#include "tr_scene.hpp"
#include "vk_flares.hpp"
#include "tr_model_iqm.hpp"
#include "tr_shader.hpp"
#include "math.hpp"

#include <functional>
#include <string_view>
#include "utils.hpp"

#define LL(x) x = LittleLong(x)

static bool R_LoadMD3(model_t &mod, int lod, void *buffer, std::size_t fileSize, std::string_view name);
static bool R_LoadMDR(model_t &mod, void *buffer, int filesize, std::string_view name);

/*
====================
R_RegisterMD3
====================
*/
static qhandle_t R_RegisterMD3(std::string_view name, model_t &mod)
{
	union
	{
		uint32_t *u;
		void *v;
	} buf{};
	int lod;
	uint32_t ident;
	bool loaded = false;
	int numLoaded;
	std::size_t fileSize;
	char filename[MAX_QPATH], namebuf[MAX_QPATH + 20];
	char *fext, defex[] = "md3";

	numLoaded = 0;

	strcpy(filename, name.data());

	fext = strchr(filename, '.');
	if (!fext)
		fext = defex;
	else
	{
		*fext = '\0';
		fext++;
	}

	for (lod = MD3_MAX_LODS - 1; lod >= 0; lod--)
	{
		if (lod)
			Com_sprintf(namebuf, sizeof(namebuf), "%s_%d.%s", filename, lod, fext);
		else
			Com_sprintf(namebuf, sizeof(namebuf), "%s.%s", filename, fext);

		fileSize = static_cast<std::size_t>(ri.FS_ReadFile(namebuf, &buf.v));
		if (!buf.v)
			continue;

		if (fileSize < sizeof(md3Header_t))
		{
			ri.Printf(PRINT_WARNING, "%s: truncated header for %s\n", __func__, name.data());
			ri.FS_FreeFile(buf.v);
			break;
		}

		ident = LittleLong(*buf.u);
		if (ident == MD3_IDENT)
			loaded = R_LoadMD3(mod, lod, buf.v, fileSize, name);
		else
			ri.Printf(PRINT_WARNING, "%s: unknown fileid for %s\n", __func__, name.data());

		ri.FS_FreeFile(buf.v);

		if (loaded)
		{
			mod.numLods++;
			numLoaded++;
		}
		else
			break;
	}

	if (numLoaded)
	{
		// duplicate into higher lod spots that weren't
		// loaded, in case the user changes r_lodbias on the fly
		for (lod--; lod >= 0; lod--)
		{
			mod.numLods++;
			mod.md3[lod] = mod.md3[lod + 1];
		}

		return mod.index;
	}

	ri.Printf(PRINT_DEVELOPER, S_COLOR_YELLOW "%s: couldn't load %s\n", __func__, name.data());

	mod.type = modtype_t::MOD_BAD;
	return 0;
}

/*
====================
R_RegisterMDR
====================
*/
static qhandle_t R_RegisterMDR(std::string_view name, model_t &mod)
{
	union
	{
		uint32_t *u;
		void *v;
	} buf{};
	uint32_t ident;
	bool loaded = false;
	int filesize;

	filesize = ri.FS_ReadFile(name.data(), &buf.v);
	if (!buf.v)
	{
		mod.type = modtype_t::MOD_BAD;
		return 0;
	}

	if (static_cast<std::size_t>(filesize) < sizeof(ident))
	{
		ri.FS_FreeFile(buf.v);
		mod.type = modtype_t::MOD_BAD;
		return 0;
	}

	ident = LittleLong(*buf.u);
	if (ident == MDR_IDENT)
		loaded = R_LoadMDR(mod, buf.v, filesize, name);

	ri.FS_FreeFile(buf.v);

	if (!loaded)
	{
		ri.Printf(PRINT_WARNING, "%s: couldn't load %s\n", __func__, name.data());
		mod.type = modtype_t::MOD_BAD;
		return 0;
	}

	return mod.index;
}

/*
====================
R_RegisterIQM
====================
*/
static qhandle_t R_RegisterIQM(std::string_view name, model_t &mod)
{
	union
	{
		unsigned *u;
		void *v;
	} buf{};
	bool loaded = false;
	int filesize;

	filesize = ri.FS_ReadFile(name.data(), (void **)&buf.v);
	if (!buf.u)
	{
		mod.type = modtype_t::MOD_BAD;
		return 0;
	}

	loaded = R_LoadIQM(mod, buf.u, filesize, name);

	ri.FS_FreeFile(buf.v);

	if (!loaded)
	{
		ri.Printf(PRINT_WARNING, "%s: couldn't load %s\n", __func__, name.data());
		mod.type = modtype_t::MOD_BAD;
		return 0;
	}

	return mod.index;
}

typedef struct
{
	std::string_view ext;
	qhandle_t (*ModelLoader)(std::string_view, model_t &);
} modelExtToLoaderMap_t;

// Note that the ordering indicates the order of preference used
// when there are multiple models of different formats available
static modelExtToLoaderMap_t modelLoaders[] =
	{
		{"iqm", R_RegisterIQM},
		{"mdr", R_RegisterMDR},
		{"md3", R_RegisterMD3}};

static constexpr int numModelLoaders = static_cast<int>(arrayLen(modelLoaders));

//===============================================================================

/*
** R_GetModelByHandle
*/
model_t *R_GetModelByHandle(qhandle_t index)
{
	model_t *mod;

	// out of range gets the default model
	if (index < 1 || index >= tr.numModels)
	{
		return tr.models[0];
	}

	mod = tr.models[index];

	return mod;
}

//===============================================================================

/*
** R_AllocModel
*/
model_t *R_AllocModel(void)
{
	if (tr.numModels >= MAX_MOD_KNOWN)
	{
		return NULL;
	}

	model_t *mod = static_cast<model_t *>(ri.Hunk_Alloc(sizeof(*tr.models[tr.numModels]), h_low));
	mod->index = tr.numModels;
	tr.models[tr.numModels] = mod;
	tr.numModels++;

	return mod;
}

/*
====================
RE_RegisterModel

Loads in a model for the given name

Zero will be returned if the model fails to load.
An entry will be retained for failed models as an
optimization to prevent disk rescanning if they are
asked for again.
====================
*/
qhandle_t RE_RegisterModel(const char *name)
{
	model_t *mod;
	qhandle_t hModel;
	bool orgNameFailed = false;
	int orgLoader = -1;
	int i;
	std::array<char, MAX_QPATH> localName;
	std::array<char, MAX_QPATH> altName{};

	if (!name || !name[0])
	{
		ri.Printf(PRINT_ALL, "RE_RegisterModel: NULL name\n");
		return 0;
	}

	if (strlen(name) >= MAX_QPATH)
	{
		ri.Printf(PRINT_ALL, "Model name exceeds MAX_QPATH\n");
		return 0;
	}

	//
	// search the currently loaded models
	//
	for (hModel = 1; hModel < tr.numModels; hModel++)
	{
		mod = tr.models[hModel];
		if (!strcmp(mod->name.data(), name))
		{
			if (mod->type == modtype_t::MOD_BAD)
			{
				return 0;
			}
			return hModel;
		}
	}

	// allocate a new model_t

	if ((mod = R_AllocModel()) == NULL)
	{
		ri.Printf(PRINT_WARNING, "RE_RegisterModel: R_AllocModel() failed for '%s'\n", name);
		return 0;
	}

	// only set the name after the model has been successfully loaded
	Q_strncpyz_cpp(mod->name, name, sizeof(mod->name));

	mod->type = modtype_t::MOD_BAD;
	mod->numLods = 0;

	//
	// load the files
	//
	Q_strncpyz_cpp(localName, name, MAX_QPATH);

	std::string_view ext = COM_GetExtension_cpp(localName);

	if (!ext.empty())
	{
		// Look for the correct loader and use it
		for (i = 0; i < numModelLoaders; i++)
		{
			if (!Q_stricmp_cpp(ext, modelLoaders[i].ext))
			{
				// Load
				hModel = modelLoaders[i].ModelLoader(localName.data(), *mod);
				break;
			}
		}

		// A loader was found
		if (i < numModelLoaders)
		{
			if (!hModel)
			{
				// Loader failed, most likely because the file isn't there;
				// try again without the extension
				orgNameFailed = true;
				orgLoader = i;
				COM_StripExtension_cpp(std::string_view(name), localName);
			}
			else
			{
				// Something loaded
				return mod->index;
			}
		}
	}

	// Try and find a suitable match using all
	// the model formats supported
	for (i = 0; i < numModelLoaders; i++)
	{
		if (i == orgLoader)
			continue;

		Com_sprintf(altName.data(), sizeof(altName), "%s.%s", localName.data(), modelLoaders[i].ext.data());

		// Load
		hModel = modelLoaders[i].ModelLoader(std::string_view(altName.data(), altName.size()), *mod);

		if (hModel)
		{
			if (orgNameFailed)
			{
				ri.Printf(PRINT_DEVELOPER, "WARNING: %s not present, using %s instead\n",
						  name, altName.data());
			}

			break;
		}
	}

	return hModel;
}

/*
=================
R_LoadMD3
=================
*/
static bool R_LoadMD3(model_t &mod, const int lod, void *buffer, const std::size_t fileSize, std::string_view mod_name)
{
	int i, j;
	md3Header_t *pinmodel, *hdr;
	md3Frame_t *frame;
	md3Surface_t *surf;
	md3Shader_t *shader;
	md3Triangle_t *tri;
	md3St_t *st;
	md3XyzNormal_t *xyz;
	md3Tag_t *tag;
	int version;
	uint32_t size;

	pinmodel = (md3Header_t *)buffer;

	version = LittleLong(pinmodel->version);
	if (version != MD3_VERSION)
	{
		ri.Printf(PRINT_WARNING, "%s: %s has wrong version (%i should be %i)\n", __func__, mod_name.data(), version, MD3_VERSION);
		return false;
	}

	size = LittleLong(pinmodel->ofsEnd);

	if (static_cast<std::size_t>(size) > fileSize)
	{
		ri.Printf(PRINT_WARNING, "%s: %s has corrupted header\n", __func__, mod_name.data());
		return false;
	}

	mod.type = modtype_t::MOD_MESH;
	mod.dataSize += size;
	mod.md3[lod] = reinterpret_cast<md3Header_t *>(ri.Hunk_Alloc(size, h_low));

	Com_Memcpy(mod.md3[lod], buffer, size);

	hdr = mod.md3[lod];

	LL(hdr->ident);
	LL(hdr->version);
	LL(hdr->numFrames);
	LL(hdr->numTags);
	LL(hdr->numSurfaces);
	LL(hdr->numSkins);
	LL(hdr->ofsFrames);
	LL(hdr->ofsTags);
	LL(hdr->ofsSurfaces);
	LL(hdr->ofsEnd);

	if (hdr->numFrames < 1)
	{
		ri.Printf(PRINT_WARNING, "%s: %s has no frames\n", __func__, mod_name.data());
		return false;
	}

	if (hdr->ofsFrames > size || hdr->ofsTags > size || hdr->ofsSurfaces > size)
	{
		ri.Printf(PRINT_WARNING, "%s: %s has corrupted header\n", __func__, mod_name.data());
		return false;
	}
	if ((unsigned)(hdr->numFrames | hdr->numTags | hdr->numSkins) > (1 << 20))
	{
		ri.Printf(PRINT_WARNING, "%s: %s has corrupted header\n", __func__, mod_name.data());
		return false;
	}

	if (hdr->ofsFrames + hdr->numFrames * sizeof(md3Frame_t) > fileSize)
	{
		ri.Printf(PRINT_WARNING, "%s: %s has corrupted header\n", __func__, mod_name.data());
		return false;
	}
	if (hdr->ofsTags + hdr->numTags * hdr->numFrames * sizeof(md3Tag_t) > fileSize)
	{
		ri.Printf(PRINT_WARNING, "%s: %s has corrupted header\n", __func__, mod_name.data());
		return false;
	}
	if (hdr->ofsSurfaces + (hdr->numSurfaces ? 1 : 0) * sizeof(md3Surface_t) > fileSize)
	{
		ri.Printf(PRINT_WARNING, "%s: %s has corrupted header\n", __func__, mod_name.data());
		return false;
	}

	// swap all the frames
	frame = (md3Frame_t *)((byte *)hdr + hdr->ofsFrames);
	for (i = 0; i < hdr->numFrames; i++, frame++)
	{
		frame->radius = LittleFloat(frame->radius);
		for (j = 0; j < 3; j++)
		{
			frame->bounds[0][j] = LittleFloat(frame->bounds[0][j]);
			frame->bounds[1][j] = LittleFloat(frame->bounds[1][j]);
			frame->localOrigin[j] = LittleFloat(frame->localOrigin[j]);
		}
	}

	// swap all the tags
	tag = (md3Tag_t *)((byte *)hdr + hdr->ofsTags);
	for (i = 0; i < hdr->numTags * hdr->numFrames; i++, tag++)
	{
		// zero-terminate tag name
		tag->name[sizeof(tag->name) - 1] = '\0';
		for (j = 0; j < 3; j++)
		{
			tag->origin[j] = LittleFloat(tag->origin[j]);
			tag->axis[0][j] = LittleFloat(tag->axis[0][j]);
			tag->axis[1][j] = LittleFloat(tag->axis[1][j]);
			tag->axis[2][j] = LittleFloat(tag->axis[2][j]);
		}
	}

	// swap all the surfaces
	surf = (md3Surface_t *)((byte *)hdr + hdr->ofsSurfaces);
	for (i = 0; i < hdr->numSurfaces; i++)
	{

		LL(surf->ident);
		LL(surf->flags);
		LL(surf->numFrames);
		LL(surf->numShaders);
		LL(surf->numTriangles);
		LL(surf->numVerts);
		LL(surf->ofsTriangles);
		LL(surf->ofsShaders);
		LL(surf->ofsSt);
		LL(surf->ofsXyzNormals);
		LL(surf->ofsEnd);

		if (static_cast<std::size_t>(surf->ofsEnd > fileSize || (((byte *)surf - (byte *)hdr) + surf->ofsEnd)) > fileSize)
		{
			ri.Printf(PRINT_WARNING, "%s: %s has corrupted surface header\n", __func__, mod_name.data());
			return false;
		}
		if (surf->ofsTriangles > fileSize || surf->ofsShaders > fileSize || surf->ofsSt > fileSize || surf->ofsXyzNormals > fileSize)
		{
			ri.Printf(PRINT_WARNING, "%s: %s has corrupted surface header\n", __func__, mod_name.data());
			return false;
		}
		if (surf->ofsTriangles + surf->numTriangles * sizeof(md3Triangle_t) > fileSize)
		{
			ri.Printf(PRINT_WARNING, "%s: %s has corrupted surface header\n", __func__, mod_name.data());
			return false;
		}
		if (surf->ofsShaders + surf->numShaders * sizeof(md3Shader_t) > fileSize || surf->numShaders > (1 << 20))
		{
			ri.Printf(PRINT_WARNING, "%s: %s has corrupted surface header\n", __func__, mod_name.data());
			return false;
		}
		if (surf->ofsSt + surf->numVerts * sizeof(md3St_t) > fileSize)
		{
			ri.Printf(PRINT_WARNING, "%s: %s has corrupted surface header\n", __func__, mod_name.data());
			return false;
		}
		if (surf->ofsXyzNormals + surf->numVerts * sizeof(md3XyzNormal_t) > fileSize)
		{
			ri.Printf(PRINT_WARNING, "%s: %s has corrupted surface header\n", __func__, mod_name.data());
			return false;
		}

		if (surf->numVerts >= SHADER_MAX_VERTEXES)
		{
			ri.Printf(PRINT_WARNING, "%s: %s has more than %i verts on %s (%i).\n", __func__,
					  mod_name.data(), SHADER_MAX_VERTEXES - 1, surf->name[0] ? surf->name : "a surface",
					  surf->numVerts);
			return false;
		}
		if (surf->numTriangles * 3 >= SHADER_MAX_INDEXES)
		{
			ri.Printf(PRINT_WARNING, "%s: %s has more than %i triangles on %s (%i).\n", __func__,
					  mod_name.data(), (SHADER_MAX_INDEXES / 3) - 1, surf->name[0] ? surf->name : "a surface",
					  surf->numTriangles);
			return false;
		}

		// change to surface identifier
		surf->ident = static_cast<int>(surfaceType_t::SF_MD3);

		// zero-terminate surface name
		surf->name[sizeof(surf->name) - 1] = '\0';

		// lowercase the surface name so skin compares are faster
		//Q_strlwr(surf->name);
		q_strlwr_cpp(std::span(surf->name));

		// strip off a trailing _1 or _2
		// this is a crutch for q3data being a mess
		j = strlen(surf->name);
		if (j > 2 && surf->name[j - 2] == '_')
		{
			surf->name[j - 2] = 0;
		}

		// register the shaders
		shader = (md3Shader_t *)((byte *)surf + surf->ofsShaders);

		for (j = 0; j < surf->numShaders; j++, shader++)
		{
			shader_t *sh;

			// zero-terminate shader name
			shader->name[sizeof(shader->name) - 1] = '\0';

			sh = R_FindShader(shader->name, LIGHTMAP_NONE, true);
			if (sh->defaultShader)
			{
				shader->shaderIndex = 0;
			}
			else
			{
				shader->shaderIndex = sh->index;
			}
		}

		// swap all the triangles
		tri = (md3Triangle_t *)((byte *)surf + surf->ofsTriangles);
		for (j = 0; j < surf->numTriangles; j++, tri++)
		{
			LL(tri->indexes[0]);
			LL(tri->indexes[1]);
			LL(tri->indexes[2]);
		}

		// swap all the ST
		st = (md3St_t *)((byte *)surf + surf->ofsSt);
		for (j = 0; j < surf->numVerts; j++, st++)
		{
			st->st[0] = LittleFloat(st->st[0]);
			st->st[1] = LittleFloat(st->st[1]);
		}

		// swap all the XyzNormals
		xyz = (md3XyzNormal_t *)((byte *)surf + surf->ofsXyzNormals);
		for (j = 0; j < surf->numVerts * surf->numFrames; j++, xyz++)
		{
			xyz->xyz[0] = LittleShort(xyz->xyz[0]);
			xyz->xyz[1] = LittleShort(xyz->xyz[1]);
			xyz->xyz[2] = LittleShort(xyz->xyz[2]);

			xyz->normal = LittleShort(xyz->normal);
		}

		// find the next surface
		surf = (md3Surface_t *)((byte *)surf + surf->ofsEnd);
	}

	return true;
}

/*
=================
R_LoadMDR
=================
*/
static bool R_LoadMDR(model_t &mod, void *buffer, const int filesize, std::string_view mod_name)
{
	int i, j, k, l;
	mdrHeader_t *pinmodel, *mdr;
	mdrFrame_t *frame;
	mdrLOD_t *lod, *curlod;
	mdrSurface_t *surf, *cursurf;
	mdrTriangle_t *tri, *curtri;
	mdrVertex_t *v, *curv;
	mdrWeight_t *weight, *curweight;
	mdrTag_t *tag, *curtag;
	int size;
	shader_t *sh;

	pinmodel = (mdrHeader_t *)buffer;

	pinmodel->version = LittleLong(pinmodel->version);
	if (pinmodel->version != MDR_VERSION)
	{
		ri.Printf(PRINT_WARNING, "%s: %s has wrong version (%i should be %i)\n", __func__, mod_name.data(), pinmodel->version, MDR_VERSION);
		return false;
	}

	size = LittleLong(pinmodel->ofsEnd);

	if (size > filesize)
	{
		ri.Printf(PRINT_WARNING, "%s: Header of %s is broken. Wrong filesize declared!\n", __func__, mod_name.data());
		return false;
	}

	mod.type = modtype_t::MOD_MDR;

	LL(pinmodel->numFrames);
	LL(pinmodel->numBones);
	LL(pinmodel->ofsFrames);

	// This is a model that uses some type of compressed Bones. We don't want to uncompress every bone for each rendered frame
	// over and over again, we'll uncompress it in this function already, so we must adjust the size of the target mdr.
	if (pinmodel->ofsFrames < 0)
	{
		// mdrFrame_t is larger than mdrCompFrame_t:
		size += pinmodel->numFrames * sizeof(frame->name);
		// now add enough space for the uncompressed bones.
		size += pinmodel->numFrames * pinmodel->numBones * ((sizeof(mdrBone_t) - sizeof(mdrCompBone_t)));
	}

	// simple bounds check
	if (pinmodel->numBones < 0 ||
		static_cast<int>(sizeof(*mdr) + pinmodel->numFrames * (sizeof(*frame) + (pinmodel->numBones - 1) * sizeof(*frame->bones))) > size)
	{
		ri.Printf(PRINT_WARNING, "R_LoadMDR: %s has broken structure.\n", mod_name.data());
		return false;
	}

	mod.dataSize += size;
	mod.modelData = mdr = reinterpret_cast<mdrHeader_t *>(ri.Hunk_Alloc(size, h_low));

	// Copy all the values over from the file and fix endian issues in the process, if necessary.

	mdr->ident = LittleLong(pinmodel->ident);
	mdr->version = pinmodel->version; // Don't need to swap byte order on this one, we already did above.
	Q_strncpyz(mdr->name, pinmodel->name, sizeof(mdr->name));
	mdr->numFrames = pinmodel->numFrames;
	mdr->numBones = pinmodel->numBones;
	mdr->numLODs = LittleLong(pinmodel->numLODs);
	mdr->numTags = LittleLong(pinmodel->numTags);
	// We don't care about the other offset values, we'll generate them ourselves while loading.

	mod.numLods = mdr->numLODs;

	if (mdr->numFrames < 1)
	{
		ri.Printf(PRINT_WARNING, "R_LoadMDR: %s has no frames\n", mod_name.data());
		return false;
	}

	/* The first frame will be put into the first free space after the header */
	frame = (mdrFrame_t *)(mdr + 1);
	mdr->ofsFrames = (int)((byte *)frame - (byte *)mdr);

	if (pinmodel->ofsFrames < 0)
	{
		mdrCompFrame_t *cframe;

		// compressed model...
		cframe = (mdrCompFrame_t *)((byte *)pinmodel - pinmodel->ofsFrames);

		for (i = 0; i < mdr->numFrames; i++)
		{
			for (j = 0; j < 3; j++)
			{
				frame->bounds[0][j] = LittleFloat(cframe->bounds[0][j]);
				frame->bounds[1][j] = LittleFloat(cframe->bounds[1][j]);
				frame->localOrigin[j] = LittleFloat(cframe->localOrigin[j]);
			}

			frame->radius = LittleFloat(cframe->radius);
			frame->name[0] = '\0'; // No name supplied in the compressed version.

			for (j = 0; j < mdr->numBones; j++)
			{
				for (k = 0; k < static_cast<int>((sizeof(cframe->bones[j].Comp) / 2)); k++)
				{
					// Do swapping for the uncompressing functions. They seem to use shorts
					// values only, so I assume this will work. Never tested it on other
					// platforms, though.

					((unsigned short *)(cframe->bones[j].Comp))[k] =
						LittleShort(((unsigned short *)(cframe->bones[j].Comp))[k]);
				}

				/* Now do the actual uncompressing */
				MC_UnCompress(frame->bones[j].matrix, cframe->bones[j].Comp);
			}

			// Next Frame...
			cframe = (mdrCompFrame_t *)&cframe->bones[j];
			frame = (mdrFrame_t *)&frame->bones[j];
		}
	}
	else
	{
		mdrFrame_t *curframe;

		// uncompressed model...
		//

		curframe = (mdrFrame_t *)((byte *)pinmodel + pinmodel->ofsFrames);

		// swap all the frames
		for (i = 0; i < mdr->numFrames; i++)
		{
			for (j = 0; j < 3; j++)
			{
				frame->bounds[0][j] = LittleFloat(curframe->bounds[0][j]);
				frame->bounds[1][j] = LittleFloat(curframe->bounds[1][j]);
				frame->localOrigin[j] = LittleFloat(curframe->localOrigin[j]);
			}

			frame->radius = LittleFloat(curframe->radius);
			Q_strncpyz(frame->name, curframe->name, sizeof(frame->name));

			for (j = 0; j < (int)(mdr->numBones * sizeof(mdrBone_t) / 4); j++)
			{
				((float *)frame->bones)[j] = LittleFloat(((float *)curframe->bones)[j]);
			}

			curframe = (mdrFrame_t *)&curframe->bones[mdr->numBones];
			frame = (mdrFrame_t *)&frame->bones[mdr->numBones];
		}
	}

	// frame should now point to the first free address after all frames.
	lod = (mdrLOD_t *)frame;
	mdr->ofsLODs = (int)((byte *)lod - (byte *)mdr);

	curlod = (mdrLOD_t *)((byte *)pinmodel + LittleLong(pinmodel->ofsLODs));

	// swap all the LOD's
	for (l = 0; l < mdr->numLODs; l++)
	{
		// simple bounds check
		if ((byte *)(lod + 1) > (byte *)mdr + size)
		{
			ri.Printf(PRINT_WARNING, "R_LoadMDR: %s has broken structure.\n", mod_name.data());
			return false;
		}

		lod->numSurfaces = LittleLong(curlod->numSurfaces);

		// swap all the surfaces
		surf = (mdrSurface_t *)(lod + 1);
		lod->ofsSurfaces = (int)((byte *)surf - (byte *)lod);
		cursurf = (mdrSurface_t *)((byte *)curlod + LittleLong(curlod->ofsSurfaces));

		for (i = 0; i < lod->numSurfaces; i++)
		{
			// simple bounds check
			if ((byte *)(surf + 1) > (byte *)mdr + size)
			{
				ri.Printf(PRINT_WARNING, "R_LoadMDR: %s has broken structure.\n", mod_name.data());
				return false;
			}

			// first do some copying stuff

			surf->ident = static_cast<int>(surfaceType_t::SF_MDR);
			Q_strncpyz(surf->name, cursurf->name, sizeof(surf->name));
			Q_strncpyz(surf->shader, cursurf->shader, sizeof(surf->shader));

			surf->ofsHeader = (byte *)mdr - (byte *)surf;

			surf->numVerts = LittleLong(cursurf->numVerts);
			surf->numTriangles = LittleLong(cursurf->numTriangles);
			// numBoneReferences and BoneReferences generally seem to be unused

			// now do the checks that may fail.
			if (surf->numVerts >= SHADER_MAX_VERTEXES)
			{
				ri.Printf(PRINT_WARNING, "R_LoadMDR: %s has more than %i verts on %s (%i).\n",
						  mod_name.data(), SHADER_MAX_VERTEXES - 1, surf->name[0] ? surf->name : "a surface",
						  surf->numVerts);
				return false;
			}
			if (surf->numTriangles * 3 >= SHADER_MAX_INDEXES)
			{
				ri.Printf(PRINT_WARNING, "R_LoadMDR: %s has more than %i triangles on %s (%i).\n",
						  mod_name.data(), (SHADER_MAX_INDEXES / 3) - 1, surf->name[0] ? surf->name : "a surface",
						  surf->numTriangles);
				return false;
			}
			// lowercase the surface name so skin compares are faster
			q_strlwr_cpp(std::span(surf->name));

			// register the shaders
			sh = R_FindShader(surf->shader, LIGHTMAP_NONE, true);
			if (sh->defaultShader)
			{
				surf->shaderIndex = 0;
			}
			else
			{
				surf->shaderIndex = sh->index;
			}

			// now copy the vertexes.
			v = (mdrVertex_t *)(surf + 1);
			surf->ofsVerts = (int)((byte *)v - (byte *)surf);
			curv = (mdrVertex_t *)((byte *)cursurf + LittleLong(cursurf->ofsVerts));

			for (j = 0; j < surf->numVerts; j++)
			{
				LL(curv->numWeights);

				// simple bounds check
				if (curv->numWeights < 0 || (byte *)(v + 1) + (curv->numWeights - 1) * sizeof(*weight) > (byte *)mdr + size)
				{
					ri.Printf(PRINT_WARNING, "R_LoadMDR: %s has broken structure.\n", mod_name.data());
					return false;
				}

				v->normal[0] = LittleFloat(curv->normal[0]);
				v->normal[1] = LittleFloat(curv->normal[1]);
				v->normal[2] = LittleFloat(curv->normal[2]);

				v->texCoords[0] = LittleFloat(curv->texCoords[0]);
				v->texCoords[1] = LittleFloat(curv->texCoords[1]);

				v->numWeights = curv->numWeights;
				weight = &v->weights[0];
				curweight = &curv->weights[0];

				// Now copy all the weights
				for (k = 0; k < v->numWeights; k++)
				{
					weight->boneIndex = LittleLong(curweight->boneIndex);
					weight->boneWeight = LittleFloat(curweight->boneWeight);

					weight->offset[0] = LittleFloat(curweight->offset[0]);
					weight->offset[1] = LittleFloat(curweight->offset[1]);
					weight->offset[2] = LittleFloat(curweight->offset[2]);

					weight++;
					curweight++;
				}

				v = (mdrVertex_t *)weight;
				curv = (mdrVertex_t *)curweight;
			}

			// we know the offset to the triangles now:
			tri = (mdrTriangle_t *)v;
			surf->ofsTriangles = (int)((byte *)tri - (byte *)surf);
			curtri = (mdrTriangle_t *)((byte *)cursurf + LittleLong(cursurf->ofsTriangles));

			// simple bounds check
			if (surf->numTriangles < 0 || (byte *)(tri + surf->numTriangles) > (byte *)mdr + size)
			{
				ri.Printf(PRINT_WARNING, "R_LoadMDR: %s has broken structure.\n", mod_name.data());
				return false;
			}

			for (j = 0; j < surf->numTriangles; j++)
			{
				tri->indexes[0] = LittleLong(curtri->indexes[0]);
				tri->indexes[1] = LittleLong(curtri->indexes[1]);
				tri->indexes[2] = LittleLong(curtri->indexes[2]);

				tri++;
				curtri++;
			}

			// tri now points to the end of the surface.
			surf->ofsEnd = (byte *)tri - (byte *)surf;
			surf = (mdrSurface_t *)tri;

			// find the next surface.
			cursurf = (mdrSurface_t *)((byte *)cursurf + LittleLong(cursurf->ofsEnd));
		}

		// surf points to the next lod now.
		lod->ofsEnd = (int)((byte *)surf - (byte *)lod);
		lod = (mdrLOD_t *)surf;

		// find the next LOD.
		curlod = (mdrLOD_t *)((byte *)curlod + LittleLong(curlod->ofsEnd));
	}

	// lod points to the first tag now, so update the offset too.
	tag = (mdrTag_t *)lod;
	mdr->ofsTags = (int)((byte *)tag - (byte *)mdr);
	curtag = (mdrTag_t *)((byte *)pinmodel + LittleLong(pinmodel->ofsTags));

	// simple bounds check
	if (mdr->numTags < 0 || (byte *)(tag + mdr->numTags) > (byte *)mdr + size)
	{
		ri.Printf(PRINT_WARNING, "R_LoadMDR: %s has broken structure.\n", mod_name.data());
		return false;
	}

	for (i = 0; i < mdr->numTags; i++)
	{
		tag->boneIndex = LittleLong(curtag->boneIndex);
		Q_strncpyz(tag->name, curtag->name, sizeof(tag->name));

		tag++;
		curtag++;
	}

	// And finally we know the real offset to the end.
	mdr->ofsEnd = (int)((byte *)tag - (byte *)mdr);

	// phew! we're done.

	return true;
}

//=============================================================================

/*
** RE_BeginRegistration
*/
void RE_BeginRegistration(glconfig_t *glconfigOut)
{

	R_Init();

	*glconfigOut = glConfig;

	tr.viewCluster = -1; // force markleafs to regenerate

	R_ClearFlares();

	RE_ClearScene();

	tr.registered = true;
}

//=============================================================================

/*
===============
R_ModelInit
===============
*/
void R_ModelInit(void)
{
	model_t *mod;

	// leave a space for NULL model
	tr.numModels = 0;

	mod = R_AllocModel();
	mod->type = modtype_t::MOD_BAD;
}

/*
================
R_Modellist_f
================
*/
void R_Modellist_f(void)
{
	int i, j;
	int total;
	int lods;

	total = 0;
	for (i = 1; i < tr.numModels; i++)
	{
		model_t &mod = *tr.models[i];
		lods = 1;
		for (j = 1; j < MD3_MAX_LODS; j++)
		{
			if (mod.md3[j] && mod.md3[j] != mod.md3[j - 1])
			{
				lods++;
			}
		}
		ri.Printf(PRINT_ALL, "%8i : (%i) %s\n", mod.dataSize, lods, mod.name.data());
		total += mod.dataSize;
	}
	ri.Printf(PRINT_ALL, "%8i : Total models\n", total);

#if 0 // not working right with new hunk
	if ( tr.world ) {
		ri.Printf( PRINT_ALL, "\n%8i : %s\n", tr.world->dataSize, tr.world->name );
	}
#endif
}

//=============================================================================

/*
================
R_GetTag
================
*/
static md3Tag_t *R_GetTag(md3Header_t *mod, int frame, std::string_view tagName)
{
	md3Tag_t *tag;
	int i;

	if (frame >= mod->numFrames)
	{
		// it is possible to have a bad frame while changing models, so don't error
		frame = mod->numFrames - 1;
	}

	tag = (md3Tag_t *)((byte *)mod + mod->ofsTags) + frame * mod->numTags;
	for (i = 0; i < mod->numTags; i++, tag++)
	{
		if (!std::string_view(tag->name).compare(tagName))
		{
			return tag; // found it
		}
	}

	return NULL;
}

static md3Tag_t *R_GetAnimTag(mdrHeader_t *mod, int framenum, std::string_view tagName, md3Tag_t &dest)
{
	int i, j, k;
	int frameSize;
	mdrFrame_t *frame;
	mdrTag_t *tag;

	if (framenum >= mod->numFrames)
	{
		// it is possible to have a bad frame while changing models, so don't error
		framenum = mod->numFrames - 1;
	}

	tag = (mdrTag_t *)((byte *)mod + mod->ofsTags);
	for (i = 0; i < mod->numTags; i++, tag++)
	{
		if (!std::string_view(tag->name).compare(tagName))
		{
			Q_strncpyz(dest.name, tag->name, sizeof(dest.name));

			// uncompressed model...
			//
			frameSize = (intptr_t)(&((mdrFrame_t *)0)->bones[mod->numBones]);
			frame = (mdrFrame_t *)((byte *)mod + mod->ofsFrames + framenum * frameSize);

			for (j = 0; j < 3; j++)
			{
				for (k = 0; k < 3; k++)
					dest.axis[j][k] = frame->bones[tag->boneIndex].matrix[k][j];
			}

			dest.origin[0] = frame->bones[tag->boneIndex].matrix[0][3];
			dest.origin[1] = frame->bones[tag->boneIndex].matrix[1][3];
			dest.origin[2] = frame->bones[tag->boneIndex].matrix[2][3];

			return &dest;
		}
	}

	return NULL;
}

/*
================
R_LerpTag
================
*/
int R_LerpTag(orientation_t *tag, qhandle_t handle, int startFrame, int endFrame,
			  float frac, const char *tagName)
{
	md3Tag_t *start, *end;
	md3Tag_t start_space, end_space;
	int i;
	float frontLerp, backLerp;
	model_t *model;

	std::string_view tagNameCpp{tagName};

	model = R_GetModelByHandle(handle);
	if (!model->md3[0])
	{
		if (model->type == modtype_t::MOD_MDR)
		{
			start = R_GetAnimTag((mdrHeader_t *)model->modelData, startFrame, tagNameCpp, start_space);
			end = R_GetAnimTag((mdrHeader_t *)model->modelData, endFrame, tagNameCpp, end_space);
		}
		else if (model->type == modtype_t::MOD_IQM)
		{
			return R_IQMLerpTag(*tag, reinterpret_cast<iqmData_t &>(model->modelData),
								startFrame, endFrame,
								frac, tagName);
		}
		else
		{
			start = end = NULL;
		}
	}
	else
	{
		start = R_GetTag(model->md3[0], startFrame, tagNameCpp);
		end = R_GetTag(model->md3[0], endFrame, tagNameCpp);
	}

	if (!start || !end)
	{
		AxisClear(tag->axis);
		VectorClear(tag->origin);
		return false;
	}

	frontLerp = frac;
	backLerp = 1.0f - frac;

	for (i = 0; i < 3; i++)
	{
		tag->origin[i] = start->origin[i] * backLerp + end->origin[i] * frontLerp;
		tag->axis[0][i] = start->axis[0][i] * backLerp + end->axis[0][i] * frontLerp;
		tag->axis[1][i] = start->axis[1][i] * backLerp + end->axis[1][i] * frontLerp;
		tag->axis[2][i] = start->axis[2][i] * backLerp + end->axis[2][i] * frontLerp;
	}
	VectorNormalize(tag->axis[0]);
	VectorNormalize(tag->axis[1]);
	VectorNormalize(tag->axis[2]);
	return true;
}

/*
====================
R_ModelBounds
====================
*/
void R_ModelBounds(qhandle_t handle, vec3_t mins, vec3_t maxs)
{
	model_t &model = *R_GetModelByHandle(handle);

	if (model.type == modtype_t::MOD_BRUSH)
	{
		VectorCopy(model.bmodel->bounds[0], mins);
		VectorCopy(model.bmodel->bounds[1], maxs);

		return;
	}
	else if (model.type == modtype_t::MOD_MESH)
	{
		md3Header_t *header;
		md3Frame_t *frame;

		header = model.md3[0];
		frame = (md3Frame_t *)((byte *)header + header->ofsFrames);

		VectorCopy(frame->bounds[0], mins);
		VectorCopy(frame->bounds[1], maxs);

		return;
	}
	else if (model.type == modtype_t::MOD_MDR)
	{
		mdrHeader_t *header;
		mdrFrame_t *frame;

		header = (mdrHeader_t *)model.modelData;
		frame = (mdrFrame_t *)((byte *)header + header->ofsFrames);

		VectorCopy(frame->bounds[0], mins);
		VectorCopy(frame->bounds[1], maxs);

		return;
	}
	else if (model.type == modtype_t::MOD_IQM)
	{
		iqmData_t *iqmData;

		iqmData = reinterpret_cast<iqmData_t *>(model.modelData);

		if (iqmData->bounds)
		{
			VectorCopy(iqmData->bounds, mins);
			VectorCopy(iqmData->bounds + 3, maxs);
			return;
		}
	}

	VectorClear(mins);
	VectorClear(maxs);
}