#ifndef CAMERA_SERVICE_H
#define CAMERA_SERVICE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_camera.h"

#define CAMERA_PREVIEW_WIDTH 100
#define CAMERA_PREVIEW_HEIGHT 100
#define CAMERA_PREVIEW_BYTES ((CAMERA_PREVIEW_WIDTH * CAMERA_PREVIEW_HEIGHT + 7) / 8)

bool camera_service_init(void);
bool camera_service_is_ready(void);

/** Power down sensor/DMA (during HTTPS or when idle) to avoid heat and FB-OVF. */
void camera_service_pause(void);

camera_fb_t *camera_service_capture(void);
void camera_service_release(camera_fb_t *fb);

framesize_t camera_service_get_framesize(void);
bool camera_service_set_framesize(framesize_t size);

/**
 * Decode a captured frame to RGB888. Allocates from PSRAM when available.
 * Caller must free(*outRgb) with free().
 */
bool camera_service_frame_to_rgb888(const camera_fb_t *fb, uint8_t **outRgb, size_t *outSize);

/**
 * Convert a captured frame to a 100x100 1bpp preview bitmap.
 * Output bit value follows e-paper convention: 1 = white, 0 = black.
 */
bool camera_service_frame_to_mono_preview100(const camera_fb_t *fb, uint8_t *outBits, size_t outBitsLen);

/**
 * Convert a captured frame to a 100x100 1bpp mosaic preview bitmap.
 * blockPx is in preview pixels; 8-12 gives a medium mosaic on the e-paper UI.
 */
bool camera_service_frame_to_mosaic_preview100(const camera_fb_t *fb, uint8_t *outBits,
                                               size_t outBitsLen, uint8_t blockPx);

/**
 * Decode a captured frame, apply RGB block mosaic, then re-encode as JPEG.
 * Caller must free(*outJpeg) with free().
 */
bool camera_service_frame_to_mosaic_jpeg(const camera_fb_t *fb, uint8_t blockPx,
                                         uint8_t jpegQuality, uint8_t **outJpeg,
                                         size_t *outLen);

#endif
