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
// tr_flares.c

#include "vk_flares.hpp"
#include "tr_shade_calc.hpp"
#include "tr_shade.hpp"
#include "tr_surface.hpp"
#include "vk.hpp"
#include "tr_main.hpp"
#include "math.hpp"

/*
=============================================================================

LIGHT FLARES

A light flare is an effect that takes place inside the eye when bright light
sources are visible.  The size of the flare relative to the screen is nearly
constant, irrespective of distance, but the intensity should be proportional to the
projected area of the light source.

A surface that has been flagged as having a light flare will calculate the depth
buffer value that its midpoint should have when the surface is added.

After all opaque surfaces have been rendered, the depth buffer is read back for
each flare in view.  If the point has not been obscured by a closer surface, the
flare should be drawn.

Surfaces that have a repeated texture should never be flagged as flaring, because
there will only be a single flare added at the midpoint of the polygon.

To prevent abrupt popping, the intensity of the flare is interpolated up and
down as it changes visibility.  This involves scene to scene state, unlike almost
all other aspects of the renderer, and is complicated by the fact that a single
frame may have multiple scenes.

RB_RenderFlares() will be called once per view (twice in a mirrored scene, potentially
up to five or more times in a frame with 3D status bar icons).

=============================================================================
*/

// flare states maintain visibility over multiple frames for fading
// layers: view, mirror, menu
typedef struct flare_s
{
	struct flare_s *next; // for active chain

	int addedFrame;
	uint32_t testCount;

	portalView_t portalView;
	int frameSceneNum;
	void *surface;
	int fogNum;

	int fadeTime;

	bool visible;		 // state of last test
	float drawIntensity; // may be non 0 even if !visible due to fading

	int windowX, windowY;
	float eyeZ;
	float drawZ;

	vec3_t origin;
	vec3_t color;
} flare_t;

static flare_t r_flareStructs[MAX_FLARES];
static flare_t *r_activeFlares, *r_inactiveFlares;

/*
==================
R_ClearFlares
==================
*/
void R_ClearFlares(void)
{
	if (!vk_inst.fragmentStores)
		return;

	Com_Memset(r_flareStructs, 0, sizeof(r_flareStructs));
	r_activeFlares = nullptr;
	r_inactiveFlares = nullptr;

	for (int i = 0; i < MAX_FLARES; i++)
	{
		r_flareStructs[i].next = r_inactiveFlares;
		r_inactiveFlares = &r_flareStructs[i];
	}
}

static flare_t *R_SearchFlare(void *surface)
{
	flare_t *flare;

	// see if a flare with a matching surface, scene, and view exists
	for (flare = r_activeFlares; flare; flare = flare->next)
	{
		if (flare->surface == surface && flare->frameSceneNum == backEnd.viewParms.frameSceneNum && flare->portalView == backEnd.viewParms.portalView)
		{
			return flare;
		}
	}

	return nullptr;
}

