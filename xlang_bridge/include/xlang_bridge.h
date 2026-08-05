/*
 * Copyright (c) 2026 XLang Foundation
 * SPDX-License-Identifier: MIT
 *
 * Stable C ABI between Electron and the separately built XLang bridge.
 *
 * This boundary deliberately contains no C++ standard-library, XLang, Node,
 * or V8 types. Input views are borrowed for the duration of an API call.
 * Output views passed to callbacks are borrowed for the duration of that
 * callback. Object handles remain valid until release_handle() or shutdown().
 */

#ifndef ELECTRON_XLANG_BRIDGE_H_
#define ELECTRON_XLANG_BRIDGE_H_

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(XLANG_BRIDGE_IMPLEMENTATION)
#define XLANG_BRIDGE_EXPORT __declspec(dllexport)
#else
#define XLANG_BRIDGE_EXPORT __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define XLANG_BRIDGE_EXPORT __attribute__((visibility("default")))
#else
#define XLANG_BRIDGE_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define XLANG_BRIDGE_ABI_VERSION 2u

typedef uint64_t xlang_bridge_request_id;
typedef uint64_t xlang_bridge_handle;
typedef uint64_t xlang_bridge_subscription_id;

typedef struct xlang_bridge_bytes_view {
  const uint8_t* data;
  uint64_t size;
} xlang_bridge_bytes_view;

typedef enum xlang_bridge_status {
  XLANG_BRIDGE_STATUS_OK = 0,
  XLANG_BRIDGE_STATUS_INVALID_ARGUMENT = -1,
  XLANG_BRIDGE_STATUS_INCOMPATIBLE_ABI = -2,
  XLANG_BRIDGE_STATUS_NOT_INITIALIZED = -3,
  XLANG_BRIDGE_STATUS_ALREADY_INITIALIZED = -4,
  XLANG_BRIDGE_STATUS_ENGINE_LOAD_FAILED = -5,
  XLANG_BRIDGE_STATUS_IMPORT_FAILED = -6,
  XLANG_BRIDGE_STATUS_HANDLE_NOT_FOUND = -7,
  XLANG_BRIDGE_STATUS_MEMBER_NOT_FOUND = -8,
  XLANG_BRIDGE_STATUS_CALL_FAILED = -9,
  XLANG_BRIDGE_STATUS_UNSUPPORTED = -10,
  XLANG_BRIDGE_STATUS_SHUTTING_DOWN = -11,
  XLANG_BRIDGE_STATUS_INTERNAL_ERROR = -12,
  XLANG_BRIDGE_STATUS_EVENT_FAILED = -13
} xlang_bridge_status;

typedef enum xlang_bridge_value_type {
  XLANG_BRIDGE_VALUE_UNDEFINED = 0,
  XLANG_BRIDGE_VALUE_NULL = 1,
  XLANG_BRIDGE_VALUE_BOOL = 2,
  XLANG_BRIDGE_VALUE_INT64 = 3,
  XLANG_BRIDGE_VALUE_DOUBLE = 4,
  XLANG_BRIDGE_VALUE_STRING = 5,
  XLANG_BRIDGE_VALUE_BINARY = 6,
  XLANG_BRIDGE_VALUE_HANDLE = 7,
  XLANG_BRIDGE_VALUE_SUBSCRIPTION = 8
} xlang_bridge_value_type;

/*
 * For HANDLE values, flags contains the numeric X::ObjType. Consumers must
 * treat unknown values as ordinary opaque objects for forward compatibility.
 */
typedef struct xlang_bridge_value {
  uint32_t struct_size;
  uint32_t type;
  uint32_t flags;
  uint32_t reserved;
  union {
    int32_t boolean_value;
    int64_t int64_value;
    double double_value;
    xlang_bridge_handle handle_value;
    xlang_bridge_subscription_id subscription_value;
    xlang_bridge_bytes_view bytes_value;
  } data;
} xlang_bridge_value;

typedef struct xlang_bridge_named_value {
  xlang_bridge_bytes_view name;
  xlang_bridge_value value;
} xlang_bridge_named_value;

enum {
  XLANG_BRIDGE_INIT_ENABLE_PYTHON = 1u << 0,
  XLANG_BRIDGE_INIT_DEBUG = 1u << 1
};

typedef struct xlang_bridge_initialize_options {
  uint32_t struct_size;
  uint32_t flags;
  /* Absolute resources/xlang directory containing xlang_eng and modules. */
  xlang_bridge_bytes_view app_path;
  /*
   * Optional newline-separated additional module/library search directories.
   * app_path is always searched first.
   */
  xlang_bridge_bytes_view library_search_paths;
} xlang_bridge_initialize_options;

typedef void (*xlang_bridge_completion_callback)(
    void* user_data,
    xlang_bridge_request_id request_id,
    xlang_bridge_status status,
    const xlang_bridge_value* value,
    xlang_bridge_bytes_view error_message);

