#ifndef ORACLE_SERVICE_H
#define ORACLE_SERVICE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
  ORACLE_RESULT_OK = 0,
  ORACLE_RESULT_NO_CAMERA,
  ORACLE_RESULT_NO_WIFI,
  ORACLE_RESULT_NO_API,
  ORACLE_RESULT_UNSUPPORTED,
  ORACLE_RESULT_CAPTURE_FAIL,
  ORACLE_RESULT_HTTP_FAIL,
  ORACLE_RESULT_PARSE_FAIL,
  ORACLE_RESULT_LOCAL_FALLBACK,
} OracleResult;

/**
 * Ask the oracle for an answer.
 * Tries online Mode 1 (camera capture → AI oracle prompt) first.
 * Falls back to Mode 2 (local random from bookofanswers.h) on any failure.
 *
 * This is a blocking call (camera + HTTP). Run from a FreeRTOS task.
 *
 * @param outText  Output buffer for the answer text
 * @param outLen   Size of outText buffer
 * @return ORACLE_RESULT_OK if online answer obtained,
 *         ORACLE_RESULT_LOCAL_FALLBACK if local fallback used,
 *         or other error codes for specific failures
 */
OracleResult oracle_service_ask(char *outText, size_t outLen);
OracleResult oracle_service_ask_jpeg(const uint8_t *jpeg, size_t jpegLen,
                                     char *outText, size_t outLen);

#endif