/*
==================
RB_AddFlare

This is called at surface tesselation time
==================
*/
void RB_AddFlare(void *surface, const int fogNum, const vec3_t &point, const vec3_t &color, const vec3_t &normal)
{
	int i;
	flare_t *flare;
	vec3_t local{};
	float d = 1;
	vec4_t eye, clip, normalized, window;

	backEnd.pc.c_flareAdds++;

	if (normal[0] || normal[1] || normal[2])
	{
		VectorSubtract(backEnd.viewParms.ort.origin, point, local);
		VectorNormalizeFast(local);
		d = DotProduct(local, normal);
		// If the viewer is behind the flare don't add it.
		if (d < 0)
		{
			return;
		}
	}

	// if the point is off the screen, don't bother adding it
	// calculate screen coordinates and depth
	R_TransformModelToClip(point, backEnd.ort.modelMatrix, backEnd.viewParms.projectionMatrix, eye, clip);

	// check to see if the point is completely off screen
	for (i = 0; i < 3; i++)
	{
		if (clip[i] >= clip[3] || clip[i] <= -clip[3])
		{
			return;
		}
	}

	R_TransformClipToWindow(clip, backEnd.viewParms, normalized, window);

	if (window[0] < 0 || window[0] >= backEnd.viewParms.viewportWidth || window[1] < 0 || window[1] >= backEnd.viewParms.viewportHeight)
	{
		return; // shouldn't happen, since we check the clip[] above, except for FP rounding
	}

	flare = R_SearchFlare(surface);

	// allocate a new one
	if (!flare)
	{
		if (!r_inactiveFlares)
		{
			// the list is completely full
			return;
		}
		flare = r_inactiveFlares;
		r_inactiveFlares = r_inactiveFlares->next;
		flare->next = r_activeFlares;
		r_activeFlares = flare;

		flare->surface = surface;
		flare->frameSceneNum = backEnd.viewParms.frameSceneNum;
		flare->portalView = backEnd.viewParms.portalView;
		flare->visible = false;
		flare->fadeTime = backEnd.refdef.time - 2000;
		flare->testCount = 0;
	}
	else
	{
		++flare->testCount;
	}

	flare->addedFrame = backEnd.viewParms.frameCount;
	flare->fogNum = fogNum;

	VectorCopy(point, flare->origin);
	VectorCopy(color, flare->color);

	// fade the intensity of the flare down as the
	// light surface turns away from the viewer
	VectorScale(flare->color, d, flare->color);

	// save info needed to test
	flare->windowX = backEnd.viewParms.viewportX + window[0];
	flare->windowY = backEnd.viewParms.viewportY + window[1];

	flare->eyeZ = eye[2];

#ifdef USE_REVERSED_DEPTH
	flare->drawZ = (clip[2] + 0.20) / clip[3];
#else
	flare->drawZ = (clip[2] - 0.20) / clip[3];
#endif
}

/*
==================
RB_AddDlightFlares
==================
*/
void RB_AddDlightFlares(void)
{
	if (!r_flares->integer)
	{
		return;
	}

	dlight_t *l;
	int j, k;
	fog_t *fog = nullptr;

	l = backEnd.refdef.dlights;

	if (tr.world)
		fog = tr.world->fogs;

	for (uint32_t i = 0; i < backEnd.refdef.num_dlights; i++, l++)
	{

		if (fog)
		{
			// find which fog volume the light is in
			for (j = 1; j < tr.world->numfogs; j++)
			{
				fog = &tr.world->fogs[j];
				for (k = 0; k < 3; k++)
				{
					if (l->origin[k] < fog->bounds[0][k] || l->origin[k] > fog->bounds[1][k])
					{
						break;
					}
				}
				if (k == 3)
				{
					break;
				}
			}
			if (j == tr.world->numfogs)
			{
				j = 0;
			}
		}
		else
			j = 0;

		RB_AddFlare((void *)l, j, l->origin, l->color, {0, 0, 0});
	}
}

/*
===============================================================================

FLARE BACK END

===============================================================================
*/

static float *vk_ortho(float x1, float x2,
					   float y2, float y1,
					   float z1, float z2)
{

	static float m[16] = {0};

	m[0] = 2.0f / (x2 - x1);
	m[5] = 2.0f / (y2 - y1);
	m[10] = 1.0f / (z1 - z2);
	m[12] = -(x2 + x1) / (x2 - x1);
	m[13] = -(y2 + y1) / (y2 - y1);
	m[14] = z1 / (z1 - z2);
	m[15] = 1.0f;

	return m;
}

