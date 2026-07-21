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
#include "../../misc/lv_area_private.h"

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

/**********************
 *  STATIC PROTOTYPES
 **********************/

static int32_t evaluate_cb(lv_draw_unit_t * draw_unit, lv_draw_task_t * task);
static int32_t dispatch_cb(lv_draw_unit_t * draw_unit, lv_layer_t * layer);
static int32_t delete_cb(lv_draw_unit_t * draw_unit);
static bool check_transfer_completion(void);
static void post_transfer_tasks(lv_draw_dma2d_unit_t * u);

/**********************
 *  STATIC VARIABLES
 **********************/

static int g_dma2d_fd = -1;

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
