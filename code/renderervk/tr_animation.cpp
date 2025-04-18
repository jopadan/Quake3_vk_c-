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

#include "tr_animation.hpp"
#include "tr_shader.hpp"
#include "tr_image.hpp"
#include "tr_mesh.hpp"
#include "tr_surface.hpp"
#include "vk_vbo.hpp"
#include "tr_light.hpp"
#include "tr_main.hpp"

/*

All bones should be an identity orientation to display the mesh exactly
as it is specified.

For all other frames, the bones represent the transformation from the
orientation of the bone in the base frame to the orientation in this
frame.

*/

// copied and adapted from tr_mesh.c

/*
=============
R_MDRCullModel
=============
*/
static int R_MDRCullModel(mdrHeader_t &header, const trRefEntity_t &ent)
{
	vec3_t bounds[2]{};
	mdrFrame_t *oldFrame, *newFrame;
	int i, frameSize;

	frameSize = (size_t)(&((mdrFrame_t *)0)->bones[header.numBones]);

	// compute frame pointers
	newFrame = reinterpret_cast<mdrFrame_t*>(
		reinterpret_cast<uintptr_t>(&header) + header.ofsFrames + frameSize * ent.e.frame
		);
	oldFrame = reinterpret_cast<mdrFrame_t*>(
		reinterpret_cast<uintptr_t>(&header) + header.ofsFrames + frameSize * ent.e.oldframe
		);

	// cull bounding sphere ONLY if this is not an upscaled entity
	if (!ent.e.nonNormalizedAxes)
	{
		if (ent.e.frame == ent.e.oldframe)
		{
			switch (R_CullLocalPointAndRadius(newFrame->localOrigin, newFrame->radius))
			{
				// Ummm... yeah yeah I know we don't really have an md3 here.. but we pretend
				// we do. After all, the purpose of mdrs are not that different, are they?

			case CULL_OUT:
				tr.pc.c_sphere_cull_md3_out++;
				return CULL_OUT;

			case CULL_IN:
				tr.pc.c_sphere_cull_md3_in++;
				return CULL_IN;

			case CULL_CLIP:
				tr.pc.c_sphere_cull_md3_clip++;
				break;
			}
		}
		else
		{
			int sphereCull, sphereCullB;

			sphereCull = R_CullLocalPointAndRadius(newFrame->localOrigin, newFrame->radius);
			if (newFrame == oldFrame)
			{
				sphereCullB = sphereCull;
			}
			else
			{
				sphereCullB = R_CullLocalPointAndRadius(oldFrame->localOrigin, oldFrame->radius);
			}

			if (sphereCull == sphereCullB)
			{
				if (sphereCull == CULL_OUT)
				{
					tr.pc.c_sphere_cull_md3_out++;
					return CULL_OUT;
				}
				else if (sphereCull == CULL_IN)
				{
					tr.pc.c_sphere_cull_md3_in++;
					return CULL_IN;
				}
				else
				{
					tr.pc.c_sphere_cull_md3_clip++;
				}
			}
		}
	}

	// calculate a bounding box in the current coordinate system
	for (i = 0; i < 3; i++)
	{
		bounds[0][i] = oldFrame->bounds[0][i] < newFrame->bounds[0][i] ? oldFrame->bounds[0][i] : newFrame->bounds[0][i];
		bounds[1][i] = oldFrame->bounds[1][i] > newFrame->bounds[1][i] ? oldFrame->bounds[1][i] : newFrame->bounds[1][i];
	}

	switch (R_CullLocalBox(bounds))
	{
	case CULL_IN:
		tr.pc.c_box_cull_md3_in++;
		return CULL_IN;
	case CULL_CLIP:
		tr.pc.c_box_cull_md3_clip++;
		return CULL_CLIP;
	case CULL_OUT:
	default:
		tr.pc.c_box_cull_md3_out++;
		return CULL_OUT;
	}
}