/*
==================
RB_TestFlare
==================
*/
static void RB_TestFlare(flare_t *f)
{
	bool visible;
	float fade;
	float *m;
	uint32_t offset;

	backEnd.pc.c_flareTests++;

	/*
		We don't have equivalent of glReadPixels() in vulkan
		and explicit depth buffer reading may be very slow and require surface conversion.

		So we will use storage buffer and exploit early depth tests by
		rendering test dot in orthographic projection at projected flare coordinates
		window-x, window-y and world-z: if test dot is not covered by
		any world geometry - it will invoke fragment shader which will
		fill storage buffer at desired location, then we discard fragment.
		In next frame we read storage buffer: if there is a non-zero value
		then our flare WAS visible (as we're working with 1-frame delay),
		multisampled image will cause multiple fragment shader invocations.
	*/

	// we neeed only single uint32_t but take care of alignment
	offset = (f - r_flareStructs) * vk_inst.storage_alignment;

	if (f->testCount)
	{
		uint32_t *cnt = (uint32_t *)(vk_inst.storage.buffer_ptr + offset);
		if (*cnt)
			visible = true;
		else
			visible = false;

		f->testCount = 1;
	}
	else
	{
		visible = false;
	}

	// reset test result in storage buffer
	// *((uint32_t*)(vk_inst.storage.buffer_ptr + offset)) = 0x00;

	m = vk_ortho(backEnd.viewParms.viewportX, backEnd.viewParms.viewportX + backEnd.viewParms.viewportWidth,
				 backEnd.viewParms.viewportY, backEnd.viewParms.viewportY + backEnd.viewParms.viewportHeight, 0, 1);
	vk_update_mvp(m);

	tess.xyz[0][0] = f->windowX;
	tess.xyz[0][1] = f->windowY;
	tess.xyz[0][2] = -f->drawZ;
	tess.numVertexes = 1;

#ifdef USE_VBO
	tess.vboIndex = 0;
#endif
	// render test dot
	vk_reset_descriptor(VK_DESC_STORAGE);
	vk_update_descriptor(VK_DESC_STORAGE, vk_inst.storage.descriptor);
	vk_update_descriptor_offset(VK_DESC_STORAGE, offset);

	vk_bind_pipeline(vk_inst.dot_pipeline);
	vk_bind_geometry(TESS_XYZ);
	vk_draw_geometry(Vk_Depth_Range::DEPTH_RANGE_NORMAL, false);

	// Com_Memcpy( vk_world.modelview_transform, modelMatrix_original, sizeof( modelMatrix_original ) );
	// vk_update_mvp( NULL );

	if (visible)
	{
		if (!f->visible)
		{
			f->visible = true;
			f->fadeTime = backEnd.refdef.time - 1;
		}
		fade = ((backEnd.refdef.time - f->fadeTime) / 1000.0f) * r_flareFade->value;
	}
	else
	{
		if (f->visible)
		{
			f->visible = false;
			f->fadeTime = backEnd.refdef.time - 1;
		}
		fade = 1.0f - ((backEnd.refdef.time - f->fadeTime) / 1000.0f) * r_flareFade->value;
	}

	if (fade < 0)
	{
		fade = 0;
	}
	else if (fade > 1)
	{
		fade = 1;
	}

	f->drawIntensity = fade;
}

/*
==================
RB_RenderFlare
==================
*/
static void RB_RenderFlare(flare_t &f)
{
	float size;
	vec3_t color{};
	float distance, intensity, factor;
	byte fogFactors[3] = {255, 255, 255};
	color4ub_t c{};

	// if ( f->drawIntensity == 0.0 )
	//	return;

	backEnd.pc.c_flareRenders++;

	// We don't want too big values anyways when dividing by distance.
	if (f.eyeZ > -1.0f)
		distance = 1.0f;
	else
		distance = -f.eyeZ;

	// calculate the flare size..
	size = backEnd.viewParms.viewportWidth * (r_flareSize->value / 640.0f + 8 / distance);

	/*
	 * This is an alternative to intensity scaling. It changes the size of the flare on screen instead
	 * with growing distance. See in the description at the top why this is not the way to go.
		// size will change ~ 1/r.
		size = backEnd.viewParms.viewportWidth * (r_flareSize->value / (distance * -2.0f));
	*/

	/*
	 * As flare sizes stay nearly constant with increasing distance we must decrease the intensity
	 * to achieve a reasonable visual result. The intensity is ~ (size^2 / distance^2) which can be
	 * got by considering the ratio of
	 * (flaresurface on screen) : (Surface of sphere defined by flare origin and distance from flare)
	 * An important requirement is:
	 * intensity <= 1 for all distances.
	 *
	 * The formula used here to compute the intensity is as follows:
	 * intensity = flareCoeff * size^2 / (distance + size*sqrt(flareCoeff))^2
	 * As you can see, the intensity will have a max. of 1 when the distance is 0.
	 * The coefficient flareCoeff will determine the falloff speed with increasing distance.
	 */

	factor = distance + size * sqrt(r_flareCoeff->value);

	intensity = r_flareCoeff->value * size * size / (factor * factor);

	VectorScale(f.color, f.drawIntensity * intensity, color);

	// Calculations for fogging
	if (tr.world && f.fogNum > 0 && f.fogNum < tr.world->numfogs)
	{
		tess.numVertexes = 1;
		VectorCopy(f.origin, tess.xyz[0]);
		tess.fogNum = f.fogNum;

		RB_CalcModulateColorsByFog(fogFactors);

		// We don't need to render the flare if colors are 0 anyways.
		if (!(fogFactors[0] || fogFactors[1] || fogFactors[2]))
			return;
	}

	RB_BeginSurface(*tr.flareShader, f.fogNum);

	c.rgba[0] = color[0] * fogFactors[0];
	c.rgba[1] = color[1] * fogFactors[1];
	c.rgba[2] = color[2] * fogFactors[2];
	c.rgba[3] = 255;

	RB_AddQuadStamp2(f.windowX - size, f.windowY - size, size * 2, size * 2, 0, 0, 1, 1, c);

	RB_EndSurface();
}

