/**
 * @file lv_draw_dma2d.c  (oveRTOS overlay)
 *
 * The stock LVGL DMA2D draw unit drives the peripheral with privileged MMIO
 * (DMA2D->..., RCC, NVIC, SCB), which an unprivileged FDPIC guest cannot do.
 * This overlay keeps the unit's pure-C front end (evaluate_cb / dispatch_cb and
 * the fill/image descriptor builders) but routes the single execute seam,
 * lv_draw_dma2d_configure_and_start_transfer(), through one ioctl on /dev/dma2d:
 * the oveRTOS coordinator validates every address against the guest region, owns
 * the DMA2D clock/registers/IRQ and the D-cache coherency, and runs the transfer
 * synchronously. init/deinit/completion/cache therefore become fd + no-op glue.
 */

#include "lv_draw_dma2d_private.h"
#if LV_USE_DRAW_DMA2D

#include "../sw/lv_draw_sw.h"
#include "../sw/blend/lv_draw_sw_blend_private.h" /* lv_draw_sw_blend_dsc_t (glyph SW fallback) */
#include "../../misc/lv_area_private.h"
/* Phase D (text): the LABEL glyph-iteration hook + glyph/font/draw-buf accessors. */
#include "../lv_draw_label.h"
#include "../lv_draw_label_private.h"
#include "../lv_draw_image.h"
#include "../lv_draw_rect.h"
#include "../lv_draw_buf.h"
#include "../../font/lv_font.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/*********************
 *      DEFINES
 *********************/

#define DRAW_UNIT_ID_DMA2D 5

/* /dev/dma2d ABI (mirror of the coordinator's lxp_uapi.h). Fixed-width fields so
 * the layout matches the ARM 32-bit kernel side exactly. */
#define LXP_DMA2D_SUBMIT _IOW('D', 1, struct lxp_dma2d_submit)
#define LXP_DMA2D_SUBMIT_BATCH _IOW('D', 2, struct lxp_dma2d_batch)

struct lxp_dma2d_submit {
    uint32_t mode;
    uint32_t w, h;
    uint32_t output_address, output_offset;
    uint32_t output_cf;
    uint32_t reg_to_mem_color;
    uint32_t fg_address, fg_offset;
    uint32_t fg_cf;
    uint32_t fg_color;
    uint32_t fg_alpha_mode, fg_alpha;
    uint32_t bg_address, bg_offset;
    uint32_t bg_cf;
    uint32_t bg_color;
    uint32_t bg_alpha_mode, bg_alpha;
};

/* N glyph descriptors submitted in ONE ioctl so a whole text run crosses the SVC
 * boundary once (the per-glyph round-trip, not the transfer, is what sinks tiny
 * glyphs one at a time). @ops is a pointer, zero-extended to 64-bit for a layout
 * identical on the 32-bit guest and the coordinator. */
struct lxp_dma2d_batch {
    uint32_t count;
    uint32_t _reserved;
    uint64_t ops;
};

/* /dev/dma2d ABI enum values used directly when hand-building a glyph descriptor. */
#define ABI_MODE_BLEND_FG 3u /* fg (A8) over bg -> output, fixed fg colour */
#define ABI_CF_A8 9u
#define ABI_AM_NONE 0u    /* alpha = the A8 coverage value */
#define ABI_AM_COMBINE 2u /* alpha = A8 * fg_alpha / 255 (applies label opacity) */

/* Batch sizing. Each glyph's decoded A8 coverage is copied into the staging arena
 * (the font engine decodes every glyph into one SHARED scratch buffer, so batched
 * descriptors must point at private, persistent copies), then flushed together. */
#define DMA2D_BATCH_N 64u
#define DMA2D_STAGE_SIZE (16u * 1024u)

/**********************
 *  STATIC PROTOTYPES
 **********************/

static int32_t evaluate_cb(lv_draw_unit_t * draw_unit, lv_draw_task_t * task);
static int32_t dispatch_cb(lv_draw_unit_t * draw_unit, lv_layer_t * layer);
static int32_t delete_cb(lv_draw_unit_t * draw_unit);
static bool check_transfer_completion(void);
static void post_transfer_tasks(lv_draw_dma2d_unit_t * u);
static void dma2d_batch_flush(void);
static bool dma2d_push_glyph(lv_draw_task_t * t, lv_draw_glyph_dsc_t * gd, const lv_draw_buf_t * db);
static void dma2d_glyph_cb(lv_draw_task_t * t, lv_draw_glyph_dsc_t * gd,
                           lv_draw_fill_dsc_t * fill_dsc, const lv_area_t * fill_area);

