/*
 * Copyright � 2006 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *    Keith Packard <keithp@keithp.com>
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "xf86.h"
#include "xf86_OSproc.h"
#include "xf86xv.h"
#include "fourcc.h"

#include "i830.h"
#include "i830_video.h"
#include "i830_hwmc.h"
#include "brw_defines.h"
#include "brw_structs.h"
#include <string.h>

/* Make assert() work. */
#undef NDEBUG
#include <assert.h>

static const uint32_t sip_kernel_static[][4] = {
/*    wait (1) a0<1>UW a145<0,1,0>UW { align1 +  } */
    { 0x00000030, 0x20000108, 0x00001220, 0x00000000 },
/*    nop (4) g0<1>UD { align1 +  } */
    { 0x0040007e, 0x20000c21, 0x00690000, 0x00000000 },
/*    nop (4) g0<1>UD { align1 +  } */
    { 0x0040007e, 0x20000c21, 0x00690000, 0x00000000 },
/*    nop (4) g0<1>UD { align1 +  } */
    { 0x0040007e, 0x20000c21, 0x00690000, 0x00000000 },
/*    nop (4) g0<1>UD { align1 +  } */
    { 0x0040007e, 0x20000c21, 0x00690000, 0x00000000 },
/*    nop (4) g0<1>UD { align1 +  } */
    { 0x0040007e, 0x20000c21, 0x00690000, 0x00000000 },
/*    nop (4) g0<1>UD { align1 +  } */
    { 0x0040007e, 0x20000c21, 0x00690000, 0x00000000 },
/*    nop (4) g0<1>UD { align1 +  } */
    { 0x0040007e, 0x20000c21, 0x00690000, 0x00000000 },
/*    nop (4) g0<1>UD { align1 +  } */
    { 0x0040007e, 0x20000c21, 0x00690000, 0x00000000 },
/*    nop (4) g0<1>UD { align1 +  } */
    { 0x0040007e, 0x20000c21, 0x00690000, 0x00000000 },
};

/*
 * this program computes dA/dx and dA/dy for the texture coordinates along
 * with the base texture coordinate. It was extracted from the Mesa driver.
 * It uses about 10 GRF registers.
 */

#define SF_KERNEL_NUM_GRF  16
#define SF_MAX_THREADS	   1

static const uint32_t sf_kernel_static[][4] = {
#include "exa_sf.g4b"
};

/*
 * Ok, this kernel picks up the required data flow values in g0 and g1
 * and passes those along in m0 and m1. In m2-m9, it sticks constant
 * values (bright pink).
 */

/* Our PS kernel uses less than 32 GRF registers (about 20) */
#define PS_KERNEL_NUM_GRF   32
#define PS_MAX_THREADS	   32

#define BRW_GRF_BLOCKS(nreg)	((nreg + 15) / 16 - 1)

static const uint32_t ps_kernel_packed_static[][4] = {
#include "exa_wm_xy.g4b"
#include "exa_wm_src_affine.g4b"
#include "exa_wm_src_sample_argb.g4b"
#include "exa_wm_yuv_rgb.g4b"
#include "exa_wm_write.g4b"
};

static const uint32_t ps_kernel_planar_static[][4] = {
#include "exa_wm_xy.g4b"
#include "exa_wm_src_affine.g4b"
#include "exa_wm_src_sample_planar.g4b"
#include "exa_wm_yuv_rgb.g4b"
#include "exa_wm_write.g4b"
};

#define ALIGN(i,m)    (((i) + (m) - 1) & ~((m) - 1))
#define MIN(a,b) ((a) < (b) ? (a) : (b))

static uint32_t float_to_uint (float f) {
    union {uint32_t i; float f;} x;
    x.f = f;
    return x.i;
}

#if 0
static struct {
    uint32_t svg_ctl;
    char *name;
} svg_ctl_bits[] = {
    { BRW_SVG_CTL_GS_BA, "General State Base Address" },
    { BRW_SVG_CTL_SS_BA, "Surface State Base Address" },
    { BRW_SVG_CTL_IO_BA, "Indirect Object Base Address" },
    { BRW_SVG_CTL_GS_AUB, "Generate State Access Upper Bound" },
    { BRW_SVG_CTL_IO_AUB, "Indirect Object Access Upper Bound" },
    { BRW_SVG_CTL_SIP, "System Instruction Pointer" },
    { 0, 0 },
};

static void
brw_debug (ScrnInfoPtr pScrn, char *when)
{
    I830Ptr pI830 = I830PTR(pScrn);
    int	i;
    uint32_t v;

    I830Sync (pScrn);
    ErrorF("brw_debug: %s\n", when);
    for (i = 0; svg_ctl_bits[i].name; i++) {
	OUTREG(BRW_SVG_CTL, svg_ctl_bits[i].svg_ctl);
	v = INREG(BRW_SVG_RDATA);
	ErrorF("\t%34.34s: 0x%08x\n", svg_ctl_bits[i].name, v);
    }
}
#endif

#define WATCH_SF 0
#define WATCH_WIZ 0
#define WATCH_STATS 0

