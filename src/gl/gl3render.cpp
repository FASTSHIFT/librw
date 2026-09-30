#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#ifdef RW_OPENGL
#include "rwgl3.h"
#include "rwgl3shader.h"

#include "rwgl3impl.h"

namespace rw {
namespace gl3 {

#define MAX_LIGHTS 

// Per-frame draw call counter (perf HUD support, ported from re3).
// Reset each frame by the skeleton; read via gl3_get_and_reset_drawcalls().
int gl3_drawcall_count = 0;
int gl3_get_and_reset_drawcalls(void) { int n = gl3_drawcall_count; gl3_drawcall_count = 0; return n; }

// ---- GPU timer-query markers (perf HUD) -----------------------------------
// Up to GL3_MAX_GPU_MARKERS named ranges per frame. The engine brackets the
// interesting passes (scene submit, 2D, swap) with gl3GpuMarkerBegin/End and
// reads back the GPU-side durations (ns) after the vsync via gl3GpuFrameStats.
// Timer queries are core in GL ES 3.0; the Mali blob supports them.
#define GL3_MAX_GPU_MARKERS 8
struct GpuMarker {
	GLuint query;
	uint64 gpuNs;
	bool active;
};
static GpuMarker gl3_gpuMarkers[GL3_MAX_GPU_MARKERS];
static int gl3_gpuMarkerCount = 0;
static char gl3_gpuMarkerNames[GL3_MAX_GPU_MARKERS][16];

void
gl3GpuMarkerBegin(const char *name)
{
	// Defensive: the glad loader here is built from the GLES header set, so
	// query entry points may be NULL on some desktop contexts. Probe once
	// and silently disable markers if unavailable (perf HUD just shows
	// nothing instead of crashing - the ui64v variant was a NULL call).
	static int available = -1;
	if(available < 0)
		available = (glGenQueries != NULL && glBeginQuery != NULL &&
		             glEndQuery != NULL && glGetQueryObjectuiv != NULL) ? 1 : 0;
	if(!available)
		return;
	if(gl3_gpuMarkerCount >= GL3_MAX_GPU_MARKERS)
		return;
	GLuint q;
	glGenQueries(1, &q);
	glBeginQuery(GL_TIME_ELAPSED, q);
	strncpy(gl3_gpuMarkerNames[gl3_gpuMarkerCount], name, 15);
	gl3_gpuMarkerNames[gl3_gpuMarkerCount][15] = '\0';
	gl3_gpuMarkers[gl3_gpuMarkerCount].query = q;
	gl3_gpuMarkers[gl3_gpuMarkerCount].active = true;
	gl3_gpuMarkerCount++;
}

void
gl3GpuMarkerEnd(void)
{
	// Close the most recently opened marker
	for(int i = gl3_gpuMarkerCount - 1; i >= 0; i--) {
		if(gl3_gpuMarkers[i].active) {
			glEndQuery(GL_TIME_ELAPSED);
			gl3_gpuMarkers[i].active = false;
			return;
		}
	}
}

int
gl3GpuFrameStats(G3GpuMarkerStat *out, int maxOut)
{
	int n = 0;
	for(int i = 0; i < gl3_gpuMarkerCount && n < maxOut; i++) {
		// Use the 32-bit glGetQueryObjectuiv, not the ui64v variant: the
		// glad loader here is built from the GLES header set, so the
		// 64-bit entry point may be a NULL pointer (crash confirmed on
		// Mesa 4.1 core and the Mali blob). 32 bits of nanoseconds still
		// covers 4.29 seconds per marker - far beyond any frame segment.
		GLuint v = 0;
		if(glGetQueryObjectuiv == NULL) break;
		glGetQueryObjectuiv(gl3_gpuMarkers[i].query, GL_QUERY_RESULT, &v);
		gl3_gpuMarkers[i].gpuNs = (uint64)v;
		strncpy(out[n].name, gl3_gpuMarkerNames[i], sizeof(out[n].name));
		out[n].name[sizeof(out[n].name)-1] = '\0';
		out[n].gpuMs = v / 1000000.0;
		n++;
	}
	return n;
}

void
gl3GpuMarkerFrameReset(void)
{
	for(int i = 0; i < gl3_gpuMarkerCount; i++)
		glDeleteQueries(1, &gl3_gpuMarkers[i].query);
	gl3_gpuMarkerCount = 0;
}

// ---- draw-call trace (batching feasibility analysis) -----------------------
// Records the per-draw state tuple (raster, shader, blend/alpha) for one
// frame on demand, then reports: total draws, state-tuple switches, and the
// number of GL-bind-worthy changes (raster/shader flips between consecutive
// draws). Read via gl3DrawTraceReport after the frame.
#define GL3_TRACE_MAX_DRAWS 4096
struct DrawTraceEntry { void *raster; void *shader; bool alpha; bool blend; };
static DrawTraceEntry gl3_drawTrace[GL3_TRACE_MAX_DRAWS];
static int gl3_drawTraceCount = 0;
static bool gl3_drawTraceEnabled = false;

void gl3DrawTraceBegin(void) { gl3_drawTraceCount = 0; gl3_drawTraceEnabled = true; }
void gl3DrawTraceEnd(void) { gl3_drawTraceEnabled = false; }

static inline void
gl3TraceDraw(void *raster, void *shader, bool alpha, bool blend)
{
	if(!gl3_drawTraceEnabled) return;
	if(gl3_drawTraceCount >= GL3_TRACE_MAX_DRAWS) return;
	gl3_drawTrace[gl3_drawTraceCount].raster = raster;
	gl3_drawTrace[gl3_drawTraceCount].shader = shader;
	gl3_drawTrace[gl3_drawTraceCount].alpha = alpha;
	gl3_drawTrace[gl3_drawTraceCount].blend = blend;
	gl3_drawTraceCount++;
}

int
gl3DrawTraceReport(int *outDraws, int *outRasterSwitch, int *outShaderSwitch,
                   int *outAlphaSwitch, int *outMergeableRuns)
{
	int draws = gl3_drawTraceCount;
	int rasterSw = 0, shaderSw = 0, alphaSw = 0, runs = 0;
	int i;
	int runStart = 0;
	for(i = 1; i <= draws; i++) {
		bool boundary = i == draws;
		if(!boundary) {
			// A "mergeable run" = same raster AND same shader AND same alpha
			// state AND both opaque (alpha-blended draws cannot be reordered
			// freely against each other without changing output).
			if(gl3_drawTrace[i].raster != gl3_drawTrace[i-1].raster) { rasterSw++; boundary = true; }
			if(gl3_drawTrace[i].shader != gl3_drawTrace[i-1].shader) { shaderSw++; boundary = true; }
			if(gl3_drawTrace[i].alpha  != gl3_drawTrace[i-1].alpha)  { alphaSw++;  boundary = true; }
		}
		if(boundary) {
			int len = i - runStart;
			// Runs of >1 identical-state draws could merge into one draw call
			// (multi-draw or index-range concatenation).
			if(len > 1) runs += len - 1;
			runStart = i;
		}
	}
	if(outDraws) *outDraws = draws;
	if(outRasterSwitch) *outRasterSwitch = rasterSw;
	if(outShaderSwitch) *outShaderSwitch = shaderSw;
	if(outAlphaSwitch) *outAlphaSwitch = alphaSw;
	if(outMergeableRuns) *outMergeableRuns = runs;
	return draws;
}

// Distinct-raster histogram helper for the batching analysis: counts unique
// rasters in the last trace and how many draws the top-repeated rasters
// account for (tells whether sorting alone helps or atlasing is needed).
void
gl3DrawTraceRasters(int *outDistinct, int *outTop1Count, int *outTop4Count)
{
	int distinct = 0, i, j;
	int top1 = 0, top4 = 0;
	int counts[GL3_TRACE_MAX_DRAWS];
	bool counted[GL3_TRACE_MAX_DRAWS];
	for(i = 0; i < gl3_drawTraceCount; i++) counted[i] = false;
	for(i = 0; i < gl3_drawTraceCount; i++) {
		if(counted[i]) continue;
		int c = 1;
		counted[i] = true;
		for(j = i+1; j < gl3_drawTraceCount; j++) {
			if(!counted[j] && gl3_drawTrace[j].raster == gl3_drawTrace[i].raster) {
				counted[j] = true;
				c++;
			}
		}
		counts[distinct++] = c;
	}
	// simple selection of top 4
	for(int t = 0; t < 4 && t < distinct; t++) {
		int best = t;
		for(j = t+1; j < distinct; j++) if(counts[j] > counts[best]) best = j;
		int tmp = counts[t]; counts[t] = counts[best]; counts[best] = tmp;
		top4 += counts[t];
		if(t == 0) top1 = counts[0];
	}
	if(outDistinct) *outDistinct = distinct;
	if(outTop1Count) *outTop1Count = top1;
	if(outTop4Count) *outTop4Count = top4;
}

void
drawInst_simple(InstanceDataHeader *header, InstanceData *inst)
{
	gl3_count_drawcall();
	// State-cache lookups are file-local to gl3device.cpp; reuse the public
	// query helpers instead (raster pointer from the trace's own getter).
	gl3TraceDraw(gl3GetBoundRaster0(),
	             (void*)(uintptr)(getAlphaTest() ? 1 : 0),
	             (rw::GetRenderState(rw::VERTEXALPHA) != 0),
	             getAlphaBlend());
	flushCache();
	glDrawElements(header->primType, inst->numIndex,
	               GL_UNSIGNED_SHORT, (void*)(uintptr)inst->offset);
}

// Emulate PS2 GS alpha test FB_ONLY case: failed alpha writes to frame- but not to depth buffer
void
drawInst_GSemu(InstanceDataHeader *header, InstanceData *inst)
{
	uint32 hasAlpha;
	int alphafunc, alpharef, gsalpharef;
	int zwrite;
	hasAlpha = getAlphaBlend();
	if(hasAlpha){
		zwrite = rw::GetRenderState(rw::ZWRITEENABLE);
		alphafunc = rw::GetRenderState(rw::ALPHATESTFUNC);
		if(zwrite){
			alpharef = rw::GetRenderState(rw::ALPHATESTREF);
			gsalpharef = rw::GetRenderState(rw::GSALPHATESTREF);

			SetRenderState(rw::ALPHATESTFUNC, rw::ALPHAGREATEREQUAL);
			SetRenderState(rw::ALPHATESTREF, gsalpharef);
			drawInst_simple(header, inst);
			SetRenderState(rw::ALPHATESTFUNC, rw::ALPHALESS);
			SetRenderState(rw::ZWRITEENABLE, 0);
			drawInst_simple(header, inst);
			SetRenderState(rw::ZWRITEENABLE, 1);
			SetRenderState(rw::ALPHATESTFUNC, alphafunc);
			SetRenderState(rw::ALPHATESTREF, alpharef);
		}else{
			SetRenderState(rw::ALPHATESTFUNC, rw::ALPHAALWAYS);
			drawInst_simple(header, inst);
			SetRenderState(rw::ALPHATESTFUNC, alphafunc);
		}
	}else
		drawInst_simple(header, inst);
}

void
drawInst(InstanceDataHeader *header, InstanceData *inst)
{
	if(rw::GetRenderState(rw::GSALPHATEST))
		drawInst_GSemu(header, inst);
	else
		drawInst_simple(header, inst);
}


void
setAttribPointers(AttribDesc *attribDescs, int32 numAttribs)
{
	AttribDesc *a;
	for(a = attribDescs; a != &attribDescs[numAttribs]; a++){
		glEnableVertexAttribArray(a->index);
		glVertexAttribPointer(a->index, a->size, a->type, a->normalized,
		                      a->stride, (void*)(uint64)a->offset);
	}
}

void
disableAttribPointers(AttribDesc *attribDescs, int32 numAttribs)
{
	AttribDesc *a;
	for(a = attribDescs; a != &attribDescs[numAttribs]; a++)
		glDisableVertexAttribArray(a->index);
}

void
setupVertexInput(InstanceDataHeader *header)
{
#ifdef RW_GL_USE_VAOS
	glBindVertexArray(header->vao);
#else
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, header->ibo);
	glBindBuffer(GL_ARRAY_BUFFER, header->vbo);
	setAttribPointers(header->attribDesc, header->numAttribs);
#endif
}

void
teardownVertexInput(InstanceDataHeader *header)
{
#ifndef RW_GL_USE_VAOS
	disableAttribPointers(header->attribDesc, header->numAttribs);
#endif
}

int32
lightingCB(Atomic *atomic)
{
	WorldLights lightData;
	Light *directionals[8];
	Light *locals[8];
	lightData.directionals = directionals;
	lightData.numDirectionals = 8;
	lightData.locals = locals;
	lightData.numLocals = 8;

	if(atomic->geometry->flags & rw::Geometry::LIGHT){
		((World*)engine->currentWorld)->enumerateLights(atomic, &lightData);
		if((atomic->geometry->flags & rw::Geometry::NORMALS) == 0){
			// Get rid of lights that need normals when we don't have any
			lightData.numDirectionals = 0;
			lightData.numLocals = 0;
		}
		return setLights(&lightData);
	}else{
		memset(&lightData, 0, sizeof(lightData));
		return setLights(&lightData);
	}
}

void
defaultRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	Material *m;

	uint32 flags = atomic->geometry->flags;
	setWorldMatrix(atomic->getFrame()->getLTM());
	int32 vsBits = lightingCB(atomic);

	setupVertexInput(header);

	InstanceData *inst = header->inst;
	int32 n = header->numMeshes;

	while(n--){
		m = inst->material;

		setMaterial(flags, m->color, m->surfaceProps);

		setTexture(0, m->texture);

		rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);

		if((vsBits & VSLIGHT_MASK) == 0){
			if(getAlphaTest())
				defaultShader->use();
			else
				defaultShader_noAT->use();
		}else{
			if(getAlphaTest())
				defaultShader_fullLight->use();
			else
				defaultShader_fullLight_noAT->use();
		}

		drawInst(header, inst);
		inst++;
	}
	teardownVertexInput(header);
}


}
}

#endif