/**********************
 *  STATIC VARIABLES
 **********************/

static int g_dma2d_fd = -1;

/* Text-offload batch state (guest-side, single-threaded: one label at a time). */
static struct lxp_dma2d_submit g_batch[DMA2D_BATCH_N];
static uint8_t g_stage[DMA2D_STAGE_SIZE] __attribute__((aligned(4)));
static uint32_t g_batch_n;
static uint32_t g_stage_off;
static lv_layer_t * g_cur_layer; /* target layer for the label being iterated */
/* Text (glyph) offload is opt-in (OVE_DMA2D_TEXT=1) and OFF by default — NOT because
 * it fails (it renders correctly: real F746 runs all 16 lvbench scenes, 64902 glyphs,
 * 0 sw fallback) but because it is a PERF WASH: the batched A8 blends run render
 * 39 -> 41 ms (every text scene slightly worse). The per-label SVC round-trip plus the
 * per-glyph staging copy outweigh the HW blend, and the render is memory-bound anyway.
 * (The earlier "transfers do not complete" note was a misdiagnosis — a too-short
 * host-side stall timeout on a slow first scene + GDB halts killing the guest.) */
static int g_text_on;

/* Diagnostics (read + printed by main.c): how much text actually went to DMA2D. */
uint32_t ove_dma2d_label_tasks;
uint32_t ove_dma2d_glyphs;
uint32_t ove_dma2d_batches;
uint32_t ove_dma2d_glyph_fallback;

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_draw_dma2d_init(void)
{
    /* A/B escape hatch: OVE_DMA2D=0 forces the pure-software path (device never
     * opened, unit never registered) so a single boot can measure sw-vs-DMA2D on
     * the same firmware + same binary. */
    const char * en = getenv("OVE_DMA2D");
    if(en && en[0] == '0') {
        return;
    }

    /* Text/glyph offload is opt-in — it renders correctly but is a perf wash, so it is
     * off unless explicitly requested (see g_text_on). */
    const char * te = getenv("OVE_DMA2D_TEXT");
    g_text_on = (te && te[0] == '1');

    /* Only register the unit if the accelerator device is present; otherwise leave
     * g_dma2d_fd < 0 and every task falls through to the software unit. */
    g_dma2d_fd = open("/dev/dma2d", O_RDWR);
    if(g_dma2d_fd < 0) {
        return;
    }

    lv_draw_dma2d_unit_t * draw_dma2d_unit = lv_draw_create_unit(sizeof(lv_draw_dma2d_unit_t));
    draw_dma2d_unit->base_unit.evaluate_cb = evaluate_cb;
    draw_dma2d_unit->base_unit.dispatch_cb = dispatch_cb;
    draw_dma2d_unit->base_unit.delete_cb = delete_cb;
    draw_dma2d_unit->base_unit.name = "DMA2D";
    /* No clock / IRQ setup — the coordinator owns the peripheral. */
}

void lv_draw_dma2d_deinit(void)
{
    if(g_dma2d_fd >= 0) {
        close(g_dma2d_fd);
        g_dma2d_fd = -1;
    }
}

#if LV_USE_DRAW_DMA2D_INTERRUPT
void lv_draw_dma2d_transfer_complete_interrupt_handler(void)
{
    /* Unused: transfers complete synchronously inside the submit ioctl. */
}
#endif

lv_draw_dma2d_output_cf_t lv_draw_dma2d_cf_to_dma2d_output_cf(lv_color_format_t cf)
{
    switch(cf) {
        case LV_COLOR_FORMAT_ARGB8888:
        case LV_COLOR_FORMAT_XRGB8888:
            return LV_DRAW_DMA2D_OUTPUT_CF_ARGB8888;
        case LV_COLOR_FORMAT_RGB888:
            return LV_DRAW_DMA2D_OUTPUT_CF_RGB888;
        case LV_COLOR_FORMAT_RGB565:
            return LV_DRAW_DMA2D_OUTPUT_CF_RGB565;
        case LV_COLOR_FORMAT_ARGB1555:
            return LV_DRAW_DMA2D_OUTPUT_CF_ARGB1555;
        default:
            LV_ASSERT_MSG(false, "unsupported output color format");
    }
    return LV_DRAW_DMA2D_OUTPUT_CF_RGB565;
}

