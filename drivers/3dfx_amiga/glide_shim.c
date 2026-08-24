/*
 * Minimal Glide 2.x compatibility layer for the classic Amiga MiniGL API.
 *
 * The BRender 3dfx driver performs transformation and lighting on the CPU,
 * so this file only translates its rasterisation, texture and LFB calls to
 * fixed-function OpenGL.  MiniGL then sends those calls to Warp3D.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "glide.h"
#include "glideutl.h"
#include <cybergraphx/cybergraphics.h>
#include <proto/cybergraphics.h>
#ifdef DETHRACE_AMIGA_SHARED_MINIGL
#include <proto/minigl.h>
#include <clib/minigl_open_protos.h>
#else
#include <mgl/gl.h>
#endif

#ifndef DETHRACE_AMIGA_SHARED_MINIGL
/* The installed headers are newer than libmgl-cosmos.a: the library's
 * GLcontext has an extra 400 bytes before this tail.  These offsets come from
 * the library's MGLSwitchDisplay/MGLLockBack code and are part of its ABI. */
typedef struct fx_mgl_cosmos_context {
    unsigned char reserved[4252];
    W3D_Context *w3dContext;
    struct Window *w3dWindow;
    struct Screen *w3dScreen;
    GLboolean arrayTexBound;
    GLint currentBinding;
    GLint virtualBinding;
    GLuint virtualTexUnits;
    GLuint activeTexture;
    W3D_Texture **w3dTexBuffer;
    GLubyte **w3dTexMemory;
    GLint texBufferSize;
    struct ScreenBuffer *buffers[3];
    struct BitMap *w3dBitMap;
    struct RastPort *w3dRastPort;
    int bufNr;
    int numBuffers;
} fx_mgl_cosmos_context;

_Static_assert(offsetof(fx_mgl_cosmos_context, w3dContext) == 4252, "MiniGL cosmos ABI");
_Static_assert(offsetof(fx_mgl_cosmos_context, buffers) == 4296, "MiniGL cosmos ABI");
_Static_assert(offsetof(fx_mgl_cosmos_context, w3dBitMap) == 4308, "MiniGL cosmos ABI");
_Static_assert(offsetof(fx_mgl_cosmos_context, bufNr) == 4316, "MiniGL cosmos ABI");
_Static_assert(offsetof(fx_mgl_cosmos_context, numBuffers) == 4320, "MiniGL cosmos ABI");
#endif

#define FXA_MAX_TEXTURES 1024
#define FXA_LFB_STRIDE_PIXELS 1024
#define FXA_TEXTURE_MEMORY (16UL * 1024UL * 1024UL)
/* Overlay mask values: 1 = HUD content carried over from an earlier frame,
 * 2 = drawn by a blit during the current frame (protected from being cleared by
 * a later overlapping blit's transparent texels). */
#define FXA_HUD_MASK_CARRIED 1
#define FXA_HUD_MASK_THIS_FRAME 2

#define FXA_LFB_TILE_SIZE 256
/* Enough tiles for the largest resolution grResolution_size() can select
 * (960x720 -> 4x3).  This was 6, which only covered 640x480; every larger mode
 * indexed lfb_tile_textures[] out of bounds and bound stray texture names. */
#define FXA_LFB_TILE_COLUMNS 4
#define FXA_LFB_TILE_ROWS 3
#define FXA_LFB_TILE_COUNT (FXA_LFB_TILE_COLUMNS * FXA_LFB_TILE_ROWS)

typedef struct fx_amiga_texture {
    FxBool used;
    FxU32 address;
    FxU32 size;
    GLuint name;
    int width;
    int height;
} fx_amiga_texture;

