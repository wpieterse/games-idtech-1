/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

//r_alias.c -- alias model rendering

#include "quakedef.h"

extern cvar_t gl_overbright_models, gl_fullbrights, r_lerpmodels, r_lerpmove; //johnfitz
extern cvar_t scr_fov, cl_gun_fovscale;
extern cvar_t r_oit;

//up to 16 color translated skins
gltexture_t *playertextures[MAX_SCOREBOARD]; //johnfitz -- changed to an array of pointers

const float	r_avertexnormals[NUMVERTEXNORMALS][3] = {
#include "anorms.h"
};

extern vec3_t	lightcolor; //johnfitz -- replaces "float shadelight" for lit support

static float	entalpha; //johnfitz

//johnfitz -- struct for passing lerp information to drawing functions
typedef struct {
	short pose1;
	short pose2;
	float blend;
	vec3_t origin;
	vec3_t angles;
} lerpdata_t;
//johnfitz

#define MAX_ALIAS_INSTANCES 256

typedef struct aliasinstance_s {
	float		worldmatrix[12];
	vec3_t		lightcolor;
	float		alpha;
	int32_t		pose1;
	int32_t		pose2;
	float		blend;
	int32_t		padding;
} aliasinstance_t;

struct ibuf_s {
	int			count;
	entity_t	*ent;
	gltexture_t	*textures[2];

	struct {
		float	matviewproj[16];
		vec3_t	eyepos;
		float	_pad;
		vec4_t	fog;
	} global;
	aliasinstance_t inst[MAX_ALIAS_INSTANCES];
} ibuf;

/*
=================
R_SetupAliasFrame -- johnfitz -- rewritten to support lerping
=================
*/
void R_SetupAliasFrame (entity_t *e, aliashdr_t *paliashdr, lerpdata_t *lerpdata)
{
	int posenum, numposes;
	int frame = e->frame;

	if ((frame >= paliashdr->numframes) || (frame < 0))
	{
		Con_DPrintf ("R_AliasSetupFrame: no such frame %d for '%s'\n", frame, e->model->name);
		frame = 0;
	}

	posenum = paliashdr->frames[frame].firstpose;
	numposes = paliashdr->frames[frame].numposes;

	if (numposes > 1)
	{
		e->lerptime = paliashdr->frames[frame].interval;
		posenum += (int)(cl.time / e->lerptime) % numposes;
	}
	else
		e->lerptime = 0.1;

	if (e->lerpflags & LERP_RESETANIM) //kill any lerp in progress
	{
		e->lerpstart = 0;
		e->previouspose = posenum;
		e->currentpose = posenum;
		e->lerpflags -= LERP_RESETANIM;
	}
	else if (e->currentpose != posenum) // pose changed, start new lerp
	{
		if (e->lerpflags & LERP_RESETANIM2) //defer lerping one more time
		{
			e->lerpstart = 0;
			e->previouspose = posenum;
			e->currentpose = posenum;
			e->lerpflags -= LERP_RESETANIM2;
		}
		else
		{
			e->lerpstart = cl.time;
			e->previouspose = e->currentpose;
			e->currentpose = posenum;
		}
	}

	//set up values
	if (r_lerpmodels.value && !(e->model->flags & MOD_NOLERP && r_lerpmodels.value != 2))
	{
		float s = (cls.demoplayback && cls.demospeed < 0.f) ? -1.f : 1.f;
		if (e->lerpflags & LERP_FINISH && numposes == 1)
			lerpdata->blend = CLAMP (0.0f, (float)(cl.time - e->lerpstart) / (e->lerpfinish - e->lerpstart), 1.0f);
		else
			lerpdata->blend = CLAMP (0.0f, (float)(cl.time - e->lerpstart) / e->lerptime * s, 1.0f);
		if (lerpdata->blend == 1.0f)
			e->previouspose = e->currentpose;
		lerpdata->pose1 = e->previouspose;
		lerpdata->pose2 = e->currentpose;
	}
	else //don't lerp
	{
		lerpdata->blend = 1;
		lerpdata->pose1 = posenum;
		lerpdata->pose2 = posenum;
	}
}