uint32_t lv_draw_dma2d_color_to_dma2d_color(lv_draw_dma2d_output_cf_t cf, lv_color_t color)
{
    switch(cf) {
        case LV_DRAW_DMA2D_OUTPUT_CF_ARGB8888:
        case LV_DRAW_DMA2D_OUTPUT_CF_RGB888:
            return lv_color_to_u32(color);
        case LV_DRAW_DMA2D_OUTPUT_CF_RGB565:
            return lv_color_to_u16(color);
        default:
            LV_ASSERT_MSG(false, "unsupported output color format");
    }
    return 0;
}

void lv_draw_dma2d_configure_and_start_transfer(const lv_draw_dma2d_configuration_t * conf)
{
    if(g_dma2d_fd < 0) {
        return;
    }

    /* LVGL's mode is the raw DMA2D CR.MODE value {M2M,PFC,BLEND,R2M,BLEND_FG,BLEND_BG};
     * the /dev/dma2d ABI orders R2M last (=4) and folds fixed-fg/bg into blend-fg (=3). */
    static const uint32_t mode_map[6] = {0u, 1u, 2u, 4u, 3u, 3u};

    struct lxp_dma2d_submit s;
    memset(&s, 0, sizeof(s));
    s.mode = mode_map[((unsigned) conf->mode) % 6u];
    s.w = conf->w;
    s.h = conf->h;
    s.output_address = (uint32_t)(uintptr_t) conf->output_address;
    s.output_offset = conf->output_offset;
    s.output_cf = (uint32_t) conf->output_cf;
    s.reg_to_mem_color = conf->reg_to_mem_mode_color;
    s.fg_address = (uint32_t)(uintptr_t) conf->fg_address;
    s.fg_offset = conf->fg_offset;
    s.fg_cf = (uint32_t) conf->fg_cf;
    s.fg_color = conf->fg_color;
    s.fg_alpha_mode = conf->fg_alpha_mode;
    s.fg_alpha = conf->fg_alpha;
    s.bg_address = (uint32_t)(uintptr_t) conf->bg_address;
    s.bg_offset = conf->bg_offset;
    s.bg_cf = (uint32_t) conf->bg_cf;
    s.bg_color = conf->bg_color;
    s.bg_alpha_mode = conf->bg_alpha_mode;
    s.bg_alpha = conf->bg_alpha;

    /* Synchronous: the coordinator programs DMA2D + polls to completion + owns the
     * D-cache maintenance before returning. On failure the task is simply not drawn
     * by hardware (the pixels stay whatever they were); rare and self-correcting. */
    (void) ioctl(g_dma2d_fd, LXP_DMA2D_SUBMIT, &s);
}

#if LV_DRAW_DMA2D_CACHE
void lv_draw_dma2d_invalidate_cache(const lv_draw_dma2d_cache_area_t * mem_area)
{
    /* The coordinator invalidates the output region after the transfer. */
    (void) mem_area;
}

void lv_draw_dma2d_clean_cache(const lv_draw_dma2d_cache_area_t * mem_area)
{
    /* The coordinator cleans the source/output regions before the transfer. */
    (void) mem_area;
}
#endif

/**********************
 *   STATIC FUNCTIONS
 **********************/

/* Submit the accumulated glyph descriptors in one ioctl, then reset the batch and
 * the staging arena. Safe to call when empty. */
static void dma2d_batch_flush(void)
{
    if(g_batch_n > 0 && g_dma2d_fd >= 0) {
        struct lxp_dma2d_batch b;
        b.count = g_batch_n;
        b._reserved = 0;
        b.ops = (uint64_t)(uintptr_t) g_batch;
        (void) ioctl(g_dma2d_fd, LXP_DMA2D_SUBMIT_BATCH, &b);
        ove_dma2d_batches++;
    }
    g_batch_n = 0;
    g_stage_off = 0;
}

/* Queue one glyph as a fixed-colour-fg DMA2D blend (A8 coverage over the draw
 * buffer). Returns false only when DMA2D can't take it (unsupported layer format
 * or a glyph too large for the staging arena) so the caller blends it in software;
 * a fully-clipped glyph returns true (nothing to draw). */
