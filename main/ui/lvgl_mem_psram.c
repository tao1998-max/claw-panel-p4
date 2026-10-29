/*
 * LVGL allocator backed by PSRAM.
 *
 * The P4 panel has 32 MB PSRAM and a small internal heap. LVGL widgets are not
 * DMA descriptors; keeping their object/style/event memory in PSRAM leaves
 * internal RAM for the Wi-Fi, audio, Agent and Lua stacks. SOC_PSRAM_DMA_CAPABLE
 * is set on ESP32-P4, so this is also safe for any LVGL buffer that ends up
 * being handed to a DMA-capable path.
 */
#include "lvgl.h"

#include "esp_heap_caps.h"

/* Pull this object out of the archive from app_start(): a static library
 * member that only defines symbols referenced by another library is not
 * necessarily linked. */
void lvgl_mem_psram_ensure_linked(void)
{
}

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

void *lv_malloc_core(size_t size)
{
    if (size == 0) {
        return NULL;
    }
    return heap_caps_malloc_prefer(size, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    if (new_size == 0) {
        heap_caps_free(p);
        return NULL;
    }
    return heap_caps_realloc_prefer(p, new_size, 2,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    if (mon_p == NULL) {
        return;
    }
    size_t total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t free_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    mon_p->total_size = total;
    mon_p->free_size = free_size;
    mon_p->free_biggest_size = largest;
    mon_p->used_pct = total ? (uint8_t)(100 - (free_size * 100 / total)) : 0;
    mon_p->frag_pct = free_size ? (uint8_t)(100 - (largest * 100 / free_size)) : 0;
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}