static void
i965_pre_draw_debug(ScrnInfoPtr scrn)
{
#if 0
    I830Ptr pI830 = I830PTR(scrn);
#endif

#if 0
    ErrorF ("before EU_ATT 0x%08x%08x EU_ATT_DATA 0x%08x%08x\n",
	    INREG(BRW_EU_ATT_1), INREG(BRW_EU_ATT_0),
	    INREG(BRW_EU_ATT_DATA_1), INREG(BRW_EU_ATT_DATA_0));

    OUTREG(BRW_VF_CTL,
	   BRW_VF_CTL_SNAPSHOT_MUX_SELECT_THREADID |
	   BRW_VF_CTL_SNAPSHOT_TYPE_VERTEX_INDEX |
	   BRW_VF_CTL_SNAPSHOT_ENABLE);
    OUTREG(BRW_VF_STRG_VAL, 0);
#endif

#if 0
    OUTREG(BRW_VS_CTL,
	   BRW_VS_CTL_SNAPSHOT_ALL_THREADS |
	   BRW_VS_CTL_SNAPSHOT_MUX_VALID_COUNT |
	   BRW_VS_CTL_THREAD_SNAPSHOT_ENABLE);

    OUTREG(BRW_VS_STRG_VAL, 0);
#endif

#if WATCH_SF
    OUTREG(BRW_SF_CTL,
	   BRW_SF_CTL_SNAPSHOT_MUX_VERTEX_COUNT |
	   BRW_SF_CTL_SNAPSHOT_ALL_THREADS |
	   BRW_SF_CTL_THREAD_SNAPSHOT_ENABLE);
    OUTREG(BRW_SF_STRG_VAL, 0);
#endif

#if WATCH_WIZ
    OUTREG(BRW_WIZ_CTL,
	   BRW_WIZ_CTL_SNAPSHOT_MUX_SUBSPAN_INSTANCE |
	   BRW_WIZ_CTL_SNAPSHOT_ALL_THREADS |
	   BRW_WIZ_CTL_SNAPSHOT_ENABLE);
    OUTREG(BRW_WIZ_STRG_VAL,
	   (box_x1) | (box_y1 << 16));
#endif

#if 0
    OUTREG(BRW_TS_CTL,
	   BRW_TS_CTL_SNAPSHOT_MESSAGE_ERROR |
	   BRW_TS_CTL_SNAPSHOT_ALL_CHILD_THREADS |
	   BRW_TS_CTL_SNAPSHOT_ALL_ROOT_THREADS |
	   BRW_TS_CTL_SNAPSHOT_ENABLE);
#endif
}

static void
i965_post_draw_debug(ScrnInfoPtr scrn)
{
#if 0
    I830Ptr pI830 = I830PTR(scrn);
#endif

#if 0
    for (j = 0; j < 100000; j++) {
	ctl = INREG(BRW_VF_CTL);
	if (ctl & BRW_VF_CTL_SNAPSHOT_COMPLETE)
	    break;
    }

    rdata = INREG(BRW_VF_RDATA);
    OUTREG(BRW_VF_CTL, 0);
    ErrorF ("VF_CTL: 0x%08x VF_RDATA: 0x%08x\n", ctl, rdata);
#endif

#if 0
    for (j = 0; j < 1000000; j++) {
	ctl = INREG(BRW_VS_CTL);
	if (ctl & BRW_VS_CTL_SNAPSHOT_COMPLETE)
	    break;
    }

    rdata = INREG(BRW_VS_RDATA);
    for (k = 0; k <= 3; k++) {
	OUTREG(BRW_VS_CTL,
	       BRW_VS_CTL_SNAPSHOT_COMPLETE |
	       (k << 8));
	rdata = INREG(BRW_VS_RDATA);
	ErrorF ("VS_CTL: 0x%08x VS_RDATA(%d): 0x%08x\n", ctl, k, rdata);
    }

    OUTREG(BRW_VS_CTL, 0);
#endif

#if WATCH_SF
    for (j = 0; j < 1000000; j++) {
	ctl = INREG(BRW_SF_CTL);
	if (ctl & BRW_SF_CTL_SNAPSHOT_COMPLETE)
	    break;
    }

    for (k = 0; k <= 7; k++) {
	OUTREG(BRW_SF_CTL,
	       BRW_SF_CTL_SNAPSHOT_COMPLETE |
	       (k << 8));
	rdata = INREG(BRW_SF_RDATA);
	ErrorF("SF_CTL: 0x%08x SF_RDATA(%d): 0x%08x\n", ctl, k, rdata);
    }

    OUTREG(BRW_SF_CTL, 0);
#endif

#if WATCH_WIZ
    for (j = 0; j < 100000; j++) {
	ctl = INREG(BRW_WIZ_CTL);
	if (ctl & BRW_WIZ_CTL_SNAPSHOT_COMPLETE)
	    break;
    }

    rdata = INREG(BRW_WIZ_RDATA);
    OUTREG(BRW_WIZ_CTL, 0);
    ErrorF("WIZ_CTL: 0x%08x WIZ_RDATA: 0x%08x\n", ctl, rdata);
#endif

#if 0
    for (j = 0; j < 100000; j++) {
	ctl = INREG(BRW_TS_CTL);
	if (ctl & BRW_TS_CTL_SNAPSHOT_COMPLETE)
	    break;
    }

    rdata = INREG(BRW_TS_RDATA);
    OUTREG(BRW_TS_CTL, 0);
    ErrorF("TS_CTL: 0x%08x TS_RDATA: 0x%08x\n", ctl, rdata);

    ErrorF("after EU_ATT 0x%08x%08x EU_ATT_DATA 0x%08x%08x\n",
	   INREG(BRW_EU_ATT_1), INREG(BRW_EU_ATT_0),
	   INREG(BRW_EU_ATT_DATA_1), INREG(BRW_EU_ATT_DATA_0));
#endif

#if 0
    for (j = 0; j < 256; j++) {
	OUTREG(BRW_TD_CTL, j << BRW_TD_CTL_MUX_SHIFT);
	rdata = INREG(BRW_TD_RDATA);
	ErrorF ("TD_RDATA(%d): 0x%08x\n", j, rdata);
    }
#endif
}

/* For 3D, the VS must have 8, 12, 16, 24, or 32 VUEs allocated to it.
 * A VUE consists of a 256-bit vertex header followed by the vertex data,
 * which in our case is 4 floats (128 bits), thus a single 512-bit URB
 * entry.
 */
#define URB_VS_ENTRIES	      8
#define URB_VS_ENTRY_SIZE     1

#define URB_GS_ENTRIES	      0
#define URB_GS_ENTRY_SIZE     0

#define URB_CLIP_ENTRIES      0
#define URB_CLIP_ENTRY_SIZE   0