static bool dma2d_push_glyph(lv_draw_task_t * t, lv_draw_glyph_dsc_t * gd, const lv_draw_buf_t * db)
{
    lv_layer_t * layer = g_cur_layer;
    if(layer == NULL) {
        return false;
    }
    lv_color_format_t cf = layer->color_format;
    if(!(cf == LV_COLOR_FORMAT_ARGB8888 || cf == LV_COLOR_FORMAT_XRGB8888
         || cf == LV_COLOR_FORMAT_RGB888 || cf == LV_COLOR_FORMAT_RGB565)) {
        return false;
    }

    const lv_area_t * letter = gd->letter_coords;
    lv_area_t clipped;
    if(!lv_area_intersect(&clipped, letter, &t->clip_area)) {
        return true; /* off the clip: handled, nothing to blend */
    }
    if(!lv_area_intersect(&clipped, &clipped, &layer->buf_area)) {
        return true;
    }
    int32_t w = lv_area_get_width(&clipped);
    int32_t h = lv_area_get_height(&clipped);
    if(w <= 0 || h <= 0) {
        return true;
    }

    uint32_t need = (uint32_t) w * (uint32_t) h; /* A8 copy, tight (1 byte/px) */
    uint32_t need_al = (need + 3u) & ~3u;        /* keep every fg_address 4-byte aligned */
    if(need_al > DMA2D_STAGE_SIZE) {
        return false; /* implausibly large glyph → let software handle it */
    }
    if(g_batch_n >= DMA2D_BATCH_N || g_stage_off + need_al > DMA2D_STAGE_SIZE) {
        dma2d_batch_flush();
    }

    /* The font engine decodes every glyph into one shared scratch buffer, so copy
     * this glyph's (clipped) coverage into private staging that outlives the batch. */
    int32_t dx = clipped.x1 - letter->x1;
    int32_t dy = clipped.y1 - letter->y1;
    const uint8_t * src = (const uint8_t *) db->data + (int32_t) dy * db->header.stride + dx;
    uint8_t * dst = g_stage + g_stage_off;
    for(int32_t row = 0; row < h; row++) {
        memcpy(dst + (int32_t) row * w, src + (int32_t) row * db->header.stride, (size_t) w);
    }

    uint32_t out_cf = (uint32_t) lv_draw_dma2d_cf_to_dma2d_output_cf(cf);
    uint32_t cf_size = LV_COLOR_FORMAT_GET_SIZE(cf);
    int32_t buf_stride = lv_draw_buf_width_to_stride(lv_area_get_width(&layer->buf_area), cf);
    void * dest = lv_draw_layer_go_to_xy(layer,
                                         clipped.x1 - layer->buf_area.x1,
                                         clipped.y1 - layer->buf_area.y1);

    struct lxp_dma2d_submit * s = &g_batch[g_batch_n++];
    memset(s, 0, sizeof(*s));
    s->mode = ABI_MODE_BLEND_FG;
    s->w = (uint32_t) w;
    s->h = (uint32_t) h;
    s->output_address = (uint32_t)(uintptr_t) dest;
    s->output_offset = (uint32_t)((buf_stride / (int32_t) cf_size) - w);
    s->output_cf = out_cf;
    s->fg_address = (uint32_t)(uintptr_t) dst;
    s->fg_offset = 0; /* staged tightly, no inter-line gap */
    s->fg_cf = ABI_CF_A8;
    s->fg_color = lv_color_to_u32(gd->color);
    if(gd->opa >= LV_OPA_MAX) {
        s->fg_alpha_mode = ABI_AM_NONE;
        s->fg_alpha = 255;
    }
    else {
        s->fg_alpha_mode = ABI_AM_COMBINE; /* alpha = coverage * opa / 255 */
        s->fg_alpha = gd->opa;
    }
    s->bg_address = s->output_address; /* blend onto the existing pixels */
    s->bg_offset = s->output_offset;
    s->bg_cf = out_cf;

    g_stage_off += need_al;
    ove_dma2d_glyphs++;
    return true;
}