/*
==================
RB_RenderFlares

Because flares are simulating an occular effect, they should be drawn after
everything (all views) in the entire frame has been drawn.

Because of the way portals use the depth buffer to mark off areas, the
needed information would be lost after each view, so we are forced to draw
flares after each view.

The resulting artifact is that flares in mirrors or portals don't dim properly
when occluded by something in the main view, and portal flares that should
extend past the portal edge will be overwritten.
==================
*/
void RB_RenderFlares(void)
{
	if (!r_flares->integer)
	{
		return;
	}

	if (vk_inst.renderPassIndex == renderPass_t::RENDER_PASS_SCREENMAP)
	{
		return;
	}

	if (backEnd.isHyperspace)
	{
		return;
	}

	flare_t *f;
	flare_t **prev;
	bool draw;
	float *m;

	// Reset currentEntity to world so that any previously referenced entities
	// don't have influence on the rendering of these flares (i.e. RF_ renderer flags).
	backEnd.currentEntity = &tr.worldEntity;
	backEnd.ort = backEnd.viewParms.world;

	// RB_AddDlightFlares();

	// perform z buffer readback on each flare in this view
	draw = false;
	prev = &r_activeFlares;
	while ((f = *prev) != NULL)
	{
		// throw out any flares that weren't added last frame
		if (backEnd.viewParms.frameCount - f->addedFrame > 0 && f->portalView == backEnd.viewParms.portalView)
		{
			*prev = f->next;
			f->next = r_inactiveFlares;
			r_inactiveFlares = f;
			continue;
		}

		// don't draw any here that aren't from this scene / portal
		f->drawIntensity = 0;
		if (f->frameSceneNum == backEnd.viewParms.frameSceneNum && f->portalView == backEnd.viewParms.portalView)
		{
			RB_TestFlare(f);
			if (f->testCount == 0)
			{
				// recently added, wait 1 frame for test result
			}
			else if (f->drawIntensity)
			{
				draw = true;
			}
			else
			{
				// this flare has completely faded out, so remove it from the chain
				*prev = f->next;
				f->next = r_inactiveFlares;
				r_inactiveFlares = f;
				continue;
			}
		}

		prev = &f->next;
	}

	if (!draw)
	{
		return; // none visible
	}

#ifdef USE_REVERSED_DEPTH
	m = vk_ortho(backEnd.viewParms.viewportX, backEnd.viewParms.viewportX + backEnd.viewParms.viewportWidth,
				 backEnd.viewParms.viewportY, backEnd.viewParms.viewportY + backEnd.viewParms.viewportHeight, 1.0, 0.0);
#else
	m = vk_ortho(backEnd.viewParms.viewportX, backEnd.viewParms.viewportX + backEnd.viewParms.viewportWidth,
				 backEnd.viewParms.viewportY, backEnd.viewParms.viewportY + backEnd.viewParms.viewportHeight, 0.0, 1.0);
#endif

	vk_update_mvp(m);

	for (f = r_activeFlares; f; f = f->next)
	{
		if (f->frameSceneNum == backEnd.viewParms.frameSceneNum && f->portalView == backEnd.viewParms.portalView && f->drawIntensity)
		{
			RB_RenderFlare(*f);
		}
	}

	// Com_Memcpy( vk_world.modelview_transform, modelMatrix_original, sizeof( modelMatrix_original ) );
	// vk_update_mvp( NULL );
}