static fx_amiga_texture textures[FXA_MAX_TEXTURES];
static FxU32 palette[256];
static GrErrorCallbackFnc_t error_callback;
static GrColor_t chromakey;
static GLfloat constant_rgba[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
static GLint state_tex_min_filter = GL_NEAREST;
static GLint state_tex_mag_filter = GL_NEAREST;
static GLint state_tex_wrap_s = GL_REPEAT;
static GLint state_tex_wrap_t = GL_REPEAT;
static void apply_texture_parameters(void);
static GrColorCombineFnc_t colour_combine = GR_COLORCOMBINE_ITRGB;
static GrAlphaSource_t alpha_source = GR_ALPHASOURCE_CC_ALPHA;
static GrBuffer_t render_buffer = GR_BUFFER_BACKBUFFER;
static int screen_width = 640;
static int screen_height = 480;
static int texture_width = 256;
static int texture_height = 256;
static GLfloat texture_inv_width = 1.0f / 256.0f;
static GLfloat texture_inv_height = 1.0f / 256.0f;
static FxBool chromakey_enabled;
static int lfb_lock_depth;
static int lfb_suspended_lock_depth;
static FxU16 *lfb_colour;
static FxU16 *lfb_original;
static FxU16 *lfb_lock_snapshot;
static FxU16 *lfb_depth;
static FxU16 *lfb_overlay_colour;
static FxU8 *lfb_overlay_mask;
static FxU32 *lfb_tile_cache;
static GLuint lfb_tile_textures[FXA_LFB_TILE_COUNT];
static FxBool lfb_tiles_created;
static int lfb_active_tile_count;
static FxBool lfb_initial_capture_done;
static FxBool previous_frame_had_3d;
static MGLLockInfo lfb_info;
static APTR bitmap_lock_handle;
static int lfb_precommitted;
static int lfb_3d_prepared;
static FxBool lfb_operation_region_valid;
static FxBool lfb_operation_read_only;
static FxBool lfb_operation_skip_scan;
static int lfb_operation_min_x;
static int lfb_operation_min_y;
static int lfb_operation_max_x;
static int lfb_operation_max_y;
static unsigned long frame_3d_triangles;
static unsigned long frame_3d_lines;
static unsigned long frame_3d_points;
static FxBool frame_world_rendered;
static FxBool state_texture_enabled;
static GLuint state_bound_texture;
static FxBool state_blend_enabled;
static GLenum state_blend_src = GL_ONE;
static GLenum state_blend_dst = GL_ZERO;
static FxBool state_depth_enabled;
static FxBool state_depth_mask = FXTRUE;
static FxBool state_colour_mask_rgb = FXTRUE;
static FxBool state_cull_enabled;
static GrCullMode_t state_cull_mode = GR_CULL_DISABLE;
static FxBool state_fog_enabled;
static FxBool frame_depth_cleared;
static FxBool frame_overlay_reset;
static FxBool frame_surface_cleared;

/* Keep MiniGL's proven immediate-mode/projective texture path, but avoid one
 * glBegin/glEnd dispatch pair for every individual triangle.  MiniGL's vertex
 * buffer holds 1024 vertices, so end the batch at the largest whole-triangle
 * count below that limit. */
#define FXA_IMMEDIATE_BATCH_VERTICES 1023
static FxBool triangle_batch_open;
static int triangle_batch_vertex_count;

static void flush_triangle_batch(void);

static FxU16 rgb_to_565(unsigned int r, unsigned int g, unsigned int b)
{
    return (FxU16)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

static FxU16 read_bitmap_pixel(const unsigned char *pixel, ULONG format)
{
    FxU16 value;
    unsigned int r, g, b;
    switch(format) {
    case PIXFMT_ARGB32: r = pixel[1]; g = pixel[2]; b = pixel[3]; break;
    case PIXFMT_BGRA32: b = pixel[0]; g = pixel[1]; r = pixel[2]; break;
    case PIXFMT_RGBA32: r = pixel[0]; g = pixel[1]; b = pixel[2]; break;
    case PIXFMT_RGB24:  r = pixel[0]; g = pixel[1]; b = pixel[2]; break;
    case PIXFMT_BGR24:  b = pixel[0]; g = pixel[1]; r = pixel[2]; break;
    case PIXFMT_RGB16PC:
    case PIXFMT_BGR16PC:
        value = (FxU16)(pixel[0] | (pixel[1] << 8));
        if(format == PIXFMT_RGB16PC) return value;
        return rgb_to_565((value & 31) << 3, ((value >> 5) & 63) << 2,
            ((value >> 11) & 31) << 3);
    case PIXFMT_BGR16:
        value = (FxU16)((pixel[0] << 8) | pixel[1]);
        return rgb_to_565((value & 31) << 3, ((value >> 5) & 63) << 2,
            ((value >> 11) & 31) << 3);
    case PIXFMT_RGB16:
    default:
        return (FxU16)((pixel[0] << 8) | pixel[1]);
    }
    return rgb_to_565(r, g, b);
}

static void write_bitmap_pixel(unsigned char *pixel, ULONG format, FxU16 value)
{
    unsigned char r = (unsigned char)(((value >> 11) & 31) * 255 / 31);
    unsigned char g = (unsigned char)(((value >> 5) & 63) * 255 / 63);
    unsigned char b = (unsigned char)((value & 31) * 255 / 31);
    FxU16 swapped;
    switch(format) {
    case PIXFMT_ARGB32: pixel[0] = 255; pixel[1] = r; pixel[2] = g; pixel[3] = b; break;
    case PIXFMT_BGRA32: pixel[0] = b; pixel[1] = g; pixel[2] = r; pixel[3] = 255; break;
    case PIXFMT_RGBA32: pixel[0] = r; pixel[1] = g; pixel[2] = b; pixel[3] = 255; break;
    case PIXFMT_RGB24:  pixel[0] = r; pixel[1] = g; pixel[2] = b; break;
    case PIXFMT_BGR24:  pixel[0] = b; pixel[1] = g; pixel[2] = r; break;
    case PIXFMT_BGR16:
    case PIXFMT_BGR16PC:
        swapped = rgb_to_565(b, g, r);
        if(format == PIXFMT_BGR16PC) {
            pixel[0] = (unsigned char)swapped; pixel[1] = (unsigned char)(swapped >> 8);
        } else {
            pixel[0] = (unsigned char)(swapped >> 8); pixel[1] = (unsigned char)swapped;
        }
        break;
    case PIXFMT_RGB16PC:
        pixel[0] = (unsigned char)value; pixel[1] = (unsigned char)(value >> 8); break;
    case PIXFMT_RGB16:
    default:
        pixel[0] = (unsigned char)(value >> 8); pixel[1] = (unsigned char)value; break;
    }
}

static int bitmap_bytes_per_pixel(void)
{
    if(lfb_info.depth > 24) return 4;
    if(lfb_info.depth > 16) return 3;
    return 2;
}

#ifndef DETHRACE_AMIGA_SHARED_MINIGL
static fx_mgl_cosmos_context *cosmos_context(void)
{
    return (fx_mgl_cosmos_context *)(void *)mini_CurrentContext;
}

static struct BitMap *current_back_bitmap(void)
{
    fx_mgl_cosmos_context *context = cosmos_context();
    if(context == NULL)
        return NULL;
    if(context->w3dBitMap != NULL)
        return context->w3dBitMap;
    if(context->numBuffers > 0 && context->bufNr >= 0 &&
        context->bufNr < context->numBuffers && context->buffers[context->bufNr] != NULL)
        return context->buffers[context->bufNr]->sb_BitMap;
    return NULL;
}
#endif

static FxBool lock_back_buffer(void)
{
    struct BitMap *bitmap;
    ULONG base = 0;
    ULONG pitch = 0;

    memset(&lfb_info, 0, sizeof(lfb_info));
#ifndef DETHRACE_AMIGA_SHARED_MINIGL
    bitmap = current_back_bitmap();
    if(bitmap != NULL) {
        bitmap_lock_handle = LockBitMapTags(bitmap,
            LBMI_BASEADDRESS, (ULONG)&base,
            LBMI_BYTESPERROW, (ULONG)&pitch,
            TAG_DONE);
        if(bitmap_lock_handle != NULL && base != 0 && pitch != 0) {
            lfb_info.base_address = (void *)base;
            lfb_info.pitch = pitch;
            lfb_info.width = GetCyberMapAttr(bitmap, CYBRMATTR_WIDTH);
            lfb_info.height = GetCyberMapAttr(bitmap, CYBRMATTR_HEIGHT);
            lfb_info.depth = GetCyberMapAttr(bitmap, CYBRMATTR_DEPTH);
            lfb_info.pixel_format = GetCyberMapAttr(bitmap, CYBRMATTR_PIXFMT);
            return FXTRUE;
        }
        if(bitmap_lock_handle != NULL) {
            UnLockBitMap(bitmap_lock_handle);
            bitmap_lock_handle = NULL;
        }
    }
#else
    (void)bitmap;
    (void)base;
    (void)pitch;
#endif

    /* Fallback for MiniGL implementations which do not expose a lockable
     * CyberGraphX bitmap. */
    if(!mglLockBack(&lfb_info) || lfb_info.base_address == NULL)
        return FXFALSE;

    /* The Warp3D context used by the cosmos MiniGL build leaves its geometry
     * fields at zero.  The selected mode is tightly packed RGB565. */
    if(lfb_info.width == 0) lfb_info.width = screen_width;
    if(lfb_info.height == 0) lfb_info.height = screen_height;
    if(lfb_info.depth == 0) lfb_info.depth = 16;
    if(lfb_info.pitch == 0) lfb_info.pitch = screen_width * 2;
    return FXTRUE;
}

static void unlock_back_buffer(void)
{
    if(bitmap_lock_handle != NULL) {
        UnLockBitMap(bitmap_lock_handle);
        bitmap_lock_handle = NULL;
    } else {
        mglUnlockDisplay();
    }
}

static void capture_back_buffer(void)
{
    int x, y, width, bytes_per_pixel;
    flush_triangle_batch();
    glFinish();
    if(!lock_back_buffer())
        return;
    bytes_per_pixel = bitmap_bytes_per_pixel();
    width = screen_width;
    if(width > (int)lfb_info.width) width = lfb_info.width;
    if(width * bytes_per_pixel > (int)lfb_info.pitch)
        width = lfb_info.pitch / bytes_per_pixel;
    for(y = 0; y < screen_height; y++) {
        FxU16 *dst = lfb_colour + y * FXA_LFB_STRIDE_PIXELS;
        const unsigned char *src = (const unsigned char *)lfb_info.base_address + y * lfb_info.pitch;
        for(x = 0; x < width; x++)
            dst[x] = read_bitmap_pixel(src + x * bytes_per_pixel, lfb_info.pixel_format);
        memcpy(lfb_original + y * FXA_LFB_STRIDE_PIXELS, dst,
            (size_t)screen_width * sizeof(FxU16));
    }
    unlock_back_buffer();
}

static void commit_lfb_changes(void)
{
    int x, y, width, bytes_per_pixel;
    if(lfb_colour == NULL || lfb_original == NULL)
        return;

    /* DirectLock can span a complete frame.  Do not keep Warp3D locked while
     * MiniGL renders, and do not copy the whole stale CPU image over the 3D
     * result.  Merge only pixels which changed while the LFB was exposed. */
    flush_triangle_batch();
    glFinish();
    if(!lock_back_buffer())
        return;
    bytes_per_pixel = bitmap_bytes_per_pixel();
    width = screen_width;
    if(width > (int)lfb_info.width) width = lfb_info.width;
    if(width * bytes_per_pixel > (int)lfb_info.pitch)
        width = lfb_info.pitch / bytes_per_pixel;
    for(y = 0; y < screen_height; y++) {
        unsigned char *dst = (unsigned char *)lfb_info.base_address + y * lfb_info.pitch;
        FxU16 *src = lfb_colour + y * FXA_LFB_STRIDE_PIXELS;
        FxU16 *old = lfb_original + y * FXA_LFB_STRIDE_PIXELS;
        for(x = 0; x < width; x++) {
            if(src[x] != old[x]) {
                write_bitmap_pixel(dst + x * bytes_per_pixel, lfb_info.pixel_format, src[x]);
            }
        }
    }
    unlock_back_buffer();
}

static void update_lfb_overlay_from_operation(void)
{
    int x, y;
    int scan_min_x = lfb_operation_region_valid ? lfb_operation_min_x : 0;
    int scan_min_y = lfb_operation_region_valid ? lfb_operation_min_y : 0;
    int scan_max_x = lfb_operation_region_valid ? lfb_operation_max_x : screen_width - 1;
    int scan_max_y = lfb_operation_region_valid ? lfb_operation_max_y : screen_height - 1;
    int min_x = scan_max_x + 1, min_y = scan_max_y + 1;
    int max_x = scan_min_x - 1, max_y = scan_min_y - 1;
    unsigned long dirty = 0;
    unsigned long zero_writes = 0;
    FxBool large_zero_clear;

    if(lfb_colour == NULL || lfb_lock_snapshot == NULL ||
        lfb_overlay_colour == NULL || lfb_overlay_mask == NULL)
        return;

    for(y = scan_min_y; y <= scan_max_y; y++) {
        FxU16 *src = lfb_colour + y * FXA_LFB_STRIDE_PIXELS;
        FxU16 *old = lfb_lock_snapshot + y * FXA_LFB_STRIDE_PIXELS;
        for(x = scan_min_x; x <= scan_max_x; x++) {
            if(src[x] != old[x]) {
                dirty++;
                if(src[x] == 0) zero_writes++;
                if(x < min_x) min_x = x;
                if(x > max_x) max_x = x;
                if(y < min_y) min_y = y;
                if(y > max_y) max_y = y;
            }
        }
    }

    large_zero_clear = dirty != 0 && dirty == zero_writes &&
        max_x - min_x + 1 >= screen_width * 3 / 4 &&
        max_y - min_y + 1 >= screen_height * 3 / 4;

    if(large_zero_clear) {
        /* A screen-sized black LFB fill starts a new 3D view.  It clears old
         * interface pixels; it is not itself an opaque black HUD layer. */
        memset(lfb_overlay_mask, 0,
            (size_t)FXA_LFB_STRIDE_PIXELS * screen_height);
    } else if(dirty != 0) {
        for(y = min_y; y <= max_y; y++) {
            FxU16 *src = lfb_colour + y * FXA_LFB_STRIDE_PIXELS;
            FxU16 *old = lfb_lock_snapshot + y * FXA_LFB_STRIDE_PIXELS;
            FxU16 *overlay = lfb_overlay_colour + y * FXA_LFB_STRIDE_PIXELS;
            FxU8 *mask = lfb_overlay_mask + y * FXA_LFB_STRIDE_PIXELS;
            for(x = min_x; x <= max_x; x++) {
                if(src[x] != old[x]) {
                    overlay[x] = src[x];
                    mask[x] = FXA_HUD_MASK_THIS_FRAME;
                }
            }
        }
    }
}

static void restore_glide_state(void)
{
    if(state_texture_enabled) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, state_bound_texture);
    /* draw_lfb_layer() forces GL_NEAREST/GL_CLAMP on its tile textures; those
     * are per-object in GL, so the app's texture needs its own back. */
    apply_texture_parameters();
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE,
        colour_combine == GR_COLORCOMBINE_DECAL_TEXTURE ? GL_REPLACE : GL_MODULATE);
    if(state_blend_enabled) {
        glEnable(GL_BLEND);
        glBlendFunc(state_blend_src, state_blend_dst);
    } else glDisable(GL_BLEND);
    if(chromakey_enabled) {
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.0f);
    } else glDisable(GL_ALPHA_TEST);
    if(state_depth_enabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask(state_depth_mask ? GL_TRUE : GL_FALSE);
    if(state_cull_enabled) {
        glEnable(GL_CULL_FACE);
        glFrontFace(state_cull_mode == GR_CULL_NEGATIVE ? GL_CW : GL_CCW);
        glCullFace(GL_BACK);
    } else glDisable(GL_CULL_FACE);
    if(state_fog_enabled) glEnable(GL_FOG); else glDisable(GL_FOG);
}