/* Per-glyph callback for lv_draw_label_iterate_characters(). Mirrors the software
 * unit's draw_letter_cb, but routes the common case (a normal, non-rotated A1..A8
 * glyph decoded to an A8 map in guest heap) through the DMA2D batch. Everything the
 * accelerator can't do (static-bitmap fonts in flash, rotated/IMAGE glyphs, the
 * placeholder box, and text decorations) falls back to the software draw, after
 * flushing the batch so draw order is preserved. */
static void dma2d_glyph_cb(lv_draw_task_t * t, lv_draw_glyph_dsc_t * gd,
                           lv_draw_fill_dsc_t * fill_dsc, const lv_area_t * fill_area)
{
    if(gd) {
        switch(gd->format) {
            case LV_FONT_GLYPH_FORMAT_NONE:
#if LV_USE_FONT_PLACEHOLDER
                if(gd->bg_coords) {
                    dma2d_batch_flush();
                    lv_draw_border_dsc_t bd;
                    lv_draw_border_dsc_init(&bd);
                    bd.opa = gd->opa;
                    bd.color = gd->color;
                    bd.width = 1;
                    lv_draw_sw_border(t, &bd, gd->bg_coords);
                }
#endif
                break;
            case LV_FONT_GLYPH_FORMAT_A1:
            case LV_FONT_GLYPH_FORMAT_A2:
            case LV_FONT_GLYPH_FORMAT_A3:
            case LV_FONT_GLYPH_FORMAT_A4:
            case LV_FONT_GLYPH_FORMAT_A8:
            case LV_FONT_GLYPH_FORMAT_IMAGE:
                if(gd->rotation % 3600 == 0 && gd->format != LV_FONT_GLYPH_FORMAT_IMAGE) {
                    if(lv_font_has_static_bitmap(gd->g->resolved_font)
                       && gd->g->format == LV_FONT_GLYPH_FORMAT_A8) {
                        /* static coverage lives in flash — outside the guest region a
                         * DMA engine may touch — so blend it in software. */
                        gd->g->req_raw_bitmap = 1;
                        const void * bitmap = lv_font_get_glyph_static_bitmap(gd->g);
                        lv_area_t mask_area = *gd->letter_coords;
                        lv_draw_sw_blend_dsc_t blend_dsc;
                        memset(&blend_dsc, 0, sizeof(blend_dsc));
                        blend_dsc.color = gd->color;
                        blend_dsc.opa = gd->opa;
                        blend_dsc.mask_buf = bitmap;
                        blend_dsc.mask_area = &mask_area;
                        blend_dsc.mask_stride = gd->g->stride;
                        blend_dsc.blend_area = gd->letter_coords;
                        blend_dsc.mask_res = LV_DRAW_SW_MASK_RES_CHANGED;
                        dma2d_batch_flush();
                        lv_draw_sw_blend(t, &blend_dsc);
                    }
                    else {
                        gd->glyph_data = lv_font_get_glyph_bitmap(gd->g, gd->_draw_buf);
                        if(gd->glyph_data == NULL) {
                            break;
                        }
                        const lv_draw_buf_t * draw_buf = gd->glyph_data;
                        if(!dma2d_push_glyph(t, gd, draw_buf)) {
                            /* accelerator declined — software blend, same result. */
                            ove_dma2d_glyph_fallback++;
                            lv_area_t mask_area = *gd->letter_coords;
                            mask_area.x2 = mask_area.x1
                                           + lv_draw_buf_width_to_stride(lv_area_get_width(&mask_area),
                                                                         LV_COLOR_FORMAT_A8) - 1;
                            lv_draw_sw_blend_dsc_t blend_dsc;
                            memset(&blend_dsc, 0, sizeof(blend_dsc));
                            blend_dsc.color = gd->color;
                            blend_dsc.opa = gd->opa;
                            blend_dsc.mask_buf = draw_buf->data;
                            blend_dsc.mask_area = &mask_area;
                            blend_dsc.mask_stride = draw_buf->header.stride;
                            blend_dsc.blend_area = gd->letter_coords;
                            blend_dsc.mask_res = LV_DRAW_SW_MASK_RES_CHANGED;
                            dma2d_batch_flush();
                            lv_draw_sw_blend(t, &blend_dsc);
                        }
                    }
                }
                else {
                    /* rotated or image glyph → software image path. */
                    gd->glyph_data = lv_font_get_glyph_bitmap(gd->g, gd->_draw_buf);
                    lv_draw_image_dsc_t img_dsc;
                    lv_draw_image_dsc_init(&img_dsc);
                    img_dsc.rotation = gd->rotation;
                    img_dsc.scale_x = LV_SCALE_NONE;
                    img_dsc.scale_y = LV_SCALE_NONE;
                    img_dsc.opa = gd->opa;
                    img_dsc.src = gd->glyph_data;
                    img_dsc.recolor = gd->color;
                    img_dsc.pivot.x = gd->pivot.x;
                    img_dsc.pivot.y = gd->g->box_h + gd->g->ofs_y;
                    dma2d_batch_flush();
                    lv_draw_sw_image(t, &img_dsc, gd->letter_coords);
                }
                break;
            default:
                break;
        }
    }

    if(fill_dsc && fill_area) {
        dma2d_batch_flush(); /* keep decorations in order with the glyphs */
        lv_draw_sw_fill(t, fill_dsc, fill_area);
    }
}