/* The SF kernel we use outputs only 4 256-bit registers, leading to an
 * entry size of 2 512-bit URBs.  We don't need to have many entries to
 * output as we're generally working on large rectangles and don't care
 * about having WM threads running on different rectangles simultaneously.
 */
#define URB_SF_ENTRIES	      1
#define URB_SF_ENTRY_SIZE     2

#define URB_CS_ENTRIES	      0
#define URB_CS_ENTRY_SIZE     0

/**
 * Little wrapper around drm_intel_bo_reloc to return the initial value you
 * should stuff into the relocation entry.
 *
 * If only we'd done this before settling on the library API.
 */
static uint32_t
intel_emit_reloc(drm_intel_bo *bo, uint32_t offset,
		 drm_intel_bo *target_bo, uint32_t target_offset,
		 uint32_t read_domains, uint32_t write_domain)
{
    drm_intel_bo_emit_reloc(bo, offset, target_bo, target_offset,
			    read_domains, write_domain);

    return target_bo->offset + target_offset;
}

static void
i965_set_dst_surface_state(ScrnInfoPtr scrn,
			   struct brw_surface_state *dest_surf_state,
			   PixmapPtr pixmap)
{
    I830Ptr pI830 = I830PTR(scrn);

    memset(dest_surf_state, 0, sizeof(*dest_surf_state));
    dest_surf_state->ss0.surface_type = BRW_SURFACE_2D;
    dest_surf_state->ss0.data_return_format = BRW_SURFACERETURNFORMAT_FLOAT32;
    if (pI830->cpp == 2) {
	dest_surf_state->ss0.surface_format = BRW_SURFACEFORMAT_B5G6R5_UNORM;
    } else {
	dest_surf_state->ss0.surface_format = BRW_SURFACEFORMAT_B8G8R8A8_UNORM;
    }
    dest_surf_state->ss0.writedisable_alpha = 0;
    dest_surf_state->ss0.writedisable_red = 0;
    dest_surf_state->ss0.writedisable_green = 0;
    dest_surf_state->ss0.writedisable_blue = 0;
    dest_surf_state->ss0.color_blend = 1;
    dest_surf_state->ss0.vert_line_stride = 0;
    dest_surf_state->ss0.vert_line_stride_ofs = 0;
    dest_surf_state->ss0.mipmap_layout_mode = 0;
    dest_surf_state->ss0.render_cache_read_mode = 0;

    dest_surf_state->ss1.base_addr = intel_get_pixmap_offset(pixmap);
    dest_surf_state->ss2.height = scrn->virtualY - 1;
    dest_surf_state->ss2.width = scrn->virtualX - 1;
    dest_surf_state->ss2.mip_count = 0;
    dest_surf_state->ss2.render_target_rotation = 0;
    dest_surf_state->ss3.pitch = intel_get_pixmap_pitch(pixmap) - 1;
    dest_surf_state->ss3.tiled_surface = i830_pixmap_tiled(pixmap);
    dest_surf_state->ss3.tile_walk = 0; /* TileX */
}

static void
i965_set_src_surface_state(ScrnInfoPtr scrn,
			      struct brw_surface_state *src_surf_state,
			      uint32_t src_offset,
			      int src_width,
			      int src_height,
			      int src_pitch,
			      uint32_t src_surf_format)
{
    /* Set up the source surface state buffer */
    memset(src_surf_state, 0, sizeof(struct brw_surface_state));
    src_surf_state->ss0.surface_type = BRW_SURFACE_2D;
    src_surf_state->ss0.surface_format = src_surf_format;
    src_surf_state->ss0.writedisable_alpha = 0;
    src_surf_state->ss0.writedisable_red = 0;
    src_surf_state->ss0.writedisable_green = 0;
    src_surf_state->ss0.writedisable_blue = 0;
    src_surf_state->ss0.color_blend = 1;
    src_surf_state->ss0.vert_line_stride = 0;
    src_surf_state->ss0.vert_line_stride_ofs = 0;
    src_surf_state->ss0.mipmap_layout_mode = 0;
    src_surf_state->ss0.render_cache_read_mode = 0;

    src_surf_state->ss1.base_addr = src_offset;
    src_surf_state->ss2.width = src_width - 1;
    src_surf_state->ss2.height = src_height - 1;
    src_surf_state->ss2.mip_count = 0;
    src_surf_state->ss2.render_target_rotation = 0;
    src_surf_state->ss3.pitch = src_pitch - 1;
}

static void
i965_set_sampler_state(ScrnInfoPtr scrn,
		       struct brw_sampler_state *sampler_state)
{
    memset(sampler_state, 0, sizeof(struct brw_sampler_state));

    sampler_state->ss0.min_filter = BRW_MAPFILTER_LINEAR;
    sampler_state->ss0.mag_filter = BRW_MAPFILTER_LINEAR;
    sampler_state->ss1.r_wrap_mode = BRW_TEXCOORDMODE_CLAMP;
    sampler_state->ss1.s_wrap_mode = BRW_TEXCOORDMODE_CLAMP;
    sampler_state->ss1.t_wrap_mode = BRW_TEXCOORDMODE_CLAMP;
}

static drm_intel_bo *
i965_create_vs_state(ScrnInfoPtr scrn)
{
    I830Ptr pI830 = I830PTR(scrn);
    drm_intel_bo *vs_bo;
    struct brw_vs_unit_state *vs_state;

    vs_bo = drm_intel_bo_alloc(pI830->bufmgr, "textured video vs state",
			       4096, 4096);
    drm_intel_bo_map(vs_bo, TRUE);
    vs_state = vs_bo->virtual;

    /* Set up the vertex shader to be disabled (passthrough) */
    memset(vs_state, 0, sizeof(*vs_state));
    vs_state->thread4.nr_urb_entries = URB_VS_ENTRIES;
    vs_state->thread4.urb_entry_allocation_size = URB_VS_ENTRY_SIZE - 1;
    vs_state->vs6.vs_enable = 0;
    vs_state->vs6.vert_cache_disable = 1;

    drm_intel_bo_unmap(vs_bo);
    return vs_bo;
}