/*
=================
R_MDRComputeFogNum
=================
*/
static int R_MDRComputeFogNum(mdrHeader_t &header, const trRefEntity_t &ent)
{
	if (tr.refdef.rdflags & RDF_NOWORLDMODEL)
	{
		return 0;
	}

	int i, j;
	mdrFrame_t *mdrFrame;
	vec3_t localOrigin{};

	int frameSize = (size_t)(&((mdrFrame_t *)0)->bones[header.numBones]);

	// FIXME: non-normalized axis issues
	mdrFrame = reinterpret_cast<mdrFrame_t*>(
		reinterpret_cast<uintptr_t>(&header) + header.ofsFrames + frameSize * ent.e.frame
	);

	VectorAdd(ent.e.origin, mdrFrame->localOrigin, localOrigin);
	for (i = 1; i < tr.world->numfogs; i++)
	{
		const fog_t& fog = tr.world->fogs[i];
		for (j = 0; j < 3; j++)
		{
			if (localOrigin[j] - mdrFrame->radius >= fog.bounds[1][j])
			{
				break;
			}
			if (localOrigin[j] + mdrFrame->radius <= fog.bounds[0][j])
			{
				break;
			}
		}
		if (j == 3)
		{
			return i;
		}
	}

	return 0;
}

/*
==============
R_MDRAddAnimSurfaces
==============
*/

// much stuff in there is just copied from R_AddMd3Surfaces in tr_mesh.c

void R_MDRAddAnimSurfaces(trRefEntity_t &ent)
{
	mdrSurface_t *surface = nullptr;
	mdrLOD_t *lod = nullptr;
	shader_t *shader = nullptr;
	skin_t *skin = nullptr;
	int i, j;
	int lodnum = 0;
	int fogNum = 0;
	int cull;
	bool personalModel;

	mdrHeader_t &header = (mdrHeader_t &)tr.currentModel->modelData;

	personalModel = (ent.e.renderfx & RF_THIRD_PERSON) && (tr.viewParms.portalView == portalView_t::PV_NONE);

	if (ent.e.renderfx & RF_WRAP_FRAMES)
	{
		ent.e.frame %= header.ident;
		ent.e.oldframe %= header.numFrames;
	}

	//
	// Validate the frames so there is no chance of a crash.
	// This will write directly into the entity structure, so
	// when the surfaces are rendered, they don't need to be
	// range checked again.
	//
	if ((ent.e.frame >= header.numFrames) || (ent.e.frame < 0) || (ent.e.oldframe >= header.numFrames) || (ent.e.oldframe < 0))
	{
		ri.Printf(PRINT_DEVELOPER, "R_MDRAddAnimSurfaces: no such frame %d to %d for '%s'\n",
				  ent.e.oldframe, ent.e.frame, tr.currentModel->name.data());
		ent.e.frame = 0;
		ent.e.oldframe = 0;
	}

	//
	// cull the entire model if merged bounding box of both frames
	// is outside the view frustum.
	//
	cull = R_MDRCullModel(header, ent);
	if (cull == CULL_OUT)
	{
		return;
	}

	// figure out the current LOD of the model we're rendering, and set the lod pointer respectively.
	lodnum = R_ComputeLOD(ent);
	// check whether this model has as that many LODs at all. If not, try the closest thing we got.
	if (header.numLODs <= 0)
		return;
	if (header.numLODs <= lodnum)
		lodnum = header.numLODs - 1;

	//lod = (mdrLOD_t *)((byte &)header + header.ofsLODs);
	lod = reinterpret_cast<mdrLOD_t*>(reinterpret_cast<uintptr_t>(&header) + header.ofsLODs);
	for (i = 0; i < lodnum; i++)
	{
		lod = (mdrLOD_t *)((byte *)lod + lod->ofsEnd);
	}

	// set up lighting
	if (!personalModel || r_shadows->integer > 1)
	{
		R_SetupEntityLighting(tr.refdef, ent);
	}

	// fogNum?
	fogNum = R_MDRComputeFogNum(header, ent);

	surface = (mdrSurface_t *)((byte *)lod + lod->ofsSurfaces);

	for (i = 0; i < lod->numSurfaces; i++)
	{
		if (ent.e.customShader)
			shader = R_GetShaderByHandle(ent.e.customShader);
		else if (ent.e.customSkin > 0 && ent.e.customSkin < tr.numSkins)
		{
			skin = R_GetSkinByHandle(ent.e.customSkin);
			shader = tr.defaultShader;

			for (j = 0; j < skin->numSurfaces; j++)
			{
				if (!strcmp(skin->surfaces[j].name, surface->name))
				{
					shader = skin->surfaces[j].shader;
					break;
				}
			}
		}
		else if (surface->shaderIndex > 0)
			shader = R_GetShaderByHandle(surface->shaderIndex);
		else
			shader = tr.defaultShader;

		// we will add shadows even if the main object isn't visible in the view

		// stencil shadows can't do personal models unless I polyhedron clip
		if (!personalModel && r_shadows->integer == 2 && fogNum == 0 && !(ent.e.renderfx & (RF_NOSHADOW | RF_DEPTHHACK)) && shader->sort == static_cast<float>(shaderSort_t::SS_OPAQUE))
		{
			R_AddDrawSurf(reinterpret_cast<surfaceType_t &>(*surface), *tr.shadowShader, 0, 0);
		}

		// projection shadows work fine with personal models
		if (r_shadows->integer == 3 && fogNum == 0 && (ent.e.renderfx & RF_SHADOW_PLANE) && shader->sort == static_cast<float>(shaderSort_t::SS_OPAQUE))
		{
			R_AddDrawSurf(reinterpret_cast<surfaceType_t &>(*surface), *tr.projectionShadowShader, 0, 0);
		}

		if (!personalModel)
		{
			R_AddDrawSurf(reinterpret_cast<surfaceType_t &>(*surface), *shader, fogNum, 0);
			tr.needScreenMap |= shader->hasScreenMap;
		}

		surface = (mdrSurface_t *)((byte *)surface + surface->ofsEnd);
	}
}