static int32_t evaluate_cb(lv_draw_unit_t * draw_unit, lv_draw_task_t * task)
{
    switch(task->type) {
        case LV_DRAW_TASK_TYPE_FILL: {
                lv_draw_fill_dsc_t * dsc = task->draw_dsc;
                if(!(dsc->radius == 0
                     && dsc->grad.dir == LV_GRAD_DIR_NONE
                     && (dsc->base.layer->color_format == LV_COLOR_FORMAT_ARGB8888
                         || dsc->base.layer->color_format == LV_COLOR_FORMAT_XRGB8888
                         || dsc->base.layer->color_format == LV_COLOR_FORMAT_RGB888
                         || dsc->base.layer->color_format == LV_COLOR_FORMAT_RGB565))) {
                    return 0;
                }
            }
            break;
        case LV_DRAW_TASK_TYPE_IMAGE: {
                lv_draw_image_dsc_t * dsc = task->draw_dsc;
                if(!(dsc->header.cf < LV_COLOR_FORMAT_PROPRIETARY_START
                     && dsc->clip_radius == 0
                     && dsc->bitmap_mask_src == NULL
                     && dsc->sup == NULL
                     && dsc->tile == 0
                     && dsc->blend_mode == LV_BLEND_MODE_NORMAL
                     && dsc->recolor_opa <= LV_OPA_MIN
                     && dsc->skew_y == 0
                     && dsc->skew_x == 0
                     && dsc->scale_x == 256
                     && dsc->scale_y == 256
                     && dsc->rotation == 0
                     && lv_image_src_get_type(dsc->src) == LV_IMAGE_SRC_VARIABLE
                     && (dsc->header.cf == LV_COLOR_FORMAT_ARGB8888
                         || dsc->header.cf == LV_COLOR_FORMAT_XRGB8888
                         || dsc->header.cf == LV_COLOR_FORMAT_RGB888
                         || dsc->header.cf == LV_COLOR_FORMAT_RGB565
                         || dsc->header.cf == LV_COLOR_FORMAT_ARGB1555)
                     && (dsc->base.layer->color_format == LV_COLOR_FORMAT_ARGB8888
                         || dsc->base.layer->color_format == LV_COLOR_FORMAT_XRGB8888
                         || dsc->base.layer->color_format == LV_COLOR_FORMAT_RGB888
                         || dsc->base.layer->color_format == LV_COLOR_FORMAT_RGB565))) {
                    return 0;
                }
            }
            break;
        case LV_DRAW_TASK_TYPE_LABEL: {
                if(!g_text_on) {
                    return 0; /* text offload opt-in (WIP); software draws the label */
                }
                /* Accept when the layer format is one DMA2D can write; per-glyph
                 * cases the accelerator can't handle fall back to software inside
                 * dma2d_glyph_cb, so this only gates on the destination format. */
                lv_draw_label_dsc_t * dsc = task->draw_dsc;
                lv_color_format_t cf = dsc->base.layer->color_format;
                if(!(cf == LV_COLOR_FORMAT_ARGB8888
                     || cf == LV_COLOR_FORMAT_XRGB8888
                     || cf == LV_COLOR_FORMAT_RGB888
                     || cf == LV_COLOR_FORMAT_RGB565)) {
                    return 0;
                }
            }
            break;
        default:
            return 0;
    }

    /* Only offload transfers large enough that the syscall round-trip is worth it;
     * tiny rects are cheaper in software. */
    if((int32_t) lv_area_get_size(&task->area) < 512) {
        return 0;
    }

    task->preferred_draw_unit_id = DRAW_UNIT_ID_DMA2D;
    task->preference_score = 0;

    return 0;
}