/*
=================
R_SetupEntityTransform -- johnfitz -- set up transform part of lerpdata
=================
*/
void R_SetupEntityTransform (entity_t *e, lerpdata_t *lerpdata)
{
	float blend;
	vec3_t d;
	int i;

	// if LERP_RESETMOVE, kill any lerps in progress
	if (e->lerpflags & LERP_RESETMOVE)
	{
		e->movelerpstart = 0;
		VectorCopy (e->origin, e->previousorigin);
		VectorCopy (e->origin, e->currentorigin);
		VectorCopy (e->angles, e->previousangles);
		VectorCopy (e->angles, e->currentangles);
		e->lerpflags -= LERP_RESETMOVE;
	}
	else if (!VectorCompare (e->origin, e->currentorigin) || !VectorCompare (e->angles, e->currentangles)) // origin/angles changed, start new lerp
	{
		e->movelerpstart = cl.time;
		VectorCopy (e->currentorigin, e->previousorigin);
		VectorCopy (e->origin,  e->currentorigin);
		VectorCopy (e->currentangles, e->previousangles);
		VectorCopy (e->angles,  e->currentangles);
	}

	//set up values
	if (r_lerpmove.value && e != &cl.viewent && e->lerpflags & LERP_MOVESTEP)
	{
		float s = (cls.demoplayback && cls.demospeed < 0.f) ? -1.f : 1.f;
		if (e->lerpflags & LERP_FINISH)
			blend = CLAMP (0.0f, (float)(cl.time - e->movelerpstart) / (e->lerpfinish - e->movelerpstart), 1.0f);
		else
			blend = CLAMP (0.0f, (float)(cl.time - e->movelerpstart) / 0.1f * s, 1.0f);

		//translation
		VectorSubtract (e->currentorigin, e->previousorigin, d);
		lerpdata->origin[0] = e->previousorigin[0] + d[0] * blend;
		lerpdata->origin[1] = e->previousorigin[1] + d[1] * blend;
		lerpdata->origin[2] = e->previousorigin[2] + d[2] * blend;

		//rotation
		VectorSubtract (e->currentangles, e->previousangles, d);
		for (i = 0; i < 3; i++)
		{
			if (d[i] > 180)  d[i] -= 360;
			if (d[i] < -180) d[i] += 360;
		}
		lerpdata->angles[0] = e->previousangles[0] + d[0] * blend;
		lerpdata->angles[1] = e->previousangles[1] + d[1] * blend;
		lerpdata->angles[2] = e->previousangles[2] + d[2] * blend;
	}
	else //don't lerp
	{
		VectorCopy (e->origin, lerpdata->origin);
		VectorCopy (e->angles, lerpdata->angles);
	}

	// chasecam
	if (chase_active.value && e == &cl_entities[cl.viewentity])
		lerpdata->angles[PITCH] *= 0.3f;
}

/*
=================
R_SetupAliasLighting -- johnfitz -- broken out from R_DrawAliasModel and rewritten
=================
*/
void R_SetupAliasLighting (entity_t	*e)
{
	vec3_t		dist;
	float		add;
	int			i;

	// if the initial trace is completely black, try again from above
	// this helps with models whose origin is slightly below ground level
	// (e.g. some of the candles in the DOTM start map)
	if (!R_LightPoint (e->origin, 0.f, &e->lightcache))
		R_LightPoint (e->origin, e->model->maxs[2] * 0.5f, &e->lightcache);

	//add dlights
	for (i=0; i<r_framedata.numlights; i++)
	{
		gpulight_t *l = &r_lightbuffer.lights[i];
		VectorSubtract (e->origin, l->pos, dist);
		add = DotProduct (dist, dist);
		if (l->radius * l->radius > add)
			VectorMA (lightcolor, l->radius - sqrtf (add), l->color, lightcolor);
	}

	// minimum light value on gun (24)
	if (e == &cl.viewent)
	{
		add = 72.0f - (lightcolor[0] + lightcolor[1] + lightcolor[2]);
		if (add > 0.0f)
		{
			add *= 1.0f / 3.0f;
			lightcolor[0] += add;
			lightcolor[1] += add;
			lightcolor[2] += add;
		}
	}

	// minimum light value on players (8)
	if (e > cl_entities && e <= cl_entities + cl.maxclients)
	{
		add = 24.0f - (lightcolor[0] + lightcolor[1] + lightcolor[2]);
		if (add > 0.0f)
		{
			add *= 1.0f / 3.0f;
			lightcolor[0] += add;
			lightcolor[1] += add;
			lightcolor[2] += add;
		}
	}

	// clamp lighting so it doesn't overbright as much (96)
	if (gl_overbright_models.value)
	{
		add = lightcolor[0] + lightcolor[1] + lightcolor[2];
		if (add > 288.0f)
			VectorScale(lightcolor, 288.0f / add, lightcolor);
	}
	//hack up the brightness when fullbrights but no overbrights (256)
	else if (e->model->flags & MOD_FBRIGHTHACK && gl_fullbrights.value)
	{
		lightcolor[0] = 256.0f;
		lightcolor[1] = 256.0f;
		lightcolor[2] = 256.0f;
	}

	VectorScale (lightcolor, 1.0f / 200.0f, lightcolor);
}