static void draw_lfb_layer(FxBool opaque)
{
    int tile_x, tile_y, x, y;
    int texture_index = 0;
    size_t tile_pixel_count =
        (size_t)FXA_LFB_TILE_SIZE * FXA_LFB_TILE_SIZE;
    flush_triangle_batch();
    if(lfb_colour == NULL || lfb_overlay_colour == NULL || lfb_overlay_mask == NULL)
        return;
    if(!lfb_tiles_created) {
        int tile_columns =
            (screen_width + FXA_LFB_TILE_SIZE - 1) / FXA_LFB_TILE_SIZE;
        int tile_rows =
            (screen_height + FXA_LFB_TILE_SIZE - 1) / FXA_LFB_TILE_SIZE;
        lfb_active_tile_count = tile_columns * tile_rows;
        if(lfb_active_tile_count > FXA_LFB_TILE_COUNT)
            return;
        lfb_tile_cache = calloc((size_t)lfb_active_tile_count,
            tile_pixel_count * sizeof(*lfb_tile_cache));
        if(lfb_tile_cache == NULL)
            return;
        glGenTextures(lfb_active_tile_count, lfb_tile_textures);
        for(texture_index = 0; texture_index < lfb_active_tile_count; texture_index++) {
            glBindTexture(GL_TEXTURE_2D, lfb_tile_textures[texture_index]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                FXA_LFB_TILE_SIZE, FXA_LFB_TILE_SIZE, 0,
                GL_RGBA, GL_UNSIGNED_BYTE,
                lfb_tile_cache + (size_t)texture_index * tile_pixel_count);
        }
        texture_index = 0;
        lfb_tiles_created = FXTRUE;
    }

    /* UAE can present Warp3D as a host-side plane, separate from the RTG
     * bitmap.  Upload direct framebuffer rendering as MiniGL tiles so 2D and
     * 3D are composed in the same plane on both Direct3D 9 and 11 hosts. */
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_FOG);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glDisable(GL_BLEND);
    if(opaque) {
        glDisable(GL_ALPHA_TEST);
    } else {
        /* The HUD mask is binary.  Alpha blending in the old cosmos MiniGL
         * can leave zero-alpha texels opaque, hiding the world with black
         * 256x256 tiles.  Alpha testing discards those texels outright. */
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.5f);
    }
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    for(tile_y = 0; tile_y < screen_height; tile_y += FXA_LFB_TILE_SIZE) {
        int tile_height = screen_height - tile_y;
        if(tile_height > FXA_LFB_TILE_SIZE) tile_height = FXA_LFB_TILE_SIZE;
        for(tile_x = 0; tile_x < screen_width; tile_x += FXA_LFB_TILE_SIZE) {
            int tile_width = screen_width - tile_x;
            FxBool has_pixels = opaque;
            FxBool tile_changed = FXFALSE;
            FxU32 *tile_cache = lfb_tile_cache +
                (size_t)texture_index * tile_pixel_count;
            if(tile_width > FXA_LFB_TILE_SIZE) tile_width = FXA_LFB_TILE_SIZE;
            for(y = 0; y < tile_height; y++) {
                FxU16 *src = (opaque ? lfb_colour : lfb_overlay_colour) +
                    (tile_y + y) * FXA_LFB_STRIDE_PIXELS + tile_x;
                FxU8 *mask = lfb_overlay_mask +
                    (tile_y + y) * FXA_LFB_STRIDE_PIXELS + tile_x;
                FxU32 *dst = tile_cache + y * FXA_LFB_TILE_SIZE;
                for(x = 0; x < tile_width; x++) {
                    FxU8 mark = mask[x];
                    FxBool changed = opaque || mark != 0;
                    FxU32 rgba = 0;
                    if(changed) {
                        FxU16 pixel = src[x];
                        unsigned int r = (pixel >> 11) & 31;
                        unsigned int g = (pixel >> 5) & 63;
                        unsigned int b = pixel & 31;
                        /* 68k is big-endian, so this word is stored in the
                         * RGBA byte order expected by MiniGL. */
                        rgba = (((r << 3) | (r >> 2)) << 24) |
                            (((g << 2) | (g >> 4)) << 16) |
                            (((b << 3) | (b >> 2)) << 8) | 255;
                        has_pixels = FXTRUE;
                    }
                    if(dst[x] != rgba) {
                        dst[x] = rgba;
                        tile_changed = FXTRUE;
                    }

                    /* Retire the overlay while its mask row is already hot in
                     * cache, instead of scanning the full screen again after
                     * the buffer switch. */
                    if(mark == FXA_HUD_MASK_THIS_FRAME)
                        mask[x] = opaque ? FXA_HUD_MASK_CARRIED : 0;
                    else if(!opaque && mark == FXA_HUD_MASK_CARRIED)
                        mask[x] = 0;
                }
            }
            if(has_pixels && texture_index < FXA_LFB_TILE_COUNT) {
                float u = tile_width / (float)FXA_LFB_TILE_SIZE;
                float v = tile_height / (float)FXA_LFB_TILE_SIZE;
                glBindTexture(GL_TEXTURE_2D, lfb_tile_textures[texture_index]);
                /* The cosmos MiniGL build corrupts its render region after
                 * GLTexSubImage2D.  Keep glTexImage2D, but avoid redefining a
                 * tile whose RGBA contents are identical to the texture
                 * already resident in MiniGL. */
                if(tile_changed)
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                        FXA_LFB_TILE_SIZE, FXA_LFB_TILE_SIZE, 0,
                        GL_RGBA, GL_UNSIGNED_BYTE, tile_cache);
                glBegin(GL_QUADS);
                glTexCoord2f(0.0f, 0.0f); glVertex3f(tile_x, tile_y, 0.0f);
                glTexCoord2f(u, 0.0f); glVertex3f(tile_x + tile_width, tile_y, 0.0f);
                glTexCoord2f(u, v); glVertex3f(tile_x + tile_width, tile_y + tile_height, 0.0f);
                glTexCoord2f(0.0f, v); glVertex3f(tile_x, tile_y + tile_height, 0.0f);
                glEnd();
            }
            texture_index++;
        }
    }
    restore_glide_state();
}

/* Carmageddon's TAB map is composed in a different order from the in-race
 * HUD: first an opaque CPU-rendered map image, then MiniGL geometry on top.
 * The original 3dfx path used BrPixelmapFlush() at that boundary, but the
 * legacy driver implements flush as a no-op.  Present the logical LFB now so
 * the following dim rectangle and 3D map view are layered over it, rather
 * than treating the map as a HUD overlay at buffer swap. */
void FXA_DrawLfbBackground(void)
{
    flush_triangle_batch();
    if(lfb_overlay_mask != NULL)
        memset(lfb_overlay_mask, 0,
            (size_t)FXA_LFB_STRIDE_PIXELS * screen_height);
    draw_lfb_layer(FXTRUE);
    /* The opaque upload covers the complete target, so the normal first-GL-
     * primitive clear must not erase it again. */
    frame_surface_cleared = FXTRUE;
}

static void prepare_lfb_for_gl(void)
{
    if(lfb_lock_depth <= 0 || lfb_precommitted ||
        lfb_colour == NULL || lfb_original == NULL)
        return;
    /* LFB composition is deferred until swap.  Uploading here can be erased by
     * a later clear and causes multiple full texture uploads per frame. */
    lfb_precommitted = 1;
}

static FxBool vertex_has_screen_depth(const GrVertex *v)
{
    /* BRender's 2D pixelmap/stretch path assigns this exact constant to all
     * vertices.  Perspective 3D primitives carry the projected depth. */
    return v->ooz > 65529.0f && v->ooz < 65531.0f;
}

static void prepare_frame_surface(void)
{
    FxBool saved_depth_mask;
    if(frame_surface_cleared || !previous_frame_had_3d)
        return;

    /* Clear at the first GL primitive of the new world frame.  Some HUD
     * rectangles are submitted before the first 3D triangle, so clearing in
     * prepare_lfb_for_3d() erased them.  This matches the working Wipeout
     * MiniGL renderer, which clears before any 2D or 3D drawing. */
    saved_depth_mask = state_depth_mask;
    glDepthMask(GL_TRUE);
    glClearDepth(1.0);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDepthMask(saved_depth_mask ? GL_TRUE : GL_FALSE);
    frame_depth_cleared = FXTRUE;
    frame_surface_cleared = FXTRUE;
}

