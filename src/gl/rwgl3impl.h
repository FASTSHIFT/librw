namespace rw {
namespace gl3 {

#ifdef RW_OPENGL

// Per-frame draw call counter (perf HUD support, ported from re3). Reset each
// frame by the skeleton; read via gl3_get_and_reset_drawcalls(). Thread-safe
// for single-render-thread use.
extern int gl3_drawcall_count;
static inline void gl3_count_drawcall(void) { gl3_drawcall_count++; }
int gl3_get_and_reset_drawcalls(void); // returns count and resets to 0

// GPU timer-query markers (perf HUD). Bracket passes with Begin/End; after the
// vsync swap, call gl3GpuFrameStats to read back GPU-side ms per marker, then
// gl3GpuMarkerFrameReset before the next frame's markers.
struct G3GpuMarkerStat { char name[16]; double gpuMs; };
void gl3GpuMarkerBegin(const char *name);
void gl3GpuMarkerEnd(void);
int  gl3GpuFrameStats(G3GpuMarkerStat *out, int maxOut); // returns count filled
void gl3GpuMarkerFrameReset(void);

extern uint32 im2DVbo, im2DIbo;
void openIm2D(void);
void closeIm2D(void);
void im2DRenderLine(void *vertices, int32 numVertices,
  int32 vert1, int32 vert2);
void im2DRenderTriangle(void *vertices, int32 numVertices,
  int32 vert1, int32 vert2, int32 vert3);
void im2DRenderPrimitive(PrimitiveType primType,
   void *vertices, int32 numVertices);
void im2DRenderIndexedPrimitive(PrimitiveType primType,
   void *vertices, int32 numVertices, void *indices, int32 numIndices);

void openIm3D(void);
void closeIm3D(void);
void im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags);
void im3DRenderPrimitive(PrimitiveType primType);
void im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices);
void im3DEnd(void);

struct DisplayMode
{
#ifdef LIBRW_SDL2
	SDL_DisplayMode mode;
#else
	GLFWvidmode mode;
#endif
	int32 depth;
	uint32 flags;
};

struct GlGlobals
{
#ifdef LIBRW_SDL2
	SDL_Window **pWindow;
	SDL_Window *window;
	SDL_GLContext glcontext;
#else
	GLFWwindow **pWindow;
	GLFWwindow *window;
	GLFWmonitor *monitor;
#endif

	int numMonitors;
	int currentMonitor;
	DisplayMode *modes;
	int numModes;
	int currentMode;
	int presentWidth, presentHeight;
	int presentOffX, presentOffY;

	// for opening the window
	int winWidth, winHeight;
	const char *winTitle;
	uint32 numSamples;
};

extern GlGlobals glGlobals;
#endif

Raster *rasterCreate(Raster *raster);
uint8 *rasterLock(Raster*, int32 level, int32 lockMode);
void rasterUnlock(Raster*, int32);
int32 rasterNumLevels(Raster*);
bool32 imageFindRasterFormat(Image *img, int32 type,
	int32 *width, int32 *height, int32 *depth, int32 *format);
bool32 rasterFromImage(Raster *raster, Image *image);
Image *rasterToImage(Raster *raster);

}
}