/*
 * Event callbacks can arrive on an XLang/LRPC thread. Every view is valid only
 * until this callback returns. HANDLE values remain valid afterward and must
 * eventually be released through release_handle().
 */
typedef void (*xlang_bridge_event_callback)(
    void* user_data,
    xlang_bridge_subscription_id subscription_id,
    const xlang_bridge_value* args,
    uint32_t arg_count,
    const xlang_bridge_named_value* kwargs,
    uint32_t kwarg_count);

typedef void (*xlang_bridge_log_callback)(void* user_data,
                                          int32_t level,
                                          xlang_bridge_bytes_view message);

typedef void (*xlang_bridge_direct_result_callback)(
    void* user_data,
    xlang_bridge_status status,
    const xlang_bridge_value* value,
    xlang_bridge_bytes_view error_message);

typedef struct xlang_bridge_host_callbacks {
  uint32_t struct_size;
  uint32_t abi_version;
  void* user_data;
  xlang_bridge_completion_callback completion;
  xlang_bridge_event_callback event;
  xlang_bridge_log_callback log;
} xlang_bridge_host_callbacks;

enum {
  XLANG_BRIDGE_FEATURE_KWARGS = 1ull << 0,
  XLANG_BRIDGE_FEATURE_BINARY = 1ull << 1,
  XLANG_BRIDGE_FEATURE_EVENTS = 1ull << 2,
  XLANG_BRIDGE_FEATURE_LRPC = 1ull << 3
};

typedef struct xlang_bridge_api {
  /*
   * The caller sets struct_size before xlang_bridge_get_api(). The bridge
   * fills only fields present in both the caller and bridge struct versions.
   */
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t feature_flags;

  xlang_bridge_status (*initialize)(
      xlang_bridge_request_id request_id,
      const xlang_bridge_initialize_options* options);

  xlang_bridge_status (*shutdown)(xlang_bridge_request_id request_id);

  xlang_bridge_status (*import_module)(xlang_bridge_request_id request_id,
                                       xlang_bridge_bytes_view module_name,
                                       xlang_bridge_bytes_view from_path,
                                       xlang_bridge_bytes_view thru);

  xlang_bridge_status (*get_member)(xlang_bridge_request_id request_id,
                                    xlang_bridge_handle object,
                                    xlang_bridge_bytes_view member_name);

  xlang_bridge_status (*set_member)(xlang_bridge_request_id request_id,
                                    xlang_bridge_handle object,
                                    xlang_bridge_bytes_view member_name,
                                    const xlang_bridge_value* value);

  xlang_bridge_status (*call)(xlang_bridge_request_id request_id,
                              xlang_bridge_handle callable,
                              const xlang_bridge_value* args,
                              uint32_t arg_count,
                              const xlang_bridge_named_value* kwargs,
                              uint32_t kwarg_count);

  xlang_bridge_status (*call_member)(xlang_bridge_request_id request_id,
                                     xlang_bridge_handle object,
                                     xlang_bridge_bytes_view member_name,
                                     const xlang_bridge_value* args,
                                     uint32_t arg_count,
                                     const xlang_bridge_named_value* kwargs,
                                     uint32_t kwarg_count);

  /*
   * Invoke an in-process XLang member on the calling thread. The completion
   * callback runs before this function returns. This is the thin JS <->
   * X::Value path used by Electron callSync; it does not enter the asynchronous
   * bridge worker queue.
   */
  xlang_bridge_status (*call_member_direct)(
      xlang_bridge_handle object,
      xlang_bridge_bytes_view member_name,
      const xlang_bridge_value* args,
      uint32_t arg_count,
      const xlang_bridge_named_value* kwargs,
      uint32_t kwarg_count,
      xlang_bridge_direct_result_callback result_callback,
      void* result_user_data);

  xlang_bridge_status (*event_on)(xlang_bridge_request_id request_id,
                                  xlang_bridge_handle object,
                                  xlang_bridge_bytes_view event_name,
                                  xlang_bridge_subscription_id subscription_id);

  xlang_bridge_status (*event_off)(
      xlang_bridge_request_id request_id,
      xlang_bridge_subscription_id subscription_id);

  /*
   * Non-blocking and thread-safe. The actual X::Value destruction is queued
   * onto the bridge worker. No completion callback is generated.
   */
  xlang_bridge_status (*release_handle)(xlang_bridge_handle handle);
} xlang_bridge_api;

typedef xlang_bridge_status (*xlang_bridge_get_api_fn)(
    uint32_t requested_abi_version,
    const xlang_bridge_host_callbacks* host_callbacks,
    xlang_bridge_api* out_api);

XLANG_BRIDGE_EXPORT xlang_bridge_status
xlang_bridge_get_api(uint32_t requested_abi_version,
                     const xlang_bridge_host_callbacks* host_callbacks,
                     xlang_bridge_api* out_api);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ELECTRON_XLANG_BRIDGE_H_ */