static void prepare_lfb_for_3d(void)
{
    FxBool saved_depth_mask;
    if(!previous_frame_had_3d && !frame_overlay_reset) {
        /* A 2D menu and the in-race HUD share the logical LFB.  Drop every
         * persistent menu pixel at the exact transition to 3D; HUD writes
         * performed later in this frame will populate the overlay again. */
        if(lfb_overlay_mask != NULL)
            memset(lfb_overlay_mask, 0,
                (size_t)FXA_LFB_STRIDE_PIXELS * screen_height);
        /* Remove the last opaque menu image once, before entering the world.
         * Clearing colour on every 3D frame can erase HUD elements which the
         * game draws before its first world primitive. */
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        frame_overlay_reset = FXTRUE;
    }
    if(!frame_depth_cleared) {
        /* Carmageddon clears the 3dfx depth pixelmap through an LFB rectangle.
         * Our RAM staging buffer is not Warp3D's depth buffer, so clear the
         * real GL depth buffer once at the start of every world frame. */
        saved_depth_mask = state_depth_mask;
        glDepthMask(GL_TRUE);
        glClearDepth(1.0);
        glClear(GL_DEPTH_BUFFER_BIT);
        glDepthMask(saved_depth_mask ? GL_TRUE : GL_FALSE);
        frame_depth_cleared = FXTRUE;
    }
    if(lfb_lock_depth <= 0 || lfb_3d_prepared ||
        lfb_colour == NULL || lfb_original == NULL)
        return;
    lfb_precommitted = 1;
    lfb_3d_prepared = 1;
}

static GLenum compare_function(GrCmpFnc_t fn)
{
    static const GLenum values[] = {
        GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL,
        GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS
    };
    return values[fn & 7];
}

static GLenum blend_function(GrAlphaBlendFnc_t fn)
{
    switch(fn) {
    case GR_BLEND_ZERO:                return GL_ZERO;
    case GR_BLEND_SRC_ALPHA:           return GL_SRC_ALPHA;
    case GR_BLEND_SRC_COLOR:           return GL_SRC_COLOR;
    case GR_BLEND_DST_ALPHA:           return GL_DST_ALPHA;
    case GR_BLEND_ONE:                 return GL_ONE;
    case GR_BLEND_ONE_MINUS_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
    case GR_BLEND_ONE_MINUS_SRC_COLOR: return GL_ONE_MINUS_SRC_COLOR;
    case GR_BLEND_ONE_MINUS_DST_ALPHA: return GL_ONE_MINUS_DST_ALPHA;
    default:                           return GL_ONE;
    }
}

static void resolution_size(GrScreenResolution_t resolution, int *width, int *height)
{
    static const int sizes[][2] = {
        {320, 200}, {320, 240}, {400, 256}, {512, 384},
        {640, 200}, {640, 350}, {640, 400}, {640, 480},
        {800, 600}, {960, 720}, {856, 480}, {512, 256}
    };
    if(resolution >= 0 && resolution < (int)(sizeof(sizes) / sizeof(sizes[0]))) {
        *width = sizes[resolution][0];
        *height = sizes[resolution][1];
    }
}

static void texture_size(const GrTexInfo *info, int *width, int *height)
{
    int large = 256 >> info->largeLod;
    *width = large;
    *height = large;
    if(info->aspectRatio < GR_ASPECT_1x1)
        *height >>= GR_ASPECT_1x1 - info->aspectRatio;
    else if(info->aspectRatio > GR_ASPECT_1x1)
        *width >>= info->aspectRatio - GR_ASPECT_1x1;
    if(*width < 1) *width = 1;
    if(*height < 1) *height = 1;
}

static int texture_bytes(GrTextureFormat_t format)
{
    return format < GR_TEXFMT_16BIT ? 1 : 2;
}

static FxU32 texture_level_size(GrLOD_t lod, GrAspectRatio_t aspect, GrTextureFormat_t format)
{
    GrTexInfo info;
    int width, height;
    info.largeLod = lod;
    info.aspectRatio = aspect;
    texture_size(&info, &width, &height);
    return (FxU32)(width * height * texture_bytes(format));
}

static fx_amiga_texture *find_texture(FxU32 address, FxBool create)
{
    int i;
    fx_amiga_texture *free_slot = NULL;
    for(i = 0; i < FXA_MAX_TEXTURES; i++) {
        if(textures[i].used && textures[i].address == address)
            return &textures[i];
        if(!textures[i].used && free_slot == NULL)
            free_slot = &textures[i];
    }
    if(create && free_slot != NULL) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->used = FXTRUE;
        free_slot->address = address;
        glGenTextures(1, &free_slot->name);
        return free_slot;
    }
    return NULL;
}

static void unpack_colour(GrColor_t colour, GLubyte *rgba)
{
    rgba[0] = (GLubyte)((colour >> 16) & 0xff);
    rgba[1] = (GLubyte)((colour >> 8) & 0xff);
    rgba[2] = (GLubyte)(colour & 0xff);
    rgba[3] = (GLubyte)((colour >> 24) & 0xff);
}

static GLubyte *convert_texture(const GrTexInfo *info, int width, int height)
{
    int i, count = width * height;
    GLubyte *out = malloc((size_t)count * 4);
    if(out == NULL)
        return NULL;

    if(info->format == GR_TEXFMT_P_8) {
        const FxU8 *src = (const FxU8 *)info->data;
        for(i = 0; i < count; i++) {
            GrColor_t c = palette[src[i]];
            unpack_colour(c, out + i * 4);
            /* Keep the key colour encoded in the texture independently of
             * the current render state.  Set3DfxState() uploads a texture
             * before it enables chromakey, so baking chromakey_enabled into
             * alpha made the power-of-two padding alternately transparent or
             * opaque black depending on which primitive preceded the upload.
             * Alpha testing still controls whether this stored alpha is used. */
            out[i * 4 + 3] =
                (c & 0xffffffUL) == (chromakey & 0xffffffUL) ? 0 : 255;
        }
    } else {
        const FxU16 *src = (const FxU16 *)info->data;
        for(i = 0; i < count; i++) {
            FxU16 p = src[i];
            if(info->format == GR_TEXFMT_RGB_565) {
                out[i * 4 + 0] = (GLubyte)(((p >> 11) & 31) * 255 / 31);
                out[i * 4 + 1] = (GLubyte)(((p >> 5) & 63) * 255 / 63);
                out[i * 4 + 2] = (GLubyte)((p & 31) * 255 / 31);
                out[i * 4 + 3] = p == (FxU16)chromakey ? 0 : 255;
            } else if(info->format == GR_TEXFMT_ARGB_4444) {
                out[i * 4 + 0] = (GLubyte)(((p >> 8) & 15) * 17);
                out[i * 4 + 1] = (GLubyte)(((p >> 4) & 15) * 17);
                out[i * 4 + 2] = (GLubyte)((p & 15) * 17);
                out[i * 4 + 3] = p == (FxU16)chromakey
                    ? 0 : (GLubyte)(((p >> 12) & 15) * 17);
            } else {
                out[i * 4 + 0] = (GLubyte)(((p >> 10) & 31) * 255 / 31);
                out[i * 4 + 1] = (GLubyte)(((p >> 5) & 31) * 255 / 31);
                out[i * 4 + 2] = (GLubyte)((p & 31) * 255 / 31);
                out[i * 4 + 3] = chromakey_enabled && p == (FxU16)chromakey
                    ? 0 : 255;
            }
        }
    }
    return out;
}

static FxBool uses_texture(void)
{
    return colour_combine == GR_COLORCOMBINE_DECAL_TEXTURE ||
        colour_combine == GR_COLORCOMBINE_TEXTURE_TIMES_CCRGB ||
        colour_combine == GR_COLORCOMBINE_TEXTURE_TIMES_ITRGB ||
        colour_combine == GR_COLORCOMBINE_TEXTURE_TIMES_ITRGB_DELTA0 ||
        colour_combine == GR_COLORCOMBINE_TEXTURE_TIMES_ITRGB_ADD_ALPHA;
}

static FxBool uses_iterated_colour(void)
{
    /* The _DELTA0 variants are the *flat* shading modes: rendfunc.c's
     * TriangleRender_Flat() fills only x/y/ooz and passes the colour through
     * grConstantColorValue4(), leaving GrVertex.r/g/b uninitialised stack
     * garbage.  Only the smooth paths (TriangleRender_Smooth and friends)
     * populate the per-vertex colours.
     *
     * Treating DELTA0 as iterated made emit_vertex() read that garbage -- the
     * HUD's dim rectangle is drawn flat-shaded, so its "black, 50% alpha" quad
     * came out tinted with whatever was on the stack.  That is the green HUD
     * background, and it also explains the NaN vertex colours seen in the
     * diagnostic log. */
    return colour_combine == GR_COLORCOMBINE_ITRGB ||
        colour_combine == GR_COLORCOMBINE_TEXTURE_TIMES_ITRGB ||
        colour_combine == GR_COLORCOMBINE_TEXTURE_TIMES_ITRGB_ADD_ALPHA;
}

