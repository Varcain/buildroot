/*
 * oveRTOS DMA2D HAL shim for the guest LVGL DMA2D draw unit.
 *
 * LVGL's lv_draw_dma2d_private.h does `#include LV_DRAW_DMA2D_HAL_INCLUDE` to pull
 * in the STM32 CMSIS register map (DMA2D->, RCC, NVIC, SCB). The oveRTOS overlay
 * lv_draw_dma2d.c drives no registers — it forwards each transfer to /dev/dma2d by
 * ioctl and the privileged coordinator owns the peripheral — so this header is
 * intentionally empty: it only satisfies the include on a target with no CMSIS HAL.
 */

#ifndef OVE_DMA2D_HAL_SHIM_H
#define OVE_DMA2D_HAL_SHIM_H

#include <stdint.h>

#endif /* OVE_DMA2D_HAL_SHIM_H */