static int32_t dispatch_cb(lv_draw_unit_t * draw_unit, lv_layer_t * layer)
{
    lv_draw_dma2d_unit_t * draw_dma2d_unit = (lv_draw_dma2d_unit_t *) draw_unit;

    if(draw_dma2d_unit->task_act) {
        if(!check_transfer_completion()) {
            return LV_DRAW_UNIT_IDLE;
        }
        post_transfer_tasks(draw_dma2d_unit);
    }

    lv_draw_task_t * t = lv_draw_get_available_task(layer, NULL, DRAW_UNIT_ID_DMA2D);
    if(t == NULL) {
        return LV_DRAW_UNIT_IDLE;
    }

    void * buf = lv_draw_layer_alloc_buf(layer);
    if(buf == NULL) {
        return LV_DRAW_UNIT_IDLE;
    }

    t->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
    t->draw_unit = draw_unit;
    draw_dma2d_unit->task_act = t;

    if(t->type == LV_DRAW_TASK_TYPE_FILL) {
        lv_draw_fill_dsc_t * dsc = t->draw_dsc;
        const lv_area_t * coords = &t->area;
        lv_area_t clipped_coords;
        if(!lv_area_intersect(&clipped_coords, coords, &t->clip_area)) {
            return LV_DRAW_UNIT_IDLE;
        }

        void * dest = lv_draw_layer_go_to_xy(layer,
                                             clipped_coords.x1 - layer->buf_area.x1,
                                             clipped_coords.y1 - layer->buf_area.y1);

        if(dsc->opa >= LV_OPA_MAX) {
            lv_draw_dma2d_opaque_fill(t,
                                      dest,
                                      lv_area_get_width(&clipped_coords),
                                      lv_area_get_height(&clipped_coords),
                                      lv_draw_buf_width_to_stride(lv_area_get_width(&layer->buf_area), dsc->base.layer->color_format));
        }
        else {
            lv_draw_dma2d_fill(t,
                               dest,
                               lv_area_get_width(&clipped_coords),
                               lv_area_get_height(&clipped_coords),
                               lv_draw_buf_width_to_stride(lv_area_get_width(&layer->buf_area), dsc->base.layer->color_format));
        }
    }
    else if(t->type == LV_DRAW_TASK_TYPE_IMAGE) {
        lv_draw_image_dsc_t * dsc = t->draw_dsc;
        const lv_area_t * coords = &t->area;
        lv_area_t clipped_coords;
        if(!lv_area_intersect(&clipped_coords, coords, &t->clip_area)) {
            return LV_DRAW_UNIT_IDLE;
        }

        if(dsc->opa >= LV_OPA_MAX) {
            lv_draw_dma2d_opaque_image(t, dsc, &t->area);
        }
        else {
            lv_draw_dma2d_image(t, dsc, &t->area);
        }
    }
    else if(t->type == LV_DRAW_TASK_TYPE_LABEL) {
        /* Decompose the label into glyphs and batch each as an A8 blend; the
         * coordinator programs the whole run per ioctl (one SVC per label). */
        lv_draw_label_dsc_t * dsc = t->draw_dsc;
        ove_dma2d_label_tasks++;
        g_cur_layer = layer;
        g_batch_n = 0;
        g_stage_off = 0;
        lv_draw_label_iterate_characters(t, dsc, &t->area, dma2d_glyph_cb);
        dma2d_batch_flush();
    }

    lv_draw_dispatch_request();

    return 1;
}

static int32_t delete_cb(lv_draw_unit_t * draw_unit)
{
    LV_UNUSED(draw_unit);
    return 0;
}

static bool check_transfer_completion(void)
{
    /* The submit ioctl is synchronous, so a dispatched transfer is always done. */
    return true;
}

static void post_transfer_tasks(lv_draw_dma2d_unit_t * u)
{
    u->task_act->state = LV_DRAW_TASK_STATE_FINISHED;
    u->task_act = NULL;
}

#endif /*LV_USE_DRAW_DMA2D*/