/* Force GL texturing to match what the current colour-combine mode actually
 * wants, immediately before a primitive is emitted.
 *
 * grDrawTriangle() draws with whatever GL state happens to be current, but the
 * decision to texture lives in colour_combine.  Any path that leaves
 * GL_TEXTURE_2D enabled behind BRender's back -- the LFB overlay's tile blits,
 * the 2D stretch blit in devpixmp.c, or match.c's cached state skipping a
 * redundant guColorCombineFunction() call -- then let untextured world geometry
 * sample whatever was last bound.  That is how the HUD's face texture ended up
 * stretched across the sky and buildings. */
static void sync_texture_state(void)
{
    if(uses_texture() && state_bound_texture != 0) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, state_bound_texture);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE,
            colour_combine == GR_COLORCOMBINE_DECAL_TEXTURE ? GL_REPLACE : GL_MODULATE);
    } else {
        /* Also unbind, not just disable.  The HUD's dim rectangle is an
         * untextured black quad drawn at 50% alpha (gDim_material, colour 0 +
         * BRT_BLEND_B); if a texture object is still bound while GL_MODULATE is
         * active, MiniGL can sample it anyway and the panel background comes out
         * tinted -- which is where the green HUD background came from. */
        glDisable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

static void emit_vertex(const GrVertex *v)
{
    float alpha;
    alpha = constant_rgba[3];
    if(alpha_source == GR_ALPHASOURCE_TEXTURE_ALPHA)
        alpha = 1.0f;
    else if(alpha_source == GR_ALPHASOURCE_ITERATED_ALPHA)
        alpha = v->a / 255.0f;

    /* BRender only refreshes the constant colour when it turns blending ON
     * (3dfx_dos/match.c).  Going back to an opaque primitive just selects
     * GR_BLEND_ONE/GR_BLEND_ZERO and leaves the old alpha_val in the constant
     * colour.  Real Glide discards it in the blend unit; MiniGL still
     * modulates it into the framebuffer, so every opaque object stayed as
     * translucent as the last blended one.  Blending off means fully opaque. */
    if(!state_blend_enabled)
        alpha = 1.0f;

    if(uses_iterated_colour())
        glColor4f(v->r / 255.0f, v->g / 255.0f, v->b / 255.0f, alpha);
    else
        glColor4f(constant_rgba[0], constant_rgba[1], constant_rgba[2], alpha);

    if(uses_texture())
        glTexCoord4f(v->tmuvtx[0].sow * texture_inv_width,
            v->tmuvtx[0].tow * texture_inv_height, 0.0f, v->oow);
    /* Keep eye-space distance monotonic for MiniGL's fixed-function fog.
     * Together with the 0..1 orthographic depth range below this produces
     * exactly the same depth-buffer value as the old +1..-1 mapping, while
     * avoiding abs(Z) fogging both the near and far planes to black. */
    glVertex3f(v->x, v->y, -(v->ooz / 65535.0f));
}

static void flush_triangle_batch(void)
{
    if(!triangle_batch_open)
        return;
    glEnd();
    triangle_batch_open = FXFALSE;
    triangle_batch_vertex_count = 0;
}

void grDrawTriangle(const GrVertex *a, const GrVertex *b, const GrVertex *c)
{
    FxBool is_3d = !(vertex_has_screen_depth(a) &&
        vertex_has_screen_depth(b) && vertex_has_screen_depth(c));

    /* These first-frame transitions can clear GL buffers.  Close a preceding
     * 2D batch before allowing them to issue GL calls. */
    if((!frame_surface_cleared && previous_frame_had_3d) ||
        (is_3d && (!frame_depth_cleared ||
            (!previous_frame_had_3d && !frame_overlay_reset))))
        flush_triangle_batch();

    prepare_frame_surface();
    if(is_3d) {
        frame_3d_triangles++;
        prepare_lfb_for_3d();
    } else {
        prepare_lfb_for_gl();
    }

    if(triangle_batch_vertex_count + 3 > FXA_IMMEDIATE_BATCH_VERTICES)
        flush_triangle_batch();
    if(!triangle_batch_open) {
        sync_texture_state();
        glBegin(GL_TRIANGLES);
        triangle_batch_open = FXTRUE;
    }
    emit_vertex(a); emit_vertex(b); emit_vertex(c);
    triangle_batch_vertex_count += 3;
}

void grDrawLine(const GrVertex *a, const GrVertex *b)
{
    FxBool is_3d = !(vertex_has_screen_depth(a) && vertex_has_screen_depth(b));
    flush_triangle_batch();
    prepare_frame_surface();
    if(is_3d) {
        frame_3d_lines++;
        prepare_lfb_for_3d();
    } else {
        prepare_lfb_for_gl();
    }
    sync_texture_state();
    glBegin(GL_LINES);
    emit_vertex(a); emit_vertex(b);
    glEnd();
}

void grDrawPoint(const GrVertex *a)
{
    FxBool is_3d = !vertex_has_screen_depth(a);
    flush_triangle_batch();
    prepare_frame_surface();
    if(is_3d) {
        frame_3d_points++;
        prepare_lfb_for_3d();
    } else {
        prepare_lfb_for_gl();
    }
    sync_texture_state();
    glBegin(GL_POINTS);
    emit_vertex(a);
    glEnd();
}

void grBufferClear(GrColor_t colour, GrAlpha_t alpha, FxU16 depth)
{
    GLubyte c[4];
    GLbitfield bits = 0;
    flush_triangle_batch();
    unpack_colour(colour, c);
    glClearColor(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, alpha / 255.0f);
    glClearDepth(depth / 65535.0);
    if(state_colour_mask_rgb) bits |= GL_COLOR_BUFFER_BIT;
    if(state_depth_mask) bits |= GL_DEPTH_BUFFER_BIT;
    if(bits != 0) glClear(bits);
}

int grBufferNumPending(void) { return 0; }
void FXA_BeginWorldFrame(void)
{
    /* The TAB map draws its dimmed 2D preview frame through BRender before
     * the live 3D view.  MiniGL can classify that parallel-camera rectangle
     * as geometry and let it populate the real depth buffer.  Start the live
     * world with a fresh depth buffer so the rectangle cannot occlude every
     * preview polygon.  Close its batch before the deferred clear. */
    flush_triangle_batch();
    frame_world_rendered = FXTRUE;
    frame_depth_cleared = FXFALSE;
}
void grBufferSwap(int interval)
{
    FxBool has_3d = frame_world_rendered || frame_3d_triangles != 0 ||
        frame_3d_lines != 0 || frame_3d_points != 0;
    (void)interval;
    flush_triangle_batch();
    if(lfb_colour != NULL)
        draw_lfb_layer(has_3d ? FXFALSE : FXTRUE);
    glFlush();
    mglSwitchDisplay();
    frame_3d_triangles = 0;
    frame_3d_lines = 0;
    frame_3d_points = 0;
    frame_world_rendered = FXFALSE;
    frame_depth_cleared = FXFALSE;
    frame_overlay_reset = FXFALSE;
    frame_surface_cleared = FXFALSE;
    previous_frame_had_3d = has_3d;
}
void grRenderBuffer(GrBuffer_t buffer) { flush_triangle_batch(); render_buffer = buffer; }
void grErrorSetCallback(GrErrorCallbackFnc_t fnc) { error_callback = fnc; }

FxBool grSstOpen(GrScreenResolution_t resolution, GrScreenRefresh_t refresh,
    GrColorFormat_t format, GrOriginLocation_t origin,
    GrSmoothingMode_t smoothing, int buffers)
{
    int viewport_y = 0;
    int scissor_y = 0;
    (void)refresh; (void)format; (void)origin; (void)smoothing; (void)buffers;
    resolution_size(resolution, &screen_width, &screen_height);
    {
        struct Window *window = (struct Window *)mglGetWindowHandle();
        if(window != NULL) {
        int inner_height = window->Height - window->BorderTop - window->BorderBottom;
        viewport_y = inner_height - screen_height;
        /* GLScissor in the cosmos MiniGL uses the outer Window->Height in
         * top = height - y - scissor_height.  Supplying this Y compensation
         * makes its private and Warp3D scissor state both resolve to top=0. */
        scissor_y = window->Height - screen_height;
        }
    }
    glDisable(GL_SCISSOR_TEST);
    /* This MiniGL build derives OpenGL's bottom-origin Y offset from the
     * Intuition window's inner height.  Compensate when the requested render
     * surface is taller than that window instead of losing most of the frame. */
    glViewport(0, viewport_y, screen_width, screen_height);
    glScissor(0, scissor_y, screen_width, screen_height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, screen_width, screen_height, 0.0, 0.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    return FXTRUE;
}

FxBool grSstQueryHardware(GrHwConfiguration *hw)
{
    memset(hw, 0, sizeof(*hw));
    hw->num_sst = 1;
    hw->SSTs[0].type = GR_SSTTYPE_VOODOO;
    hw->SSTs[0].sstBoard.VoodooConfig.fbRam = 4;
    hw->SSTs[0].sstBoard.VoodooConfig.nTexelfx = 1;
    hw->SSTs[0].sstBoard.VoodooConfig.tmuConfig[0].tmuRam = 16;
    return FXTRUE;
}

void grSstSelect(int which) { (void)which; }
void grGlideInit(void) { memset(textures, 0, sizeof(textures)); }
void grGlideShutdown(void)
{
    int i;
    flush_triangle_batch();
    for(i = 0; i < FXA_MAX_TEXTURES; i++)
        if(textures[i].used) glDeleteTextures(1, &textures[i].name);
    if(lfb_tiles_created) glDeleteTextures(lfb_active_tile_count, lfb_tile_textures);
    lfb_tiles_created = FXFALSE;
    lfb_active_tile_count = 0;
    free(lfb_tile_cache); lfb_tile_cache = NULL;
    free(lfb_colour); lfb_colour = NULL;
    free(lfb_original); lfb_original = NULL;
    free(lfb_lock_snapshot); lfb_lock_snapshot = NULL;
    free(lfb_depth); lfb_depth = NULL;
    free(lfb_overlay_colour); lfb_overlay_colour = NULL;
    free(lfb_overlay_mask); lfb_overlay_mask = NULL;
    lfb_operation_region_valid = FXFALSE;
    lfb_operation_read_only = FXFALSE;
}

void grAlphaBlendFunction(GrAlphaBlendFnc_t rgb_sf, GrAlphaBlendFnc_t rgb_df,
    GrAlphaBlendFnc_t alpha_sf, GrAlphaBlendFnc_t alpha_df)
{
    GLenum new_src = blend_function(rgb_sf);
    GLenum new_dst = blend_function(rgb_df);
    FxBool new_enabled = !(rgb_sf == GR_BLEND_ONE && rgb_df == GR_BLEND_ZERO);
    (void)alpha_sf; (void)alpha_df;
    if(state_blend_src == new_src && state_blend_dst == new_dst &&
        state_blend_enabled == new_enabled)
        return;
    flush_triangle_batch();
    state_blend_src = new_src;
    state_blend_dst = new_dst;
    state_blend_enabled = new_enabled;
    if(!state_blend_enabled)
        glDisable(GL_BLEND);
    else {
        glEnable(GL_BLEND);
        glBlendFunc(state_blend_src, state_blend_dst);
    }
}

void grChromakeyMode(GrChromakeyMode_t mode)
{
    FxBool enabled = mode == GR_CHROMAKEY_ENABLE;
    if(chromakey_enabled == enabled)
        return;
    flush_triangle_batch();
    chromakey_enabled = enabled;
    if(chromakey_enabled) { glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.0f); }
    else glDisable(GL_ALPHA_TEST);
}
void grChromakeyValue(GrColor_t value) { chromakey = value; }
/* This MiniGL build declares GLColorMask but does not export it.  BRender only
 * requests the normal all-colour-channels state, which is already the default. */
void grColorMask(FxBool rgb, FxBool alpha) { state_colour_mask_rgb = rgb; (void)alpha; }
void grConstantColorValue(GrColor_t value)
{
    GLubyte rgba[4];
    unpack_colour(value, rgba);
    constant_rgba[0] = rgba[0] * (1.0f / 255.0f);
    constant_rgba[1] = rgba[1] * (1.0f / 255.0f);
    constant_rgba[2] = rgba[2] * (1.0f / 255.0f);
    constant_rgba[3] = rgba[3] * (1.0f / 255.0f);
}
void grConstantColorValue4(float a, float r, float g, float b)
{
    grConstantColorValue(((FxU32)a << 24) | ((FxU32)r << 16) |
        ((FxU32)g << 8) | (FxU32)b);
}
void grCullMode(GrCullMode_t mode)
{
    if(state_cull_mode == mode)
        return;
    flush_triangle_batch();
    state_cull_mode = mode;
    state_cull_enabled = mode != GR_CULL_DISABLE;
    if(!state_cull_enabled) glDisable(GL_CULL_FACE);
    else {
        glEnable(GL_CULL_FACE);
        glFrontFace(mode == GR_CULL_NEGATIVE ? GL_CW : GL_CCW);
        glCullFace(GL_BACK);
    }
}
void grDepthBufferFunction(GrCmpFnc_t fn)
{
    static GrCmpFnc_t current = (GrCmpFnc_t)-1;
    if(current == fn)
        return;
    flush_triangle_batch();
    current = fn;
    glDepthFunc(compare_function(fn));
}
void grDepthBufferMode(GrDepthBufferMode_t mode)
{
    FxBool enabled = mode != GR_DEPTHBUFFER_DISABLE;
    if(state_depth_enabled == enabled)
        return;
    flush_triangle_batch();
    state_depth_enabled = enabled;
    if(!state_depth_enabled) glDisable(GL_DEPTH_TEST);
    else glEnable(GL_DEPTH_TEST);
}
void grDepthMask(FxBool mask)
{
    if(state_depth_mask == mask)
        return;
    flush_triangle_batch();
    state_depth_mask = mask;
    glDepthMask(mask ? GL_TRUE : GL_FALSE);
}
void grFogColorValue(GrColor_t colour)
{
    (void)colour;
}
void grFogMode(GrFogMode_t mode)
{
    /* MiniGL's fixed-function fog does not use Glide's reciprocal-W fog
     * coordinate and currently blacks out the complete Splat Pack scene.
     * Keep it disabled until the Glide fog table is emulated explicitly. */
    (void)mode;
    if(state_fog_enabled) {
        flush_triangle_batch();
        state_fog_enabled = FXFALSE;
        glDisable(GL_FOG);
    }
}
void grFogTable(const GrFog_t table[GR_FOG_TABLE_SIZE])
{
    (void)table;
}

FxU32 grTexCalcMemRequired(GrLOD_t small, GrLOD_t large, GrAspectRatio_t aspect, GrTextureFormat_t format)
{
    FxU32 size = 0;
    GrLOD_t lod;
    for(lod = large; lod <= small; lod++) size += texture_level_size(lod, aspect, format);
    return size;
}
FxU32 grTexTextureMemRequired(FxU32 mask, GrTexInfo *info)
{
    (void)mask;
    return grTexCalcMemRequired(info->smallLod, info->largeLod, info->aspectRatio, info->format);
}
FxU32 grTexMinAddress(GrChipID_t tmu) { (void)tmu; return 0; }
FxU32 grTexMaxAddress(GrChipID_t tmu) { (void)tmu; return FXA_TEXTURE_MEMORY; }
void grTexDownloadTable(GrChipID_t tmu, GrTexTable_t type, void *data)
{
    (void)tmu;
    if(type == GR_TEXTABLE_PALETTE) memcpy(palette, data, sizeof(palette));
}
void grTexDownloadMipMap(GrChipID_t tmu, FxU32 address, FxU32 mask, GrTexInfo *info)
{
    fx_amiga_texture *texture;
    GLubyte *pixels;
    int width, height;
    FxU32 span;
    flush_triangle_batch();
    (void)tmu; (void)mask;
    /* BRender's texture cache evicts and reallocates blocks inside its own
     * simulated 16MB heap, so an address we already know can come back holding
     * a different image.  We only find out here, at the upload.  Drop every
     * cached entry whose byte range this upload overwrites, or grTexSource()
     * would keep selecting the stale GL texture -- which is how the HUD's
     * scale-buffer (Max's face) ended up painted across world geometry. */
    span = grTexTextureMemRequired(GR_MIPMAPLEVELMASK_BOTH, info);
    if(span != 0) {
        int i;
        for(i = 0; i < FXA_MAX_TEXTURES; i++) {
            if(!textures[i].used || textures[i].address == address)
                continue;
            if(textures[i].size == 0)
                continue;
            if(address < textures[i].address + textures[i].size &&
                textures[i].address < address + span) {
                if(state_bound_texture == textures[i].name) {
                    state_bound_texture = 0;
                    state_texture_enabled = FXFALSE;
                    glDisable(GL_TEXTURE_2D);
                    glBindTexture(GL_TEXTURE_2D, 0);
                }
                glDeleteTextures(1, &textures[i].name);
                memset(&textures[i], 0, sizeof(textures[i]));
            }
        }
    }
    texture = find_texture(address, FXTRUE);
    if(texture == NULL) {
        if(error_callback) error_callback("MiniGL texture table exhausted", FXFALSE);
        return;
    }
    texture->size = span;
    texture_size(info, &width, &height);
    pixels = convert_texture(info, width, height);
    if(pixels == NULL) {
        /* Conversion failed.  The slot now claims an address whose GL texture
         * still holds the *previous* image, so grTexSource() would hand that
         * stale picture to whatever geometry asks for this address -- Max's
         * face turning up on tree sprites.  Retire the slot instead. */
        glDeleteTextures(1, &texture->name);
        memset(texture, 0, sizeof(*texture));
        return;
    }
    glBindTexture(GL_TEXTURE_2D, texture->name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, state_tex_min_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, state_tex_mag_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, state_tex_wrap_s);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, state_tex_wrap_t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    texture->width = width; texture->height = height;
    /* Restore the app's selected texture *and* its parameters — the LFB tile
     * textures force GL_NEAREST/GL_CLAMP and a bare rebind left those behind. */
    glBindTexture(GL_TEXTURE_2D, state_bound_texture);
    apply_texture_parameters();
    free(pixels);
}
/* Glide treats filter and clamp as per-TMU state which applies to whichever
 * texture is selected next.  In GL they are per-texture-object parameters, so
 * setting them on the currently bound object leaked one texture's filter onto
 * unrelated textures depending on bind order.  Track them here and re-apply on
 * every grTexSource bind. */
static void apply_texture_parameters(void)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, state_tex_min_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, state_tex_mag_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, state_tex_wrap_s);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, state_tex_wrap_t);
}