static drm_intel_bo *
i965_create_sf_state(ScrnInfoPtr scrn, uint32_t sf_kernel_offset)
{
    I830Ptr pI830 = I830PTR(scrn);
    drm_intel_bo *sf_bo;
    struct brw_sf_unit_state *sf_state;

    sf_bo = drm_intel_bo_alloc(pI830->bufmgr, "textured video sf state",
			       4096, 4096);
    drm_intel_bo_map(sf_bo, TRUE);
    sf_state = sf_bo->virtual;

    /* Set up the SF kernel to do coord interp: for each attribute,
     * calculate dA/dx and dA/dy.  Hand these interpolation coefficients
     * back to SF which then hands pixels off to WM.
     */
    memset(sf_state, 0, sizeof(*sf_state));
    sf_state->thread0.kernel_start_pointer = sf_kernel_offset >> 6;
    sf_state->thread0.grf_reg_count = BRW_GRF_BLOCKS(SF_KERNEL_NUM_GRF);
    sf_state->sf1.single_program_flow = 1; /* XXX */
    sf_state->sf1.binding_table_entry_count = 0;
    sf_state->sf1.thread_priority = 0;
    sf_state->sf1.floating_point_mode = 0; /* Mesa does this */
    sf_state->sf1.illegal_op_exception_enable = 1;
    sf_state->sf1.mask_stack_exception_enable = 1;
    sf_state->sf1.sw_exception_enable = 1;
    sf_state->thread2.per_thread_scratch_space = 0;
    /* scratch space is not used in our kernel */
    sf_state->thread2.scratch_space_base_pointer = 0;
    sf_state->thread3.const_urb_entry_read_length = 0; /* no const URBs */
    sf_state->thread3.const_urb_entry_read_offset = 0; /* no const URBs */
    sf_state->thread3.urb_entry_read_length = 1; /* 1 URB per vertex */
    sf_state->thread3.urb_entry_read_offset = 0;
    sf_state->thread3.dispatch_grf_start_reg = 3;
    sf_state->thread4.max_threads = SF_MAX_THREADS - 1;
    sf_state->thread4.urb_entry_allocation_size = URB_SF_ENTRY_SIZE - 1;
    sf_state->thread4.nr_urb_entries = URB_SF_ENTRIES;
    sf_state->thread4.stats_enable = 1;
    sf_state->sf5.viewport_transform = FALSE; /* skip viewport */
    sf_state->sf6.cull_mode = BRW_CULLMODE_NONE;
    sf_state->sf6.scissor = 0;
    sf_state->sf7.trifan_pv = 2;
    sf_state->sf6.dest_org_vbias = 0x8;
    sf_state->sf6.dest_org_hbias = 0x8;

    drm_intel_bo_unmap(sf_bo);
    return sf_bo;
}

static void
i965_set_wm_state(ScrnInfoPtr scrn, struct brw_wm_unit_state *wm_state,
		  uint32_t ps_kernel_offset,
		  uint32_t sampler_offset, int n_src_surf)
{
    memset(wm_state, 0, sizeof (*wm_state));
    wm_state->thread0.kernel_start_pointer = ps_kernel_offset >> 6;
    wm_state->thread0.grf_reg_count = BRW_GRF_BLOCKS(PS_KERNEL_NUM_GRF);
    wm_state->thread1.single_program_flow = 1; /* XXX */
    wm_state->thread1.binding_table_entry_count = 1 + n_src_surf;
    /* Though we never use the scratch space in our WM kernel, it has to be
     * set, and the minimum allocation is 1024 bytes.
     */
    wm_state->thread2.scratch_space_base_pointer = 0;
    wm_state->thread2.per_thread_scratch_space = 0; /* 1024 bytes */
    wm_state->thread3.dispatch_grf_start_reg = 3; /* XXX */
    wm_state->thread3.const_urb_entry_read_length = 0;
    wm_state->thread3.const_urb_entry_read_offset = 0;
    wm_state->thread3.urb_entry_read_length = 1; /* XXX */
    wm_state->thread3.urb_entry_read_offset = 0; /* XXX */
    wm_state->wm4.stats_enable = 1;
    wm_state->wm4.sampler_state_pointer = sampler_offset >> 5;
    wm_state->wm4.sampler_count = 1; /* 1-4 samplers used */
    wm_state->wm5.max_threads = PS_MAX_THREADS - 1;
    wm_state->wm5.thread_dispatch_enable = 1;
    wm_state->wm5.enable_16_pix = 1;
    wm_state->wm5.enable_8_pix = 0;
    wm_state->wm5.early_depth_test = 1;
}

static drm_intel_bo *
i965_create_cc_vp_state(ScrnInfoPtr scrn)
{
    I830Ptr pI830 = I830PTR(scrn);
    drm_intel_bo *cc_vp_bo;
    struct brw_cc_viewport *cc_viewport;

    cc_vp_bo = drm_intel_bo_alloc(pI830->bufmgr, "textured video cc viewport",
				  4096, 4096);
    drm_intel_bo_map(cc_vp_bo, TRUE);
    cc_viewport = cc_vp_bo->virtual;

    memset (cc_viewport, 0, sizeof (*cc_viewport));
    cc_viewport->min_depth = -1.e35;
    cc_viewport->max_depth = 1.e35;

    drm_intel_bo_unmap(cc_vp_bo);
    return cc_vp_bo;
}