/*
=================
R_FlushAliasInstances
=================
*/
void R_FlushAliasInstances (void)
{
	qmodel_t	*model;
	aliashdr_t	*paliashdr;
	qboolean	alphatest, translucent, oit;
	GLuint		program;
	unsigned	state;
	GLuint		buf;
	GLbyte		*ofs;
	size_t		ibuf_size;
	GLuint		buffers[3];
	GLintptr	offsets[3];
	GLsizeiptr	sizes[3];

	if (!ibuf.count)
		return;

	model = ibuf.ent->model;
	paliashdr = (aliashdr_t *)Mod_Extradata (model);

	GL_BeginGroup (model->name);

	alphatest = model->flags & MF_HOLEY ? 1 : 0;
	translucent = !ENTALPHA_OPAQUE (ibuf.ent->alpha);
	oit = translucent && r_oit.value != 0.f;
	switch (softemu)
	{
	case SOFTEMU_BANDED:
		program = glprogs.alias[oit][ALIASSHADER_NOPERSP][alphatest];
		break;
	case SOFTEMU_COARSE:
		program = glprogs.alias[oit][ALIASSHADER_DITHER][alphatest];
		break;
	default:
		program = glprogs.alias[oit][ALIASSHADER_STANDARD][alphatest];
		break;
	}
	GL_UseProgram (program);

	state = GLS_CULL_BACK | GLS_ATTRIBS(0);
	if (!translucent)
		state |= GLS_BLEND_OPAQUE;
	else
		state |= GLS_BLEND_ALPHA_OIT | GLS_NO_ZWRITE;
	GL_SetState (state);

	memcpy (ibuf.global.matviewproj, r_matviewproj, sizeof (r_matviewproj));
	memcpy (ibuf.global.eyepos, r_refdef.vieworg, sizeof (r_refdef.vieworg));
	memcpy (ibuf.global.fog, r_framedata.fogdata, 3 * sizeof (float));
	// use fog density sign bit as overbright flag
	ibuf.global.fog[3] =
		gl_overbright_models.value ?
			-fabs (r_framedata.fogdata[3]) :
			 fabs (r_framedata.fogdata[3])
	;

	ibuf_size = sizeof(ibuf.global) + sizeof(ibuf.inst[0]) * ibuf.count;
	GL_Upload (GL_SHADER_STORAGE_BUFFER, &ibuf.global, ibuf_size, &buf, &ofs);

	buffers[0] = buf;
	buffers[1] = model->meshvbo;
	buffers[2] = model->meshvbo;
	offsets[0] = (GLintptr) ofs;
	offsets[1] = model->vboxyzofs;
	offsets[2] = model->vbostofs;
	sizes[0] = ibuf_size;
	sizes[1] = sizeof (meshxyz_t) * paliashdr->numverts_vbo * paliashdr->numposes;
	sizes[2] = sizeof (meshst_t) * paliashdr->numverts_vbo;
	GL_BindBuffersRange (GL_SHADER_STORAGE_BUFFER, 1, 3, buffers, offsets, sizes);

	GL_BindTextures (0, 2, ibuf.textures);

	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, model->meshindexesvbo);
	GL_DrawElementsInstancedFunc (GL_TRIANGLES, paliashdr->numindexes, GL_UNSIGNED_SHORT, (void *)(intptr_t)model->vboindexofs, ibuf.count);

	rs_aliaspasses += paliashdr->numtris * ibuf.count;
	ibuf.count = 0;

	GL_EndGroup();
}