void grTexSource(GrChipID_t tmu, FxU32 address, FxU32 mask, GrTexInfo *info)
{
    fx_amiga_texture *texture = find_texture(address, FXFALSE);
    (void)tmu; (void)mask; (void)info;
    if(texture != NULL) {
        int largest = texture->width > texture->height ? texture->width : texture->height;
        int new_width = texture->width * 256 / largest;
        int new_height = texture->height * 256 / largest;
        if(state_texture_enabled && state_bound_texture == texture->name &&
            texture_width == new_width && texture_height == new_height)
            return;
        flush_triangle_batch();
        state_texture_enabled = FXTRUE;
        state_bound_texture = texture->name;
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, texture->name);
        apply_texture_parameters();
        /* Glide's s/t coordinates use a canonical 0..256 range at every LOD,
         * with only the aspect ratio reducing the shorter axis.  Dividing by
         * the physical size made 64px textures repeat four times. */
        texture_width = new_width;
        texture_height = new_height;
        texture_inv_width = 1.0f / new_width;
        texture_inv_height = 1.0f / new_height;
    } else {
        /* Selecting a texture we never cached must not leave the previous one
         * bound: the HUD's texture then bled onto world geometry (Max's face
         * tiled across buildings).  Fall back to untextured drawing. */
        if(!state_texture_enabled && state_bound_texture == 0)
            return;
        flush_triangle_batch();
        state_texture_enabled = FXFALSE;
        state_bound_texture = 0;
        glDisable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}
