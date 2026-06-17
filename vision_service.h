#ifndef VISION_SERVICE_H
#define VISION_SERVICE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
  VISION_RESULT_OK = 0,
  VISION_RESULT_NO_CAMERA,
  VISION_RESULT_NO_WIFI,
  VISION_RESULT_NO_API,
  VISION_RESULT_UNSUPPORTED,
  VISION_RESULT_CAPTURE_FAIL,
  VISION_RESULT_HTTP_FAIL,
  VISION_RESULT_PARSE_FAIL,
} VisionResult;

VisionResult vision_service_describe_camera(char *outText, size_t outLen);
VisionResult vision_service_describe_jpeg(const uint8_t *jpeg, size_t jpegLen, char *outText, size_t outLen);
VisionResult vision_service_book_answer_jpeg(const uint8_t *jpeg, size_t jpegLen, char *outText, size_t outLen);

#endif