/*
=================
R_DrawAliasModel_Real
=================
*/
static void R_DrawAliasModel_Real (entity_t *e, qboolean showtris)
{
	aliashdr_t	*paliashdr;
	int			anim, skinnum;
	gltexture_t	*tx, *fb;
	lerpdata_t	lerpdata;
	float		fovscale = 1.0f;
	float		model_matrix[16];
	aliasinstance_t	*instance;

	//
	// setup pose/lerp data -- do it first so we don't miss updates due to culling
	//
	paliashdr = (aliashdr_t *)Mod_Extradata (e->model);
	R_SetupAliasFrame (e, paliashdr, &lerpdata);
	R_SetupEntityTransform (e, &lerpdata);

	if (lerpdata.pose1 == lerpdata.pose2)
		lerpdata.blend = 0.f;

	//
	// cull it
	//
	if (R_CullModelForEntity(e))
		return;

	//
	// transform it
	//
	if (e == &cl.viewent && r_refdef.basefov > 90.f && cl_gun_fovscale.value)
	{
		fovscale = tan(r_refdef.basefov * (0.5f * M_PI / 180.f));
		fovscale = 1.f + (fovscale - 1.f) * cl_gun_fovscale.value;
	}

	R_EntityMatrix (model_matrix, lerpdata.origin, lerpdata.angles, e->scale);
	ApplyTranslation (model_matrix, paliashdr->scale_origin[0], paliashdr->scale_origin[1] * fovscale, paliashdr->scale_origin[2] * fovscale);
	ApplyScale (model_matrix, paliashdr->scale[0], paliashdr->scale[1] * fovscale, paliashdr->scale[2] * fovscale);

	//
	// set up for alpha blending
	//
	if (r_lightmap_cheatsafe) //no alpha in drawflat or lightmap mode
		entalpha = 1;
	else
		entalpha = ENTALPHA_DECODE(e->alpha);

	if (entalpha == 0)
		return;

	//
	// set up lighting
	//
	rs_aliaspolys += paliashdr->numtris;
	R_SetupAliasLighting (e);

	//
	// set up textures
	//
	anim = (int)(cl.time*10) & 3;
	skinnum = e->skinnum;
	if ((skinnum >= paliashdr->numskins) || (skinnum < 0))
	{
		Con_DPrintf ("R_DrawAliasModel: no such skin # %d for '%s'\n", skinnum, e->model->name);
		// ericw -- display skin 0 for winquake compatibility
		skinnum = 0;
	}
	tx = paliashdr->gltextures[skinnum][anim];
	fb = paliashdr->fbtextures[skinnum][anim];
	if (e->colormap != vid.colormap && !gl_nocolors.value)
	{
		if ((uintptr_t)e >= (uintptr_t)&cl_entities[1] && (uintptr_t)e <= (uintptr_t)&cl_entities[cl.maxclients]) /* && !strcmp (currententity->model->name, "progs/player.mdl") */
			tx = playertextures[e - cl_entities - 1];
	}
	if (!gl_fullbrights.value)
		fb = blacktexture;

	//
	// draw it
	//

	if (r_fullbright_cheatsafe)
		lightcolor[0] = lightcolor[1] = lightcolor[2] = 0.5f;

	if (r_lightmap_cheatsafe)
	{
		tx = greytexture;
		fb = blacktexture;
	}

	if (!fb)
		fb = blacktexture;

	if (showtris)
	{
		tx = blacktexture;
		fb = whitetexture;
		lightcolor[0] = lightcolor[1] = lightcolor[2] = 0.5f;
		entalpha = 1.f;
	}

	if (ibuf.count)
	{
		if (ibuf.count == countof(ibuf.inst) ||
			ibuf.ent->model != e->model ||
			ibuf.textures[0] != tx ||
			ibuf.textures[1] != fb)
		{
			R_FlushAliasInstances ();
		}
	}

	if (!ibuf.count)
	{
		ibuf.ent         = e;
		ibuf.textures[0] = tx;
		ibuf.textures[1] = fb;
	}

	instance = &ibuf.inst[ibuf.count++];

	MatrixTranspose4x3 (model_matrix, instance->worldmatrix);

	instance->lightcolor[0] = lightcolor[0];
	instance->lightcolor[1] = lightcolor[1];
	instance->lightcolor[2] = lightcolor[2];
	instance->alpha = entalpha;
	instance->pose1 = lerpdata.pose1 * paliashdr->numverts_vbo;
	instance->pose2 = lerpdata.pose2 * paliashdr->numverts_vbo;
	instance->blend = lerpdata.blend;
}

/*
=================
R_DrawAliasModels
=================
*/
void R_DrawAliasModels (entity_t **ents, int count)
{
	int i;
	for (i = 0; i < count; i++)
		R_DrawAliasModel_Real (ents[i], false);
	R_FlushAliasInstances ();
}

/*
=================
R_DrawAliasModels_ShowTris
=================
*/
void R_DrawAliasModels_ShowTris (entity_t **ents, int count)
{
	int i;
	for (i = 0; i < count; i++)
		R_DrawAliasModel_Real (ents[i], true);
	R_FlushAliasInstances ();
}