void grTexClampMode(GrChipID_t tmu, GrTextureClampMode_t s, GrTextureClampMode_t t)
{
    GLint new_s;
    GLint new_t;
    (void)tmu;
    new_s = s == GR_TEXTURECLAMP_CLAMP ? GL_CLAMP : GL_REPEAT;
    new_t = t == GR_TEXTURECLAMP_CLAMP ? GL_CLAMP : GL_REPEAT;
    if(state_tex_wrap_s == new_s && state_tex_wrap_t == new_t)
        return;
    flush_triangle_batch();
    state_tex_wrap_s = new_s;
    state_tex_wrap_t = new_t;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, state_tex_wrap_s);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, state_tex_wrap_t);
}
void grTexFilterMode(GrChipID_t tmu, GrTextureFilterMode_t min, GrTextureFilterMode_t mag)
{
    GLint new_min;
    GLint new_mag;
    (void)tmu;
    new_min = min == GR_TEXTUREFILTER_BILINEAR ? GL_LINEAR : GL_NEAREST;
    new_mag = mag == GR_TEXTUREFILTER_BILINEAR ? GL_LINEAR : GL_NEAREST;
    if(state_tex_min_filter == new_min && state_tex_mag_filter == new_mag)
        return;
    flush_triangle_batch();
    state_tex_min_filter = new_min;
    state_tex_mag_filter = new_mag;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, state_tex_min_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, state_tex_mag_filter);
}
void grTexMipMapMode(GrChipID_t tmu, GrMipMapMode_t mode, FxBool blend) { (void)tmu; (void)mode; (void)blend; }
void grTexCombineFunction(GrChipID_t tmu, GrTextureCombineFnc_t fn) { (void)tmu; (void)fn; }
void grTexDetailControl(GrChipID_t tmu, int bias, FxU8 scale, float max) { (void)tmu; (void)bias; (void)scale; (void)max; }
void grHints(GrHint_t hint, FxU32 value) { (void)hint; (void)value; }
void grGammaCorrectionValue(float value) { (void)value; }
void guAlphaSource(GrAlphaSource_t mode) { alpha_source = mode; }
void guColorCombineFunction(GrColorCombineFnc_t fn)
{
    if(colour_combine == fn)
        return;
    flush_triangle_batch();
    colour_combine = fn;
    /* Only a grTexSource() binding may enable texturing.  This used to call
     * glEnable(GL_TEXTURE_2D) on its own, which re-armed whatever texture was
     * still bound from the HUD -- match.c caches colour_mode, so the combine
     * mode is often selected without any accompanying grTexSource(). */
    if(uses_texture() && state_bound_texture != 0) {
        state_texture_enabled = FXTRUE;
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE,
            fn == GR_COLORCOMBINE_DECAL_TEXTURE ? GL_REPLACE : GL_MODULATE);
    } else { state_texture_enabled = FXFALSE; glDisable(GL_TEXTURE_2D); }
}
void guTexCombineFunction(GrChipID_t tmu, GrTextureCombineFnc_t fn) { grTexCombineFunction(tmu, fn); }
void guTexSource(GrMipMapId_t id)
{
    if(id == GR_NULL_MIPMAP_HANDLE) {
        /* Drop the binding too, so a later combine-mode change cannot resurrect
         * this texture on untextured geometry. */
        if(!state_texture_enabled && state_bound_texture == 0)
            return;
        flush_triangle_batch();
        state_texture_enabled = FXFALSE;
        state_bound_texture = 0;
        glDisable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

/* Mark one pixel as not being HUD content.
 *
 * Called from the game's masked HUD blit for every transparent source texel.
 * The shim's frame buffer copy is persistent and never cleared, so a texel the
 * blit skips would otherwise keep the previous frame's HUD pixel forever.
 * Clearing the overlay mask makes draw_lfb_layer() emit alpha 0 there, letting
 * the rendered 3D scene show through -- which is what the skipped write did on
 * real hardware. */
/* Mark one pixel as HUD content drawn during the current frame, protecting it
 * from FXA_ClearHudPixelAt() until the next frame begins. */
void FXA_MarkHudPixelAt(void *pixel)
{
    size_t offset;
    if(lfb_overlay_mask == NULL || lfb_colour == NULL || pixel == NULL)
        return;
    if((FxU16 *)pixel < lfb_colour)
        return;
    offset = (size_t)((FxU16 *)pixel - lfb_colour);
    if(offset >= (size_t)FXA_LFB_STRIDE_PIXELS * screen_height)
        return;
    lfb_overlay_colour[offset] = lfb_colour[offset];
    lfb_overlay_mask[offset] = FXA_HUD_MASK_THIS_FRAME;
}

void FXA_MarkHudMaskedSpan(void *pixels, const FxU8 *source, int count)
{
    size_t offset;
    size_t available;
    int i;
    if(lfb_overlay_mask == NULL || lfb_overlay_colour == NULL ||
        lfb_colour == NULL || pixels == NULL || source == NULL || count <= 0)
        return;
    if((FxU16 *)pixels < lfb_colour)
        return;
    offset = (size_t)((FxU16 *)pixels - lfb_colour);
    if(offset >= (size_t)FXA_LFB_STRIDE_PIXELS * screen_height)
        return;
    available = (size_t)FXA_LFB_STRIDE_PIXELS * screen_height - offset;
    if((size_t)count > available)
        count = (int)available;
    for(i = 0; i < count; i++) {
        if(source[i] != 0) {
            lfb_overlay_colour[offset + i] = lfb_colour[offset + i];
            lfb_overlay_mask[offset + i] = FXA_HUD_MASK_THIS_FRAME;
        }
    }
}

void FXA_ClearHudPixelAt(void *pixel)
{
    size_t offset;
    /* Take the address the blit was about to write.  Deriving the offset from
     * the pointer avoids having to replicate the pixelmap's base_x/base_y
     * origin, which directLock() has already folded into pm_pixels. */
    if(lfb_overlay_mask == NULL || lfb_colour == NULL || pixel == NULL)
        return;
    if((FxU16 *)pixel < lfb_colour)
        return;
    offset = (size_t)((FxU16 *)pixel - lfb_colour);
    if(offset >= (size_t)FXA_LFB_STRIDE_PIXELS * screen_height)
        return;
    /* Never clear a pixel another blit already drew this frame.  HUD panels
     * overlap -- the speedometer dial is blitted first, then its digits on top
     * -- and the digit blit's transparent texels would otherwise punch holes
     * straight through the dial underneath it. */
    if(lfb_overlay_mask[offset] == FXA_HUD_MASK_THIS_FRAME)
        return;
    lfb_overlay_mask[offset] = 0;
}

static FxBool ensure_lfb_buffers(void)
{
    size_t bytes;
    size_t mask_bytes;
    bytes = (size_t)FXA_LFB_STRIDE_PIXELS * screen_height * sizeof(FxU16);
    mask_bytes = (size_t)FXA_LFB_STRIDE_PIXELS * screen_height;
    if(lfb_colour == NULL) lfb_colour = calloc(1, bytes);
    if(lfb_original == NULL) lfb_original = calloc(1, bytes);
    if(lfb_lock_snapshot == NULL) lfb_lock_snapshot = calloc(1, bytes);
    if(lfb_depth == NULL) lfb_depth = calloc(1, bytes);
    if(lfb_overlay_colour == NULL) lfb_overlay_colour = calloc(1, bytes);
    if(lfb_overlay_mask == NULL) lfb_overlay_mask = calloc(1, mask_bytes);
    if(lfb_colour == NULL || lfb_original == NULL ||
        lfb_lock_snapshot == NULL || lfb_depth == NULL ||
        lfb_overlay_colour == NULL || lfb_overlay_mask == NULL) {
        return FXFALSE;
    }
    /* Keep one logical LFB in RAM.  Re-locking the RTG bitmap for every 2D
     * operation stalls Warp3D and, under UAE, does not include host-side 3D.
     *
     */
    if(!lfb_initial_capture_done) {
        capture_back_buffer();
        lfb_initial_capture_done = FXTRUE;
    }
    return FXTRUE;
}

void FXA_LfbSetWriteRegion(int x, int y, int width, int height)
{
    /* A nested operation shares the outer snapshot.  It cannot safely narrow
     * that outer operation's final scan, so fall back to the complete LFB. */
    if(lfb_lock_depth > 0) {
        lfb_operation_region_valid = FXFALSE;
        lfb_operation_read_only = FXFALSE;
        return;
    }
    lfb_operation_read_only = FXFALSE;
    if(width <= 0 || height <= 0) {
        lfb_operation_region_valid = FXFALSE;
        return;
    }
    if(x < 0) { width += x; x = 0; }
    if(y < 0) { height += y; y = 0; }
    if(x + width > screen_width) width = screen_width - x;
    if(y + height > screen_height) height = screen_height - y;
    if(width <= 0 || height <= 0) {
        lfb_operation_region_valid = FXFALSE;
        return;
    }
    lfb_operation_min_x = x;
    lfb_operation_min_y = y;
    lfb_operation_max_x = x + width - 1;
    lfb_operation_max_y = y + height - 1;
    lfb_operation_region_valid = FXTRUE;
}

void FXA_LfbSetReadOnly(void)
{
    if(lfb_lock_depth > 0)
        return;
    lfb_operation_region_valid = FXFALSE;
    lfb_operation_read_only = FXTRUE;
}

void FXA_LfbSetDirectWrite(void)
{
    /* During an established 3D race frame the game writes HUD pixels through
     * instrumented blitters.  Those writes update the overlay immediately, so
     * snapshotting and comparing the complete 640x480 LFB at every temporary
     * unlock only repeats work already done pixel-by-pixel.  Keep the scan for
     * menus and transitions, where arbitrary uninstrumented CPU writes occur. */
    lfb_operation_skip_scan = previous_frame_had_3d;
    lfb_operation_region_valid = FXFALSE;
    lfb_operation_read_only = FXFALSE;
}

void FXA_LfbCommitSolidRegion(int x, int y, int width, int height, FxU16 colour)
{
    int row;
    if(!lfb_operation_skip_scan || lfb_overlay_colour == NULL ||
        lfb_overlay_mask == NULL || width <= 0 || height <= 0)
        return;
    if(x < 0) { width += x; x = 0; }
    if(y < 0) { height += y; y = 0; }
    if(x + width > screen_width) width = screen_width - x;
    if(y + height > screen_height) height = screen_height - y;
    if(width <= 0 || height <= 0)
        return;
    if(colour == 0 && width >= screen_width * 3 / 4 &&
        height >= screen_height * 3 / 4) {
        memset(lfb_overlay_mask, 0,
            (size_t)FXA_LFB_STRIDE_PIXELS * screen_height);
        return;
    }
    for(row = y; row < y + height; row++) {
        FxU16 *dst = lfb_overlay_colour + row * FXA_LFB_STRIDE_PIXELS + x;
        FxU8 *mask = lfb_overlay_mask + row * FXA_LFB_STRIDE_PIXELS + x;
        int column;
        for(column = 0; column < width; column++)
            dst[column] = colour;
        memset(mask, FXA_HUD_MASK_THIS_FRAME, (size_t)width);
    }
}

void grLfbBegin(void)
{
    int y;
    flush_triangle_batch();
    if(lfb_lock_depth++ > 0)
        return;
    lfb_precommitted = 0;
    lfb_3d_prepared = 0;
    if(!ensure_lfb_buffers()) {
        lfb_lock_depth = 0;
        return;
    }
    if(lfb_operation_read_only || lfb_operation_skip_scan)
        return;
    if(lfb_operation_region_valid) {
        size_t bytes = (size_t)(lfb_operation_max_x - lfb_operation_min_x + 1) *
            sizeof(FxU16);
        for(y = lfb_operation_min_y; y <= lfb_operation_max_y; y++)
            memcpy(lfb_lock_snapshot + y * FXA_LFB_STRIDE_PIXELS +
                    lfb_operation_min_x,
                lfb_colour + y * FXA_LFB_STRIDE_PIXELS +
                    lfb_operation_min_x, bytes);
    } else {
        size_t bytes = (size_t)FXA_LFB_STRIDE_PIXELS * screen_height * sizeof(FxU16);
        memcpy(lfb_lock_snapshot, lfb_colour, bytes);
    }
}

/* Pixelmap lines reach the back buffer as hundreds of individual pixelSet()
 * calls.  Each pixel is already copied into the HUD overlay explicitly by
 * FXA_MarkHudPixelAt(), so taking a 960 KiB snapshot and scanning the complete
 * 640x480 LFB for every pixel is redundant. */
void FXA_LfbBeginMarkedWrite(void)
{
    if(lfb_lock_depth++ > 0)
        return;
    lfb_operation_region_valid = FXFALSE;
    lfb_operation_read_only = FXFALSE;
    lfb_precommitted = 0;
    lfb_3d_prepared = 0;
    if(!ensure_lfb_buffers())
        lfb_lock_depth = 0;
}

void FXA_LfbEndMarkedWrite(void)
{
    if(lfb_lock_depth <= 0)
        return;
    --lfb_lock_depth;
}

/* Temporarily release the logical LFB while BRender submits GL geometry.
 * Keep the original lock snapshot alive: DeviouslyDimRectangle() repeatedly
 * unlocks and re-locks the back screen even though it performs no CPU writes
 * while unlocked.  A normal grLfbEnd()/grLfbBegin() pair would scan and copy
 * the complete 1024x480 staging buffer for every rectangle. */
void FXA_LfbSuspend(void)
{
    if(lfb_suspended_lock_depth != 0 || lfb_lock_depth <= 0)
        return;
    lfb_suspended_lock_depth = lfb_lock_depth;
    lfb_lock_depth = 0;
}

void FXA_LfbResume(void)
{
    if(lfb_suspended_lock_depth == 0)
        return;
    lfb_lock_depth = lfb_suspended_lock_depth;
    lfb_suspended_lock_depth = 0;
    /* LFB operations performed while the persistent back-screen lock was
     * suspended have their own begin/end pair and reset the operation flags.
     * Restore the outer direct-write policy instead of making its eventual
     * unlock scan the complete colour buffer again. */
    lfb_operation_region_valid = FXFALSE;
    lfb_operation_read_only = FXFALSE;
    lfb_operation_skip_scan = previous_frame_had_3d || frame_world_rendered;
}

void grLfbEnd(void)
{
    if(lfb_lock_depth <= 0)
        return;
    if(--lfb_lock_depth > 0)
        return;
    if(!lfb_operation_read_only && !lfb_operation_skip_scan)
        update_lfb_overlay_from_operation();
    lfb_operation_region_valid = FXFALSE;
    lfb_operation_read_only = FXFALSE;
    lfb_operation_skip_scan = FXFALSE;
}
void grLfbBypassMode(GrLfbBypassMode_t mode) { (void)mode; }
void grLfbWriteMode(GrLfbWriteMode_t mode) { (void)mode; }
const FxU32 *grLfbGetReadPtr(GrBuffer_t buffer) { return (const FxU32 *)(buffer == GR_BUFFER_DEPTHBUFFER ? (void *)lfb_depth : (void *)lfb_colour); }
void *grLfbGetWritePtr(GrBuffer_t buffer) { return buffer == GR_BUFFER_DEPTHBUFFER ? (void *)lfb_depth : (void *)lfb_colour; }
void guFbReadRegion(const int x, const int y, const int w, const int h, const void *dst, const int stride)
{
    int row;
    const FxU16 *source = render_buffer == GR_BUFFER_DEPTHBUFFER ? lfb_depth : lfb_colour;
    for(row = 0; row < h; row++)
        memcpy((char *)(void *)dst + row * stride,
            source + (y + row) * FXA_LFB_STRIDE_PIXELS + x, (size_t)w * 2);
}
void guFbWriteRegion(const int x, const int y, const int w, const int h, const void *src, const int stride)
{
    int row;
    FxU16 *dest = render_buffer == GR_BUFFER_DEPTHBUFFER ? lfb_depth : lfb_colour;
    for(row = 0; row < h; row++)
        memcpy(dest + (y + row) * FXA_LFB_STRIDE_PIXELS + x,
            (const char *)src + row * stride, (size_t)w * 2);

}

/* Portable replacement for the DOS/x86 assembly monochrome-mask blitter. */
void _MemCopyBits3DFX_A(char *dest, FxU32 dest_qual, FxI32 dest_stride,
    FxU8 *src, FxU32 src_stride, FxU32 start_bit, FxU32 end_bit,
    FxU32 rows, FxU32 colour)
{
    FxU32 row, bit;
    (void)dest_qual;
    for(row = 0; row < rows; row++) {
        FxU16 *out = (FxU16 *)(dest + row * dest_stride);
        FxU8 *in = src + row * src_stride;
        for(bit = start_bit; bit < end_bit; bit++) {
            if(in[bit >> 3] & (0x80U >> (bit & 7)))
                out[bit - start_bit] = (FxU16)colour;
        }
    }
}
