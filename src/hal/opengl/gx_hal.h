#ifndef _GX_HAL_H_
#define _GX_HAL_H_

#include <dolphin/gx/GXEnum.h>
#include <dolphin/gx/GXStruct.h>
#include <dolphin/vi/vitypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _GXHALContext {
    int width;
    int height;
    int framebuffer_width;
    int framebuffer_height;
    
    unsigned int fbo;
    unsigned int color_texture;
    unsigned int depth_texture;
    
    unsigned int vao;
    unsigned int vbo;
    
    float projection_matrix[16];
    float modelview_matrix[16];
    float texture_matrix[16];
    
    GXRenderModeObj* current_render_mode;
    
    unsigned int current_program;
    int current_primitive_type;
    int vertex_count;
    int max_vertices;
    float* vertex_buffer;
    
    GXVtxFmt current_vtxfmt;
    GXAttr current_vtx_attrs[GX_VA_MAX_ATTR];
    GXAttrType current_vtx_types[GX_VA_MAX_ATTR];
    
    unsigned int bound_textures[8];
    GXTexObj* texture_objects[8];
    
    GXColor clear_color;
    unsigned int clear_z;
    
    GXCullMode cull_mode;
    GXBool lighting_enabled;
    GXBool depth_test_enabled;
    GXCompare depth_func;
    GXBool alpha_test_enabled;
    GXCompare alpha_func;
    u8 alpha_ref;
    
    GXBlendMode blend_mode;
    GXBlendFactor blend_src;
    GXBlendFactor blend_dst;
    GXLogicOp logic_op;
    
    GXFogType fog_type;
    float fog_start;
    float fog_end;
    GXColor fog_color;
    GXBool fog_enabled;
    
    int scissor_x, scissor_y, scissor_w, scissor_h;
    GXBool scissor_enabled;
    
    float viewport_x, viewport_y, viewport_w, viewport_h;
    
    GXTevStageID current_tev_stage;
    int num_tev_stages;
    
    GXBool texgen_enabled[8];
    GXTexGenType texgen_type[8];
    GXTexGenSrc texgen_src[8];
    u32 texgen_mtx[8];
    
    int num_tex_gens;
} GXHALContext;

extern GXHALContext* gx_hal_get_context(void);
extern void gx_hal_init(int width, int height, GXRenderModeObj* rm);
extern void gx_hal_set_efb_size(int w, int h);
extern void gx_hal_bind_efb(void); // (re)bind the EFB framebuffer
extern void gx_hal_shutdown(void);
extern void gx_hal_swap_buffers(void);
extern void gx_hal_set_viewport(float x, float y, float w, float h);
extern void gx_hal_set_scissor(int x, int y, int w, int h);
extern void gx_hal_clear(GXColor color, u32 z);
extern void gx_hal_set_copy_clear(GXColor color, u32 z);
extern void gx_hal_begin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts);
extern void gx_hal_end(void);
extern void gx_hal_flush(void); // drain trailing batch (present path)
extern void gx_hal_set_vtx_desc(GXAttr attr, GXAttrType type);
extern void gx_hal_set_vtx_attr_fmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac);
extern void gx_hal_set_array(GXAttr attr, const void* base_ptr, u8 stride);
extern void gx_hal_set_tev_stage(GXTevStageID stage);
extern void gx_hal_load_tex_obj(GXTexObj* obj, GXTexMapID id);
extern void gx_hal_init_tex_obj(GXTexObj* obj, void* image_ptr, u16 width, u16 height,
                                GXTexFmt format, GXTexWrapMode wrap_s, GXTexWrapMode wrap_t,
                                u8 mipmap);
extern void gx_hal_init_tex_obj_lod(GXTexObj* obj, GXTexFilter min_filt, GXTexFilter mag_filt,
                                    f32 min_lod, f32 max_lod, f32 lod_bias, GXBool bias_clamp,
                                    GXBool do_edge_lod, GXAnisotropy max_aniso);
extern void gx_hal_set_cull_mode(GXCullMode mode);
extern void gx_hal_set_ztest(GXBool enable, GXCompare func, GXBool update);
extern void gx_hal_set_blend_mode(GXBlendMode mode, GXBlendFactor src, GXBlendFactor dst, GXLogicOp logic_op);
extern void gx_hal_set_alpha_test(GXBool enable, GXCompare func, u8 ref);
extern void gx_hal_set_fog(GXFogType type, float start, float end, float nearz, float farz, GXColor color);
extern void gx_hal_set_projection(GXProjectionType type, float left, float right, float top, float bottom, float nearz, float farz);
extern void gx_hal_load_projection_mtx(float* mtx, GXProjectionType type);
extern void gx_hal_load_pos_mtx_imm(float* mtx, u32 mtx_idx);
extern void gx_hal_load_nrm_mtx_imm(float* mtx, u32 mtx_idx);
extern void gx_hal_load_tex_mtx_imm(float* mtx, u32 mtx_idx);
extern void gx_hal_set_num_tex_gens(u8 n);
extern void gx_hal_set_tex_coord_gen(GXTexCoordID dst, GXTexGenType func, GXTexGenSrc src, u32 mtx);
extern void gx_hal_set_tex_coord_gen2(GXTexCoordID dst, GXTexGenType func, GXTexGenSrc src, u32 mtx, GXBool normalize, u32 pt_texmtx);
extern void gx_hal_set_tex_coord_scale_manually(GXTexCoordID coord, u8 enable, u16 ss, u16 ts);
extern void gx_hal_set_tex_coord_cyl_wrap(GXTexCoordID coord, u8 s_enable, u8 t_enable);
extern void gx_hal_set_tex_coord_bias(GXTexCoordID coord, u8 s_enable, u8 t_enable);
extern void gx_hal_set_num_chans(u8 n);
extern void gx_hal_set_chan_ctrl(GXChannelID chan, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src, u32 light_mask, GXDiffuseFn diff_fn, GXAttnFn attn_fn);
extern void gx_hal_set_chan_amb_color(GXChannelID chan, GXColor color);
extern void gx_hal_set_chan_mat_color(GXChannelID chan, GXColor color);
extern void gx_hal_invalidate_tex_all(void);
extern void gx_hal_copy_disp(void* dest, GXBool clear);
extern u32 gx_hal_get_tex_buffer_size(u16 width, u16 height, u32 format, u8 mipmap, u8 max_lod);

#ifdef __cplusplus
}
#endif

#endif