static drm_intel_bo *
i965_create_cc_state(ScrnInfoPtr scrn)
{
    I830Ptr pI830 = I830PTR(scrn);
    drm_intel_bo *cc_bo, *cc_vp_bo;
    struct brw_cc_unit_state *cc_state;

    cc_vp_bo = i965_create_cc_vp_state(scrn);

    cc_bo = drm_intel_bo_alloc(pI830->bufmgr, "textured video cc state",
			       4096, 4096);
    drm_intel_bo_map(cc_bo, TRUE);
    cc_state = cc_bo->virtual;

    /* Color calculator state */
    memset(cc_state, 0, sizeof(*cc_state));
    cc_state->cc0.stencil_enable = 0;   /* disable stencil */
    cc_state->cc2.depth_test = 0;       /* disable depth test */
    cc_state->cc2.logicop_enable = 1;   /* enable logic op */
    cc_state->cc3.ia_blend_enable = 1;  /* blend alpha just like colors */
    cc_state->cc3.blend_enable = 0;     /* disable color blend */
    cc_state->cc3.alpha_test = 0;       /* disable alpha test */
    cc_state->cc4.cc_viewport_state_offset =
	intel_emit_reloc(cc_bo, offsetof(struct brw_cc_unit_state, cc4),
			 cc_vp_bo, 0,
			 I915_GEM_DOMAIN_INSTRUCTION, 0) >> 5;
    cc_state->cc5.dither_enable = 0;    /* disable dither */
    cc_state->cc5.logicop_func = 0xc;   /* WHITE */
    cc_state->cc5.statistics_enable = 1;
    cc_state->cc5.ia_blend_function = BRW_BLENDFUNCTION_ADD;
    cc_state->cc5.ia_src_blend_factor = BRW_BLENDFACTOR_ONE;
    cc_state->cc5.ia_dest_blend_factor = BRW_BLENDFACTOR_ONE;

    drm_intel_bo_unmap(cc_bo);

    drm_intel_bo_unreference(cc_vp_bo);
    return cc_bo;
}