/*
==============
RB_MDRSurfaceAnim
==============
*/
void RB_MDRSurfaceAnim(mdrSurface_t &surface)
{
	int i, j, k;
	float frontlerp, backlerp;
	int *triangles;
	int indexes;
	int baseIndex, baseVertex;
	int numVerts;
	mdrVertex_t *v;
	mdrHeader_t *header;
	mdrFrame_t *frame;
	mdrFrame_t *oldFrame;
	mdrBone_t bones[MDR_MAX_BONES]{}, * bonePtr, * bone;

	int frameSize;

#ifdef USE_VBO
	VBO_Flush();

	tess.surfType = surfaceType_t::SF_MDR;
#endif

	// don't lerp if lerping off, or this is the only frame, or the last frame...
	//
	if (backEnd.currentEntity->e.oldframe == backEnd.currentEntity->e.frame)
	{
		backlerp = 0; // if backlerp is 0, lerping is off and frontlerp is never used
		frontlerp = 1;
	}
	else
	{
		backlerp = backEnd.currentEntity->e.backlerp;
		frontlerp = 1.0f - backlerp;
	}

	//header = (mdrHeader_t *)((byte &)surface + surface.ofsHeader);
	header = reinterpret_cast<mdrHeader_t*>(reinterpret_cast<uintptr_t>(&surface) + surface.ofsHeader);

	frameSize = (size_t)(&((mdrFrame_t *)0)->bones[header->numBones]);

	frame = (mdrFrame_t *)((byte *)header + header->ofsFrames +
						   backEnd.currentEntity->e.frame * frameSize);
	oldFrame = (mdrFrame_t *)((byte *)header + header->ofsFrames +
							  backEnd.currentEntity->e.oldframe * frameSize);

	RB_CHECKOVERFLOW(surface.numVerts, surface.numTriangles * 3);

	//triangles = (int *)((byte &)surface + surface.ofsTriangles);
	triangles = reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(&surface) + surface.ofsTriangles);
	indexes = surface.numTriangles * 3;
	baseIndex = tess.numIndexes;
	baseVertex = tess.numVertexes;

	// Set up all triangles.
	for (j = 0; j < indexes; j++)
	{
		tess.indexes[baseIndex + j] = baseVertex + triangles[j];
	}
	tess.numIndexes += indexes;

	//
	// lerp all the needed bones
	//
	if (!backlerp)
	{
		// no lerping needed
		bonePtr = frame->bones;
	}
	else
	{
		bonePtr = bones;

		for (i = 0; i < header->numBones * 12; i++)
		{
			((float *)bonePtr)[i] = frontlerp * ((float *)frame->bones)[i] + backlerp * ((float *)oldFrame->bones)[i];
		}
	}

	//
	// deform the vertexes by the lerped bones
	//
	numVerts = surface.numVerts;
	//v = (mdrVertex_t *)((byte &)surface + surface.ofsVerts);
	v = reinterpret_cast<mdrVertex_t*>(reinterpret_cast<uintptr_t>(&surface) + surface.ofsVerts);
	for (j = 0; j < numVerts; j++)
	{
		vec3_t tempVert{}, tempNormal{};
		mdrWeight_t *w;

		w = v->weights;
		for (k = 0; k < v->numWeights; k++, w++)
		{
			bone = bonePtr + w->boneIndex;

			tempVert[0] += w->boneWeight * (DotProduct(bone->matrix[0], w->offset) + bone->matrix[0][3]);
			tempVert[1] += w->boneWeight * (DotProduct(bone->matrix[1], w->offset) + bone->matrix[1][3]);
			tempVert[2] += w->boneWeight * (DotProduct(bone->matrix[2], w->offset) + bone->matrix[2][3]);

#ifdef USE_TESS_NEEDS_NORMAL
			if (tess.needsNormal)
#endif
			{
				tempNormal[0] += w->boneWeight * DotProduct(bone->matrix[0], v->normal);
				tempNormal[1] += w->boneWeight * DotProduct(bone->matrix[1], v->normal);
				tempNormal[2] += w->boneWeight * DotProduct(bone->matrix[2], v->normal);
			}
		}

		tess.xyz[baseVertex + j][0] = tempVert[0];
		tess.xyz[baseVertex + j][1] = tempVert[1];
		tess.xyz[baseVertex + j][2] = tempVert[2];

#ifdef USE_TESS_NEEDS_NORMAL
		if (tess.needsNormal)
#endif
		{
			tess.normal[baseVertex + j][0] = tempNormal[0];
			tess.normal[baseVertex + j][1] = tempNormal[1];
			tess.normal[baseVertex + j][2] = tempNormal[2];
		}

		tess.texCoords[0][baseVertex + j][0] = v->texCoords[0];
		tess.texCoords[0][baseVertex + j][1] = v->texCoords[1];

		v = (mdrVertex_t *)&v->weights[v->numWeights];
	}

	tess.numVertexes += surface.numVerts;
}

