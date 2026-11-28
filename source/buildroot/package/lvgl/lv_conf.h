/* LVGL configuration for the Toons: Toon 2 (1024x600 XRGB8888 framebuffer,
 * Cortex-A9 with NEON) and Toon 1 (800x480 RGB565, ARM926 without NEON).
 * The colour depth is set by the package (BR2_PACKAGE_LVGL_COLOR_DEPTH_16
 * writes 16 here when the file is copied into the build). Anything not set
 * here keeps the LVGL default. */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 32

#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

#define LV_DEF_REFR_PERIOD 25
#define LV_DPI_DEF 130

#define LV_USE_OS LV_OS_NONE
#define LV_DRAW_SW_DRAW_UNIT_CNT 1
#ifdef __ARM_NEON
#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NEON
#else
#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE
#endif
#define LV_DRAW_SW_COMPLEX 1
#define LV_DRAW_SW_SHADOW_CACHE_SIZE 0

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_TINY_TTF 1
#define LV_TINY_TTF_FILE_SUPPORT 0
#define LV_TINY_TTF_CACHE_GLYPH_CNT 512

/* the info screen's QR code that opens the web interface logged in */
#define LV_USE_QRCODE 1

/* apps' el.image: JPEG and PNG, decoded by toon-ui itself (apps.c) */
#define LV_USE_TJPGD 1
#define LV_USE_LODEPNG 1

#define LV_USE_SNAPSHOT 1
#define LV_USE_SYSMON 0
#define LV_USE_PROFILER 0

#define LV_USE_LINUX_FBDEV 1
#define LV_LINUX_FBDEV_BSD 0
#define LV_LINUX_FBDEV_RENDER_MODE LV_DISPLAY_RENDER_MODE_PARTIAL
#define LV_LINUX_FBDEV_BUFFER_COUNT 0
#define LV_LINUX_FBDEV_BUFFER_SIZE 60
#define LV_USE_EVDEV 1

#define LV_BUILD_EXAMPLES 0

#endif