void
I965DisplayVideoTextured(ScrnInfoPtr pScrn, I830PortPrivPtr pPriv, int id,
			 RegionPtr dstRegion,
			 short width, short height, int video_pitch,
			 int x1, int y1, int x2, int y2,
			 short src_w, short src_h,
			 short drw_w, short drw_h,
			 PixmapPtr pPixmap)
{
    I830Ptr pI830 = I830PTR(pScrn);
    BoxPtr pbox;
    int nbox, dxo, dyo, pix_xoff, pix_yoff;
    int urb_vs_start, urb_vs_size;
    int urb_gs_start, urb_gs_size;
    int urb_clip_start, urb_clip_size;
    int urb_sf_start, urb_sf_size;
    int urb_cs_start, urb_cs_size;
    float src_scale_x, src_scale_y;
    uint32_t *binding_table;
    Bool first_output = TRUE;
    int dest_surf_offset, src_surf_offset[6], sampler_offset[6];
    int wm_offset, vb_offset;
    int sf_kernel_offset, ps_kernel_offset, sip_kernel_offset;
    int binding_table_offset;
    int next_offset, total_state_size;
    int vb_size = (4 * 4) * 4; /* 4 DWORDS per vertex */
    char *state_base;
    int state_base_offset;
    int src_surf;
    int n_src_surf;
    uint32_t	src_surf_format;
    uint32_t	src_surf_base[6];
    int		src_width[6];
    int		src_height[6];
    int		src_pitch[6];
    int wm_binding_table_entries;
    const uint32_t	*ps_kernel_static;
    int		ps_kernel_static_size;

#if 0
    ErrorF("BroadwaterDisplayVideoTextured: %dx%d (pitch %d)\n", width, height,
	   video_pitch);
#endif

    /* enable debug */
    OUTREG (INST_PM,
	    (1 << (16 + 4)) |
	    (1 << 4));
#if 0
    ErrorF ("INST_PM 0x%08x\n", INREG(INST_PM));
#endif

    src_surf_base[0] = pPriv->YBuf0offset;
    src_surf_base[1] = pPriv->YBuf0offset;
    src_surf_base[2] = pPriv->VBuf0offset;
    src_surf_base[3] = pPriv->VBuf0offset;
    src_surf_base[4] = pPriv->UBuf0offset;
    src_surf_base[5] = pPriv->UBuf0offset;
#if 0
    ErrorF ("base 0 0x%x base 1 0x%x base 2 0x%x\n",
	    src_surf_base[0], src_surf_base[1], src_surf_base[2]);
#endif
    
    switch (id) {
    case FOURCC_UYVY:
	src_surf_format = BRW_SURFACEFORMAT_YCRCB_SWAPY;
	n_src_surf = 1;
	ps_kernel_static = &ps_kernel_packed_static[0][0];
	ps_kernel_static_size = sizeof (ps_kernel_packed_static);
	src_width[0] = width;
	src_height[0] = height;
	src_pitch[0] = video_pitch;
	break;
    case FOURCC_YUY2:
	src_surf_format = BRW_SURFACEFORMAT_YCRCB_NORMAL;
	ps_kernel_static = &ps_kernel_packed_static[0][0];
	ps_kernel_static_size = sizeof (ps_kernel_packed_static);
	src_width[0] = width;
	src_height[0] = height;
	src_pitch[0] = video_pitch;
	n_src_surf = 1;
	break;
#ifdef INTEL_XVMC
    case FOURCC_XVMC:
#endif
    case FOURCC_I420:
    case FOURCC_YV12:
	src_surf_format = BRW_SURFACEFORMAT_R8_UNORM;
	ps_kernel_static = &ps_kernel_planar_static[0][0];
	ps_kernel_static_size = sizeof (ps_kernel_planar_static);
	src_width[1] = src_width[0] = width;
	src_height[1] = src_height[0] = height;
	src_pitch[1] = src_pitch[0] = video_pitch * 2;
	src_width[4] = src_width[5] = src_width[2] = src_width[3] = width / 2;
	src_height[4] = src_height[5] = src_height[2] = src_height[3] = height / 2;
	src_pitch[4] = src_pitch[5] = src_pitch[2] = src_pitch[3] = video_pitch;
	n_src_surf = 6;
	break;
    default:
	return;
    }    
    wm_binding_table_entries = 1 + n_src_surf;

    IntelEmitInvarientState(pScrn);
    *pI830->last_3d = LAST_3D_VIDEO;

    next_offset = 0;

    /* Set up our layout of state in framebuffer.  First the general state: */
    wm_offset = ALIGN(next_offset, 32);
    next_offset = wm_offset + sizeof(struct brw_wm_unit_state);

    sf_kernel_offset = ALIGN(next_offset, 64);
    next_offset = sf_kernel_offset + sizeof (sf_kernel_static);
    ps_kernel_offset = ALIGN(next_offset, 64);
    next_offset = ps_kernel_offset + ps_kernel_static_size;
    sip_kernel_offset = ALIGN(next_offset, 64);
    next_offset = sip_kernel_offset + sizeof (sip_kernel_static);

    for (src_surf = 0; src_surf < n_src_surf; src_surf++) {    
	sampler_offset[src_surf] = ALIGN(next_offset, 32);
	next_offset = sampler_offset[src_surf] + sizeof(struct brw_sampler_state);
    }
    
    /* Align VB to native size of elements, for safety */
    vb_offset = ALIGN(next_offset, 8);
    next_offset = vb_offset + vb_size;

    /* And then the general state: */
    dest_surf_offset = ALIGN(next_offset, 32);
    next_offset = dest_surf_offset + sizeof(struct brw_surface_state);
    
    for (src_surf = 0; src_surf < n_src_surf; src_surf++) {
	src_surf_offset[src_surf] = ALIGN(next_offset, 32);
	next_offset = src_surf_offset[src_surf] + sizeof(struct brw_surface_state);
    }
    
    binding_table_offset = ALIGN(next_offset, 32);
    next_offset = binding_table_offset + (wm_binding_table_entries * 4);

    /* Allocate an area in framebuffer for our state layout we just set up */
    total_state_size = next_offset;
    assert (total_state_size < BRW_LINEAR_EXTRA);

    /*
     * Use the extra space allocated at the end of the Xv buffer
     */
    state_base_offset = pPriv->extra_offset;
    state_base_offset = ALIGN(state_base_offset, 64);

    state_base = (char *)(pI830->FbBase + state_base_offset);

    binding_table = (void *)(state_base + binding_table_offset);

#if 0
    ErrorF("wm:            0x%08x\n", state_base_offset + wm_offset);
    ErrorF("sf kernel:     0x%08x\n", state_base_offset + sf_kernel_offset);
    ErrorF("ps kernel:     0x%08x\n", state_base_offset + ps_kernel_offset);
    ErrorF("sip kernel:    0x%08x\n", state_base_offset + sip_kernel_offset);
    ErrorF("src sampler:   0x%08x\n", state_base_offset + sampler_offset);
    ErrorF("vb:            0x%08x\n", state_base_offset + vb_offset);
    ErrorF("dst surf:      0x%08x\n", state_base_offset + dest_surf_offset);
    ErrorF("src surf:      0x%08x\n", state_base_offset + src_surf_offset);
    ErrorF("binding table: 0x%08x\n", state_base_offset + binding_table_offset);
#endif

    urb_vs_start = 0;
    urb_vs_size = URB_VS_ENTRIES * URB_VS_ENTRY_SIZE;
    urb_gs_start = urb_vs_start + urb_vs_size;
    urb_gs_size = URB_GS_ENTRIES * URB_GS_ENTRY_SIZE;
    urb_clip_start = urb_gs_start + urb_gs_size;
    urb_clip_size = URB_CLIP_ENTRIES * URB_CLIP_ENTRY_SIZE;
    urb_sf_start = urb_clip_start + urb_clip_size;
    urb_sf_size = URB_SF_ENTRIES * URB_SF_ENTRY_SIZE;
    urb_cs_start = urb_sf_start + urb_sf_size;
    urb_cs_size = URB_CS_ENTRIES * URB_CS_ENTRY_SIZE;

    /* We'll be poking the state buffers that could be in use by the 3d
     * hardware here, but we should have synced the 3D engine already in
     * I830PutImage.
     */

    /* Upload kernels */
    memcpy(state_base + sip_kernel_offset, sip_kernel_static,
	   sizeof(sip_kernel_static));
    memcpy(state_base + sf_kernel_offset, sf_kernel_static,
	   sizeof(sf_kernel_static));
    memcpy(state_base + ps_kernel_offset, ps_kernel_static,
	   ps_kernel_static_size);

    i965_set_dst_surface_state(pScrn, (void *)(state_base +
					       dest_surf_offset),
			       pPixmap);

    for (src_surf = 0; src_surf < n_src_surf; src_surf++)
	i965_set_src_surface_state(pScrn,
				   (void *)(state_base +
					    src_surf_offset[src_surf]),
				   src_surf_base[src_surf],
				   src_width[src_surf],
				   src_height[src_surf],
				   src_pitch[src_surf],
				   src_surf_format);

    for (src_surf = 0; src_surf < n_src_surf; src_surf++)
	i965_set_sampler_state(pScrn, (void *)(state_base +
					       sampler_offset[src_surf]));

    /* Set up a binding table for our surfaces.  Only the PS will use it */
    binding_table[0] = state_base_offset + dest_surf_offset;
    for (src_surf = 0; src_surf < n_src_surf; src_surf++)
	binding_table[1 + src_surf] = state_base_offset + src_surf_offset[src_surf];

    if (pI830->video.gen4_vs_bo == NULL)
	pI830->video.gen4_vs_bo = i965_create_vs_state(pScrn);
    if (pI830->video.gen4_sf_bo == NULL)
	pI830->video.gen4_sf_bo = i965_create_sf_state(pScrn,
						       state_base_offset +
						       sf_kernel_offset);
    i965_set_wm_state(pScrn, (void *)(state_base + wm_offset),
		      state_base_offset + ps_kernel_offset,
		      state_base_offset + sampler_offset[0],
		      n_src_surf);

    if (pI830->video.gen4_cc_bo == NULL)
	pI830->video.gen4_cc_bo = i965_create_cc_state(pScrn);

    {
	BEGIN_BATCH(2);
	OUT_BATCH(MI_FLUSH |
		  MI_STATE_INSTRUCTION_CACHE_FLUSH |
		  BRW_MI_GLOBAL_SNAPSHOT_RESET);
	OUT_BATCH(MI_NOOP);
	ADVANCE_BATCH();
    }

    /* brw_debug (pScrn, "before base address modify"); */
    {
	BEGIN_BATCH(12);
	/* Match Mesa driver setup */
	if (IS_G4X(pI830))
	    OUT_BATCH(NEW_PIPELINE_SELECT | PIPELINE_SELECT_3D);
	else
	    OUT_BATCH(BRW_PIPELINE_SELECT | PIPELINE_SELECT_3D);

	/* Mesa does this. Who knows... */
	OUT_BATCH(BRW_CS_URB_STATE | 0);
	OUT_BATCH((0 << 4) |	/* URB Entry Allocation Size */
		  (0 << 0));	/* Number of URB Entries */

	/* Zero out the two base address registers so all offsets are
	 * absolute
	 */
	OUT_BATCH(BRW_STATE_BASE_ADDRESS | 4);
	OUT_BATCH(0 | BASE_ADDRESS_MODIFY);  /* Generate state base address */
	OUT_BATCH(0 | BASE_ADDRESS_MODIFY);  /* Surface state base address */
	OUT_BATCH(0 | BASE_ADDRESS_MODIFY);  /* media base addr, don't care */
	/* general state max addr, disabled */
	OUT_BATCH(0x10000000 | BASE_ADDRESS_MODIFY);
	/* media object state max addr, disabled */
	OUT_BATCH(0x10000000 | BASE_ADDRESS_MODIFY);

	/* Set system instruction pointer */
	OUT_BATCH(BRW_STATE_SIP | 0);
	/* system instruction pointer */
	OUT_BATCH(state_base_offset + sip_kernel_offset);

	OUT_BATCH(MI_NOOP);
	ADVANCE_BATCH();
    }

    /* brw_debug (pScrn, "after base address modify"); */

    {
       BEGIN_BATCH(42);
       /* Enable VF statistics */
       OUT_BATCH(BRW_3DSTATE_VF_STATISTICS | 1);

       /* Pipe control */
       OUT_BATCH(BRW_PIPE_CONTROL |
		 BRW_PIPE_CONTROL_NOWRITE |
		 BRW_PIPE_CONTROL_IS_FLUSH |
		 2);
       OUT_BATCH(0);			/* Destination address */
       OUT_BATCH(0);			/* Immediate data low DW */
       OUT_BATCH(0);			/* Immediate data high DW */

       /* Binding table pointers */
       OUT_BATCH(BRW_3DSTATE_BINDING_TABLE_POINTERS | 4);
       OUT_BATCH(0); /* vs */
       OUT_BATCH(0); /* gs */
       OUT_BATCH(0); /* clip */
       OUT_BATCH(0); /* sf */
       /* Only the PS uses the binding table */
       OUT_BATCH(state_base_offset + binding_table_offset); /* ps */

       /* Blend constant color (magenta is fun) */
       OUT_BATCH(BRW_3DSTATE_CONSTANT_COLOR | 3);
       OUT_BATCH(float_to_uint (1.0));
       OUT_BATCH(float_to_uint (0.0));
       OUT_BATCH(float_to_uint (1.0));
       OUT_BATCH(float_to_uint (1.0));

       /* The drawing rectangle clipping is always on.  Set it to values that
	* shouldn't do any clipping.
	*/
       OUT_BATCH(BRW_3DSTATE_DRAWING_RECTANGLE | 2); /* XXX 3 for BLC or CTG */
       OUT_BATCH(0x00000000);			/* ymin, xmin */
       OUT_BATCH((pScrn->virtualX - 1) |
		 (pScrn->virtualY - 1) << 16);	/* ymax, xmax */
       OUT_BATCH(0x00000000);			/* yorigin, xorigin */

       /* skip the depth buffer */
       /* skip the polygon stipple */
       /* skip the polygon stipple offset */
       /* skip the line stipple */

       /* Set the pointers to the 3d pipeline state */
       OUT_BATCH(BRW_3DSTATE_PIPELINED_POINTERS | 5);
       OUT_RELOC(pI830->video.gen4_vs_bo, I915_GEM_DOMAIN_INSTRUCTION, 0, 0);
       /* disable GS, resulting in passthrough */
       OUT_BATCH(BRW_GS_DISABLE);
       /* disable CLIP, resulting in passthrough */
       OUT_BATCH(BRW_CLIP_DISABLE);
       OUT_RELOC(pI830->video.gen4_sf_bo, I915_GEM_DOMAIN_INSTRUCTION, 0, 0);
       OUT_BATCH(state_base_offset + wm_offset);  /* 32 byte aligned */
       OUT_RELOC(pI830->video.gen4_cc_bo, I915_GEM_DOMAIN_INSTRUCTION, 0, 0);

       /* URB fence */
       OUT_BATCH(BRW_URB_FENCE |
		 UF0_CS_REALLOC |
		 UF0_SF_REALLOC |
		 UF0_CLIP_REALLOC |
		 UF0_GS_REALLOC |
		 UF0_VS_REALLOC |
		 1);
       OUT_BATCH(((urb_clip_start + urb_clip_size) << UF1_CLIP_FENCE_SHIFT) |
		 ((urb_gs_start + urb_gs_size) << UF1_GS_FENCE_SHIFT) |
		 ((urb_vs_start + urb_vs_size) << UF1_VS_FENCE_SHIFT));
       OUT_BATCH(((urb_cs_start + urb_cs_size) << UF2_CS_FENCE_SHIFT) |
		 ((urb_sf_start + urb_sf_size) << UF2_SF_FENCE_SHIFT));

       /* Constant buffer state */
       OUT_BATCH(BRW_CS_URB_STATE | 0);
       OUT_BATCH(((URB_CS_ENTRY_SIZE - 1) << 4) |
		 (URB_CS_ENTRIES << 0));

       /* Set up the pointer to our vertex buffer */
       OUT_BATCH(BRW_3DSTATE_VERTEX_BUFFERS | 2);
       /* four 32-bit floats per vertex */
       OUT_BATCH((0 << VB0_BUFFER_INDEX_SHIFT) |
		 VB0_VERTEXDATA |
		 ((4 * 4) << VB0_BUFFER_PITCH_SHIFT));
       OUT_BATCH(state_base_offset + vb_offset);
       OUT_BATCH(3); /* four corners to our rectangle */

       /* Set up our vertex elements, sourced from the single vertex buffer. */
       OUT_BATCH(BRW_3DSTATE_VERTEX_ELEMENTS | 3);
       /* offset 0: X,Y -> {X, Y, 1.0, 1.0} */
       OUT_BATCH((0 << VE0_VERTEX_BUFFER_INDEX_SHIFT) |
		 VE0_VALID |
		 (BRW_SURFACEFORMAT_R32G32_FLOAT << VE0_FORMAT_SHIFT) |
		 (0 << VE0_OFFSET_SHIFT));
       OUT_BATCH((BRW_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT) |
		 (BRW_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT) |
		 (BRW_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_2_SHIFT) |
		 (BRW_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_3_SHIFT) |
		 (0 << VE1_DESTINATION_ELEMENT_OFFSET_SHIFT));
       /* offset 8: S0, T0 -> {S0, T0, 1.0, 1.0} */
       OUT_BATCH((0 << VE0_VERTEX_BUFFER_INDEX_SHIFT) |
		 VE0_VALID |
		 (BRW_SURFACEFORMAT_R32G32_FLOAT << VE0_FORMAT_SHIFT) |
		 (8 << VE0_OFFSET_SHIFT));
       OUT_BATCH((BRW_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT) |
		 (BRW_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT) |
		 (BRW_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_2_SHIFT) |
		 (BRW_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_3_SHIFT) |
		 (4 << VE1_DESTINATION_ELEMENT_OFFSET_SHIFT));

       OUT_BATCH(MI_NOOP);			/* pad to quadword */
       ADVANCE_BATCH();
    }

   /* Set up the offset for translating from the given region (in screen
    * coordinates) to the backing pixmap.
    */
#ifdef COMPOSITE
    pix_xoff = -pPixmap->screen_x + pPixmap->drawable.x;
    pix_yoff = -pPixmap->screen_y + pPixmap->drawable.y;
#else
    pix_xoff = 0;
    pix_yoff = 0;
#endif

    dxo = dstRegion->extents.x1;
    dyo = dstRegion->extents.y1;

    /* Use normalized texture coordinates */
    src_scale_x = ((float)src_w / width) / (float)drw_w;
    src_scale_y = ((float)src_h / height) / (float)drw_h;

    pbox = REGION_RECTS(dstRegion);
    nbox = REGION_NUM_RECTS(dstRegion);
    while (nbox--) {
	int box_x1 = pbox->x1;
	int box_y1 = pbox->y1;
	int box_x2 = pbox->x2;
	int box_y2 = pbox->y2;
	int i;
	float *vb;

	if (!first_output) {
	    /* Since we use the same little vertex buffer over and over, sync
	     * for subsequent rectangles.
	     */
	    i830WaitSync(pScrn);
	}

	pbox++;

	vb = (void *)(state_base + vb_offset);
	i = 0;
	vb[i++] = (box_x2 - dxo) * src_scale_x;
	vb[i++] = (box_y2 - dyo) * src_scale_y;
	vb[i++] = (float) box_x2 + pix_xoff;
	vb[i++] = (float) box_y2 + pix_yoff;

	vb[i++] = (box_x1 - dxo) * src_scale_x;
	vb[i++] = (box_y2 - dyo) * src_scale_y;
	vb[i++] = (float) box_x1 + pix_xoff;
	vb[i++] = (float) box_y2 + pix_yoff;

	vb[i++] = (box_x1 - dxo) * src_scale_x;
	vb[i++] = (box_y1 - dyo) * src_scale_y;
	vb[i++] = (float) box_x1 + pix_xoff;
	vb[i++] = (float) box_y1 + pix_yoff;

	i965_pre_draw_debug(pScrn);

	BEGIN_BATCH(6);
	OUT_BATCH(BRW_3DPRIMITIVE |
		  BRW_3DPRIMITIVE_VERTEX_SEQUENTIAL |
		  (_3DPRIM_RECTLIST << BRW_3DPRIMITIVE_TOPOLOGY_SHIFT) |
		  (0 << 9) |  /* CTG - indirect vertex count */
		  4);
	OUT_BATCH(3); /* vertex count per instance */
	OUT_BATCH(0); /* start vertex offset */
	OUT_BATCH(1); /* single instance */
	OUT_BATCH(0); /* start instance location */
	OUT_BATCH(0); /* index buffer offset, ignored */
	ADVANCE_BATCH();

	i965_post_draw_debug(pScrn);

	first_output = FALSE;
	i830MarkSync(pScrn);
    }

#if WATCH_STATS
    i830_dump_error_state(pScrn);
#endif
}

void
i965_free_video(ScrnInfoPtr scrn)
{
    I830Ptr pI830 = I830PTR(scrn);

    drm_intel_bo_unreference(pI830->video.gen4_vs_bo);
    pI830->video.gen4_vs_bo = NULL;
    drm_intel_bo_unreference(pI830->video.gen4_sf_bo);
    pI830->video.gen4_sf_bo = NULL;
    drm_intel_bo_unreference(pI830->video.gen4_cc_bo);
    pI830->video.gen4_cc_bo = NULL;
    drm_intel_bo_unreference(pI830->video.gen4_cc_vp_bo);
    pI830->video.gen4_cc_vp_bo = NULL;
}