constexpr float MC_SCALE_VECT = (1.0f / 64);

constexpr int MC_MASK_X = (1 << MC_BITS_X) - 1;
constexpr int MC_MASK_Y = (1 << MC_BITS_Y) - 1;
constexpr int MC_MASK_Z = (1 << MC_BITS_Z) - 1;
constexpr int MC_MASK_VECT = (1 << MC_BITS_VECT) - 1;

void MC_UnCompress(float mat[3][4], const unsigned char *comp)
{
	mat[0][3] = (static_cast<float>(reinterpret_cast<const short *>(comp)[0] - (1 << (MC_BITS_X - 1)))) * MC_SCALE_X;
	mat[1][3] = (static_cast<float>(reinterpret_cast<const short *>(comp)[1] - (1 << (MC_BITS_Y - 1)))) * MC_SCALE_Y;
	mat[2][3] = (static_cast<float>(reinterpret_cast<const short *>(comp)[2] - (1 << (MC_BITS_Z - 1)))) * MC_SCALE_Z;

	for (int i = 0; i < 3; ++i)
	{
		mat[i][0] = (static_cast<float>(reinterpret_cast<const short *>(comp)[i * 3 + 3] - (1 << (MC_BITS_VECT - 1)))) * MC_SCALE_VECT;
		mat[i][1] = (static_cast<float>(reinterpret_cast<const short *>(comp)[i * 3 + 4] - (1 << (MC_BITS_VECT - 1)))) * MC_SCALE_VECT;
		mat[i][2] = (static_cast<float>(reinterpret_cast<const short *>(comp)[i * 3 + 5] - (1 << (MC_BITS_VECT - 1)))) * MC_SCALE_VECT;
	}
}