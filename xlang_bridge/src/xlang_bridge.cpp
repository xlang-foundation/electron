/*
 * Copyright (c) 2026 XLang Foundation
 * SPDX-License-Identifier: MIT
 */

#include "xlang_bridge.h"

#include "value.h"
#include "xhost.h"
#include "xlang.h"
#include "xload.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kMinimumHostCallbacksSize =
    static_cast<uint32_t>(offsetof(xlang_bridge_host_callbacks, completion) +
                          sizeof(xlang_bridge_completion_callback));
constexpr uint32_t kMinimumApiSize = static_cast<uint32_t>(
    offsetof(xlang_bridge_api, feature_flags) + sizeof(uint64_t));

bool IsViewValid(xlang_bridge_bytes_view view) {
  if (view.size == 0) {
    return true;
  }
  constexpr uint64_t kMaximumViewSize =
      static_cast<uint64_t>(std::numeric_limits<std::ptrdiff_t>::max());
  return view.data != nullptr && view.size <= kMaximumViewSize &&
         view.size <= static_cast<uint64_t>(std::numeric_limits<size_t>::max());
}

bool IsRequiredViewValid(xlang_bridge_bytes_view view) {
  return view.size != 0 && IsViewValid(view);
}

bool CopyView(xlang_bridge_bytes_view view, std::string& destination) {
  if (!IsViewValid(view)) {
    return false;
  }
  if (view.size == 0) {
    destination.clear();
    return true;
  }
  destination.assign(reinterpret_cast<const char*>(view.data),
                     static_cast<size_t>(view.size));
  return true;
}

char* DuplicateCString(const std::string& value) {
  char* copy = new char[value.size() + 1];
  if (!value.empty()) {
    std::memcpy(copy, value.data(), value.size());
  }
  copy[value.size()] = '\0';
  return copy;
}

xlang_bridge_bytes_view ViewOf(std::string_view value) {
  return {reinterpret_cast<const uint8_t*>(value.data()),
          static_cast<uint64_t>(value.size())};
}

struct InputValue {
  uint32_t type = XLANG_BRIDGE_VALUE_UNDEFINED;
  int32_t boolean_value = 0;
  int64_t int64_value = 0;
  double double_value = 0.0;
  xlang_bridge_handle handle_value = 0;
  std::vector<uint8_t> bytes;
};

struct InputNamedValue {
  std::string name;
  InputValue value;
};

bool CopyInputValue(const xlang_bridge_value& source, InputValue& destination) {
  if (source.struct_size < sizeof(xlang_bridge_value)) {
    return false;
  }

  destination.type = source.type;
  switch (source.type) {
    case XLANG_BRIDGE_VALUE_UNDEFINED:
    case XLANG_BRIDGE_VALUE_NULL:
      return true;
    case XLANG_BRIDGE_VALUE_BOOL:
      destination.boolean_value = source.data.boolean_value;
      return true;
    case XLANG_BRIDGE_VALUE_INT64:
      destination.int64_value = source.data.int64_value;
      return true;
    case XLANG_BRIDGE_VALUE_DOUBLE:
      destination.double_value = source.data.double_value;
      return true;
    case XLANG_BRIDGE_VALUE_STRING:
    case XLANG_BRIDGE_VALUE_BINARY: {
      const auto view = source.data.bytes_value;
      if (!IsViewValid(view)) {
        return false;
      }
      if (view.size != 0) {
        destination.bytes.assign(view.data,
                                 view.data + static_cast<size_t>(view.size));
      }
      return true;
    }
    case XLANG_BRIDGE_VALUE_HANDLE:
      destination.handle_value = source.data.handle_value;
      return destination.handle_value != 0;
    default:
      return false;
  }
}

struct ConvertedValue {
  xlang_bridge_value value{};
  std::string string_storage;
  std::vector<uint8_t> binary_storage;

  ConvertedValue() {
    value.struct_size = sizeof(value);
    value.type = XLANG_BRIDGE_VALUE_UNDEFINED;
  }

  void RefreshView() {
    if (value.type == XLANG_BRIDGE_VALUE_STRING) {
      value.data.bytes_value = ViewOf(string_storage);
    } else if (value.type == XLANG_BRIDGE_VALUE_BINARY) {
      value.data.bytes_value = {binary_storage.data(),
                                static_cast<uint64_t>(binary_storage.size())};
    }
  }
};

struct ConvertedNamedValue {
  std::string name_storage;
  ConvertedValue converted;
  xlang_bridge_named_value named{};

  void RefreshView() {
    converted.RefreshView();
    named.name = ViewOf(name_storage);
    named.value = converted.value;
  }
};

class Bridge {
 public:
  static Bridge& Get() {
    // Intentionally process-lifetime. A dynamically loaded module must not run
    // a blocking C++ static destructor while the Windows loader lock is held.
    static Bridge* bridge = new Bridge();
    return *bridge;
  }

  xlang_bridge_status ConfigureHost(
      const xlang_bridge_host_callbacks& callbacks) {
    std::lock_guard<std::mutex> lock(host_mutex_);
    host_ = {};
    const size_t copy_size =
        std::min<size_t>(callbacks.struct_size, sizeof(host_));
    std::memcpy(&host_, &callbacks, copy_size);
    host_.struct_size = sizeof(host_);
    host_.abi_version = XLANG_BRIDGE_ABI_VERSION;
    return XLANG_BRIDGE_STATUS_OK;
  }

  xlang_bridge_status Initialize(
      xlang_bridge_request_id request_id,
      const xlang_bridge_initialize_options* options) {
    if (options == nullptr ||
        options->struct_size < sizeof(xlang_bridge_initialize_options) ||
        !IsRequiredViewValid(options->app_path) ||
        !IsViewValid(options->library_search_paths)) {
      return XLANG_BRIDGE_STATUS_INVALID_ARGUMENT;
    }

    const uint32_t flags = options->flags;
    std::string app_path;
    std::string extra_paths;
    if (!CopyView(options->app_path, app_path) ||
        !CopyView(options->library_search_paths, extra_paths)) {
      return XLANG_BRIDGE_STATUS_INVALID_ARGUMENT;
    }
    auto initialize_task = [this, request_id, flags,
                            app_path = std::move(app_path),
                            extra_paths = std::move(extra_paths)]() mutable {
      try {
        const xlang_bridge_status status =
            InitializeOnWorker(flags, app_path, extra_paths);
        Lifecycle expected = Lifecycle::kInitializing;
        lifecycle_.compare_exchange_strong(
            expected, status == XLANG_BRIDGE_STATUS_OK ? Lifecycle::kReady
                                                       : Lifecycle::kFailed);
        Complete(request_id, status, nullptr,
                 status == XLANG_BRIDGE_STATUS_OK
                     ? std::string()
                     : std::string("Unable to load and start xlang_eng"));
      } catch (...) {
        try {
          ShutdownOnWorker();
        } catch (...) {
        }
        Lifecycle expected = Lifecycle::kInitializing;
        lifecycle_.compare_exchange_strong(expected, Lifecycle::kFailed);
        throw;
      }
    };

    {
      /*
       * Publish kInitializing and place the initialization task while holding
       * the queue lock. Concurrent operations that observe kInitializing then
       * enqueue strictly behind initialization, including a retry after a
       * failed load.
       */
      std::lock_guard<std::mutex> lock(queue_mutex_);
      const Lifecycle state = lifecycle_.load();
      if (state == Lifecycle::kStopped) {
        // xlang_eng currently keeps imported extension DLLs process-global.
        // Loading it again after a full unload can re-enter cleaned singleton
        // state and crash. A completed shutdown is therefore process-final;
        // failed initialization remains retryable through kFailed.
        return XLANG_BRIDGE_STATUS_UNSUPPORTED;
      }
      if (state != Lifecycle::kIdle && state != Lifecycle::kFailed) {
        return state == Lifecycle::kReady || state == Lifecycle::kInitializing
                   ? XLANG_BRIDGE_STATUS_ALREADY_INITIALIZED
                   : XLANG_BRIDGE_STATUS_SHUTTING_DOWN;
      }

      lifecycle_.store(Lifecycle::kInitializing);
      worker_should_stop_ = false;
      bool pushed = false;
      try {
        queue_.push_back({request_id, std::move(initialize_task)});
        pushed = true;
        if (!worker_.joinable()) {
          worker_ = std::thread([this]() { WorkerMain(); });
        }
      } catch (...) {
        if (pushed) {
          queue_.pop_back();
        }
        lifecycle_.store(Lifecycle::kFailed);
        return XLANG_BRIDGE_STATUS_INTERNAL_ERROR;
      }
    }
    queue_cv_.notify_one();
    return XLANG_BRIDGE_STATUS_OK;
  }

  xlang_bridge_status Shutdown(xlang_bridge_request_id request_id) {
    {
      std::lock_guard<std::mutex> lock(shutdown_result_mutex_);
      shutdown_status_ = XLANG_BRIDGE_STATUS_OK;
      shutdown_error_.clear();
    }

    auto shutdown_task = [this]() {
      try {
        ShutdownOnWorker();
      } catch (const std::exception& error) {
        RecordShutdownFailure(error.what());
      } catch (...) {
        RecordShutdownFailure("Non-standard exception during XLang shutdown");
      }
      {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        worker_should_stop_ = true;
      }
      queue_cv_.notify_all();
    };

    {
      /*
       * The lifecycle transition and shutdown queue insertion are one atomic
       * queue operation. Normal operations therefore run before this task or
       * are rejected; none can remain queued behind shutdown for a restart.
       */
      std::lock_guard<std::mutex> lock(queue_mutex_);
      const Lifecycle previous_state = lifecycle_.load();
      if (previous_state == Lifecycle::kShuttingDown ||
          previous_state == Lifecycle::kStopped) {
        return XLANG_BRIDGE_STATUS_SHUTTING_DOWN;
      }
      if (worker_should_stop_) {
        return XLANG_BRIDGE_STATUS_INTERNAL_ERROR;
      }

      lifecycle_.store(Lifecycle::kShuttingDown);
      bool pushed = false;
      try {
        queue_.push_back({0, std::move(shutdown_task)});
        pushed = true;
        if (!worker_.joinable()) {
          worker_ = std::thread([this]() { WorkerMain(); });
        }
      } catch (...) {
        if (pushed) {
          queue_.pop_back();
        }
        lifecycle_.store(previous_state);
        return XLANG_BRIDGE_STATUS_INTERNAL_ERROR;
      }
    }
    queue_cv_.notify_one();

    /*
     * Completion is delivered only after the worker has exited. The host must
     * marshal the callback to Electron's main sequence and unload this library
     * after the callback returns.
     */
    std::unique_ptr<std::thread> completion_thread;
    try {
      completion_thread = std::make_unique<std::thread>(
          [this, request_id]() { FinishShutdown(request_id); });
      completion_thread->detach();
    } catch (...) {
      if (completion_thread && completion_thread->joinable()) {
        // detach() itself is not noexcept. In its exceptional path, leave the
        // already-running joiner alive rather than destroying a joinable
        // std::thread and terminating the process.
        completion_thread.release();
      } else {
        // Thread allocation/start failed. Preserve the accepted asynchronous
        // operation's completion contract, even if that means blocking only in
        // this exceptional resource-exhaustion path.
        FinishShutdown(request_id);
      }
    }
    return XLANG_BRIDGE_STATUS_OK;
  }

  xlang_bridge_status ImportModule(xlang_bridge_request_id request_id,
                                   xlang_bridge_bytes_view module_name,
                                   xlang_bridge_bytes_view from_path,
                                   xlang_bridge_bytes_view thru) {
    const bool arguments_valid = IsRequiredViewValid(module_name) &&
                                 IsViewValid(from_path) && IsViewValid(thru);
    if (!IsOperationAccepted() || !arguments_valid) {
      return OperationRejectionOrInvalid(arguments_valid);
    }
    std::string module;
    std::string from;
    std::string transport;
    if (!CopyView(module_name, module) || !CopyView(from_path, from) ||
        !CopyView(thru, transport)) {
      return XLANG_BRIDGE_STATUS_INVALID_ARGUMENT;
    }
    if (!Enqueue(request_id, [this, request_id, module = std::move(module),
                              from = std::move(from),
                              transport = std::move(transport)]() {
          X::Value imported;
          const bool ok =
              X::g_pXHost != nullptr && runtime_ != nullptr &&
              X::g_pXHost->Import(
                  runtime_, module.c_str(),
                  from.empty() ? nullptr : from.c_str(),
                  transport.empty() ? nullptr : transport.c_str(), imported);
          if (!ok || !imported.IsValid()) {
            Complete(request_id, XLANG_BRIDGE_STATUS_IMPORT_FAILED, nullptr,
                     TakeException("XLang module import failed"));
            return;
          }
          ConvertedValue converted = ConvertOutput(imported);
          converted.RefreshView();
          Complete(request_id, XLANG_BRIDGE_STATUS_OK, &converted.value, {});
        })) {
      return QueueFailureStatus();
    }
    return XLANG_BRIDGE_STATUS_OK;
  }

  xlang_bridge_status GetMember(xlang_bridge_request_id request_id,
                                xlang_bridge_handle object,
                                xlang_bridge_bytes_view member_name) {
    const bool arguments_valid =
        object != 0 && IsRequiredViewValid(member_name);
    if (!IsOperationAccepted() || !arguments_valid) {
      return OperationRejectionOrInvalid(arguments_valid);
    }
    std::string member;
    if (!CopyView(member_name, member)) {
      return XLANG_BRIDGE_STATUS_INVALID_ARGUMENT;
    }
    if (!Enqueue(request_id, [this, request_id, object,
                              member = std::move(member)]() {
          X::Value owner;
          if (!LookupHandle(object, owner)) {
            Complete(request_id, XLANG_BRIDGE_STATUS_HANDLE_NOT_FOUND, nullptr,
                     "Unknown XLang object handle");
            return;
          }
          X::Value result = QueryMember(owner, member);
          if (!result.IsValid()) {
            Complete(request_id, XLANG_BRIDGE_STATUS_MEMBER_NOT_FOUND, nullptr,
                     TakeException("XLang member was not found"));
            return;
          }

          if (IsProperty(result)) {
            X::ARGS args(0);
            X::KWARGS kwargs;
            X::Value property_value;
            if (!Invoke(result, args, kwargs, property_value)) {
              Complete(request_id, XLANG_BRIDGE_STATUS_CALL_FAILED, nullptr,
                       TakeException("XLang property getter failed"));
              return;
            }
            result = property_value;
          }

          ConvertedValue converted = ConvertOutput(result);
          converted.RefreshView();
          Complete(request_id, XLANG_BRIDGE_STATUS_OK, &converted.value, {});
        })) {
      return QueueFailureStatus();
    }
    return XLANG_BRIDGE_STATUS_OK;
  }

  xlang_bridge_status SetMember(xlang_bridge_request_id request_id,
                                xlang_bridge_handle object,
                                xlang_bridge_bytes_view member_name,
                                const xlang_bridge_value* value) {
    const bool arguments_valid =
        object != 0 && IsRequiredViewValid(member_name) && value != nullptr;
    if (!IsOperationAccepted() || !arguments_valid) {
      return OperationRejectionOrInvalid(arguments_valid);
    }
    InputValue input;
    if (!CopyInputValue(*value, input)) {
      return XLANG_BRIDGE_STATUS_INVALID_ARGUMENT;
    }
    std::string member;
    if (!CopyView(member_name, member)) {
      return XLANG_BRIDGE_STATUS_INVALID_ARGUMENT;
    }
    if (!Enqueue(request_id, [this, request_id, object,
                              member = std::move(member),
                              input = std::move(input)]() {
          X::Value owner;
          if (!LookupHandle(object, owner)) {
            Complete(request_id, XLANG_BRIDGE_STATUS_HANDLE_NOT_FOUND, nullptr,
                     "Unknown XLang object handle");
            return;
          }
          X::Value xvalue;
          if (!ConvertInput(input, xvalue)) {
            Complete(request_id, XLANG_BRIDGE_STATUS_HANDLE_NOT_FOUND, nullptr,
                     "An argument referenced an unknown XLang handle");
            return;
          }

          X::Value member_value = QueryMember(owner, member);
          if (IsProperty(member_value)) {
            X::ARGS args(1);
            args.push_back(xvalue);
            X::KWARGS kwargs;
            X::Value ignored;
            if (!Invoke(member_value, args, kwargs, ignored)) {
              Complete(request_id, XLANG_BRIDGE_STATUS_CALL_FAILED, nullptr,
                       TakeException("XLang property setter failed"));
              return;
            }
          } else {
            X::Value key(member);
            if (!owner.IsObject() || !owner.GetObj()->Set(key, xvalue)) {
              Complete(request_id, XLANG_BRIDGE_STATUS_UNSUPPORTED, nullptr,
                       TakeException(
                           "XLang object does not support member assignment"));
              return;
            }
          }
          ConvertedValue converted;
          converted.value.type = XLANG_BRIDGE_VALUE_BOOL;
          converted.value.data.boolean_value = 1;
          Complete(request_id, XLANG_BRIDGE_STATUS_OK, &converted.value, {});
        })) {
      return QueueFailureStatus();
    }
    return XLANG_BRIDGE_STATUS_OK;
  }

  xlang_bridge_status Call(xlang_bridge_request_id request_id,
                           xlang_bridge_handle callable,
                           const xlang_bridge_value* args,
                           uint32_t arg_count,
                           const xlang_bridge_named_value* kwargs,
                           uint32_t kwarg_count) {
    if (!IsOperationAccepted() || callable == 0) {
      return OperationRejectionOrInvalid(callable != 0);
    }
    std::vector<InputValue> copied_args;
    std::vector<InputNamedValue> copied_kwargs;
    if (!CopyInputs(args, arg_count, kwargs, kwarg_count, copied_args,
                    copied_kwargs)) {
      return XLANG_BRIDGE_STATUS_INVALID_ARGUMENT;
    }
    if (!Enqueue(request_id, [this, request_id, callable,
                              copied_args = std::move(copied_args),
                              copied_kwargs = std::move(copied_kwargs)]() {
          X::Value function;
          if (!LookupHandle(callable, function)) {
            Complete(request_id, XLANG_BRIDGE_STATUS_HANDLE_NOT_FOUND, nullptr,
                     "Unknown XLang callable handle");
            return;
          }
          CallValue(request_id, function, copied_args, copied_kwargs);
        })) {
      return QueueFailureStatus();
    }
    return XLANG_BRIDGE_STATUS_OK;
  }

  xlang_bridge_status CallMember(xlang_bridge_request_id request_id,
                                 xlang_bridge_handle object,
                                 xlang_bridge_bytes_view member_name,
                                 const xlang_bridge_value* args,
                                 uint32_t arg_count,
                                 const xlang_bridge_named_value* kwargs,
                                 uint32_t kwarg_count) {
    const bool arguments_valid =
        object != 0 && IsRequiredViewValid(member_name);
    if (!IsOperationAccepted() || !arguments_valid) {
      return OperationRejectionOrInvalid(arguments_valid);
    }
    std::vector<InputValue> copied_args;
    std::vector<InputNamedValue> copied_kwargs;
    if (!CopyInputs(args, arg_count, kwargs, kwarg_count, copied_args,
                    copied_kwargs)) {
      return XLANG_BRIDGE_STATUS_INVALID_ARGUMENT;
    }
    std::string member;
    if (!CopyView(member_name, member)) {
      return XLANG_BRIDGE_STATUS_INVALID_ARGUMENT;
    }
    if (!Enqueue(request_id, [this, request_id, object,
                              member = std::move(member),
                              copied_args = std::move(copied_args),
                              copied_kwargs = std::move(copied_kwargs)]() {
          X::Value owner;
          if (!LookupHandle(object, owner)) {
            Complete(request_id, XLANG_BRIDGE_STATUS_HANDLE_NOT_FOUND, nullptr,
                     "Unknown XLang object handle");
            return;
          }
          X::Value function = QueryMember(owner, member);
          if (!function.IsValid() || !function.IsObject()) {
            Complete(request_id, XLANG_BRIDGE_STATUS_MEMBER_NOT_FOUND, nullptr,
                     "XLang member was not found or is not callable");
            return;
          }
          CallValue(request_id, function, copied_args, copied_kwargs);
        })) {
      return QueueFailureStatus();
    }
    return XLANG_BRIDGE_STATUS_OK;
  }

  xlang_bridge_status EventOn(xlang_bridge_request_id request_id,
                              xlang_bridge_handle object,
                              xlang_bridge_bytes_view event_name,
                              xlang_bridge_subscription_id subscription_id) {
    const bool arguments_valid =
        object != 0 && IsRequiredViewValid(event_name) && subscription_id != 0;
    if (!IsOperationAccepted() || !arguments_valid) {
      return OperationRejectionOrInvalid(arguments_valid);
    }
    std::string member;
    if (!CopyView(event_name, member)) {
      return XLANG_BRIDGE_STATUS_INVALID_ARGUMENT;
    }
    if (!Enqueue(request_id, [this, request_id, object,
                              member = std::move(member), subscription_id]() {
          {
            std::lock_guard<std::mutex> lock(subscription_mutex_);
            if (subscriptions_.find(subscription_id) != subscriptions_.end()) {
              Complete(request_id, XLANG_BRIDGE_STATUS_EVENT_FAILED, nullptr,
                       "Duplicate XLang subscription identifier");
              return;
            }
          }

          X::Value owner;
          if (!LookupHandle(object, owner)) {
            Complete(request_id, XLANG_BRIDGE_STATUS_HANDLE_NOT_FOUND, nullptr,
                     "Unknown XLang object handle");
            return;
          }
          X::Value event_value = QueryMember(owner, member);
          if (!event_value.IsValid() || !event_value.IsObject()) {
            Complete(request_id, XLANG_BRIDGE_STATUS_MEMBER_NOT_FOUND, nullptr,
                     "XLang event member was not found");
            return;
          }

          X::U_FUNC handler([this, subscription_id](X::XRuntime*, X::XObj*,
                                                    X::XObj*, X::ARGS& params,
                                                    X::KWARGS& kwargs,
                                                    X::Value& ret_value) {
            if (BeginEventCallback()) {
              try {
                EmitEvent(subscription_id, params, kwargs);
              } catch (const std::exception& error) {
                Log(3, std::string("XLang event callback failed: ") +
                           error.what());
              } catch (...) {
                Log(3, "XLang event callback failed");
              }
              EndEventCallback();
            }
            ret_value = X::Value(true);
            return true;
          });
          X::Func callback("__electron_xlang_event", handler);
          X::Value callback_value(callback);

          Subscription provisional;
          provisional.event = event_value;
          provisional.callback = callback_value;
          provisional.cookie = 0;
          {
            std::lock_guard<std::mutex> lock(subscription_mutex_);
            const auto inserted =
                subscriptions_.emplace(subscription_id, provisional);
            if (!inserted.second) {
              Complete(request_id, XLANG_BRIDGE_STATUS_EVENT_FAILED, nullptr,
                       "Duplicate XLang subscription identifier");
              return;
            }
          }

          X::ARGS event_args(1);
          event_args.push_back(callback_value);
          X::KWARGS event_kwargs(1);
          X::Value operation{std::string(X::EventSubscribeOperation)};
          event_kwargs.Add(X::EventOperationKeyword, operation);
          X::Value cookie_value;
          if (!Invoke(event_value, event_args, event_kwargs, cookie_value) ||
              !cookie_value.IsNumber() || cookie_value.GetLongLong() == 0) {
            {
              std::lock_guard<std::mutex> lock(subscription_mutex_);
              subscriptions_.erase(subscription_id);
            }
            Complete(request_id, XLANG_BRIDGE_STATUS_EVENT_FAILED, nullptr,
                     TakeException("XLang event subscription failed"));
            return;
          }

          {
            std::lock_guard<std::mutex> lock(subscription_mutex_);
            const auto found = subscriptions_.find(subscription_id);
            if (found != subscriptions_.end()) {
              found->second.cookie = cookie_value.GetLongLong();
            }
          }

          ConvertedValue converted;
          converted.value.type = XLANG_BRIDGE_VALUE_SUBSCRIPTION;
          converted.value.data.subscription_value = subscription_id;
          Complete(request_id, XLANG_BRIDGE_STATUS_OK, &converted.value, {});
        })) {
      return QueueFailureStatus();
    }
    return XLANG_BRIDGE_STATUS_OK;
  }

  xlang_bridge_status EventOff(xlang_bridge_request_id request_id,
                               xlang_bridge_subscription_id subscription_id) {
    if (!IsOperationAccepted() || subscription_id == 0) {
      return OperationRejectionOrInvalid(subscription_id != 0);
    }
    if (!Enqueue(request_id, [this, request_id, subscription_id]() {
          const bool removed = UnsubscribeOnWorker(subscription_id);
          if (!removed) {
            Complete(request_id, XLANG_BRIDGE_STATUS_EVENT_FAILED, nullptr,
                     TakeException("XLang event unsubscription failed"));
            return;
          }
          ConvertedValue converted;
          converted.value.type = XLANG_BRIDGE_VALUE_BOOL;
          converted.value.data.boolean_value = 1;
          Complete(request_id, XLANG_BRIDGE_STATUS_OK, &converted.value, {});
        })) {
      return QueueFailureStatus();
    }
    return XLANG_BRIDGE_STATUS_OK;
  }

  xlang_bridge_status ReleaseHandle(xlang_bridge_handle handle) {
    if (handle == 0) {
      return XLANG_BRIDGE_STATUS_INVALID_ARGUMENT;
    }
    if (!IsOperationAccepted()) {
      return OperationRejectionOrInvalid(true);
    }
    if (!Enqueue(0, [this, handle]() {
          std::lock_guard<std::mutex> lock(handle_mutex_);
          handles_.erase(handle);
        })) {
      return QueueFailureStatus();
    }
    return XLANG_BRIDGE_STATUS_OK;
  }

 private:
  enum class Lifecycle {
    kIdle,
    kInitializing,
    kReady,
    kFailed,
    kShuttingDown,
    kStopped
  };

  struct Subscription {
    X::Value event;
    X::Value callback;
    int64_t cookie = 0;
  };

  struct QueuedTask {
    xlang_bridge_request_id request_id = 0;
    std::function<void()> function;
  };

  Bridge() = default;

  bool IsOperationAccepted() const {
    const Lifecycle state = lifecycle_.load();
    return state == Lifecycle::kInitializing || state == Lifecycle::kReady;
  }

  xlang_bridge_status OperationRejectionOrInvalid(bool arguments_valid) const {
    if (!arguments_valid) {
      return XLANG_BRIDGE_STATUS_INVALID_ARGUMENT;
    }
    const Lifecycle state = lifecycle_.load();
    if (state == Lifecycle::kShuttingDown || state == Lifecycle::kStopped) {
      return XLANG_BRIDGE_STATUS_SHUTTING_DOWN;
    }
    return XLANG_BRIDGE_STATUS_NOT_INITIALIZED;
  }

  xlang_bridge_status QueueFailureStatus() const {
    const Lifecycle state = lifecycle_.load();
    if (state == Lifecycle::kShuttingDown || state == Lifecycle::kStopped) {
      return XLANG_BRIDGE_STATUS_SHUTTING_DOWN;
    }
    if (state == Lifecycle::kIdle || state == Lifecycle::kFailed) {
      return XLANG_BRIDGE_STATUS_NOT_INITIALIZED;
    }
    return XLANG_BRIDGE_STATUS_INTERNAL_ERROR;
  }

  bool Enqueue(xlang_bridge_request_id request_id, std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      if (worker_should_stop_) {
        return false;
      }
      const Lifecycle state = lifecycle_.load();
      if (state != Lifecycle::kInitializing && state != Lifecycle::kReady) {
        return false;
      }

      bool pushed = false;
      try {
        queue_.push_back({request_id, std::move(task)});
        pushed = true;
        if (!worker_.joinable()) {
          worker_ = std::thread([this]() { WorkerMain(); });
        }
      } catch (...) {
        if (pushed) {
          queue_.pop_back();
        }
        return false;
      }
    }
    queue_cv_.notify_one();
    return true;
  }

  void FinishShutdown(xlang_bridge_request_id request_id) noexcept {
    try {
      if (worker_.joinable()) {
        worker_.join();
      }
      lifecycle_.store(Lifecycle::kStopped);
      xlang_bridge_status status;
      std::string error;
      {
        std::lock_guard<std::mutex> lock(shutdown_result_mutex_);
        status = shutdown_status_;
        error = shutdown_error_;
      }
      Complete(request_id, status, nullptr, error);
    } catch (...) {
      lifecycle_.store(Lifecycle::kStopped);
      Complete(request_id, XLANG_BRIDGE_STATUS_INTERNAL_ERROR, nullptr,
               "Unable to finish XLang shutdown");
    }
  }

  void RecordShutdownFailure(std::string_view error) noexcept {
    try {
      std::lock_guard<std::mutex> lock(shutdown_result_mutex_);
      shutdown_status_ = XLANG_BRIDGE_STATUS_INTERNAL_ERROR;
      try {
        if (error.empty()) {
          shutdown_error_.clear();
        } else {
          shutdown_error_.assign(error.data(), error.size());
        }
      } catch (...) {
        shutdown_error_.clear();
      }
    } catch (...) {
      // Lock acquisition cannot normally fail for this non-recursive use. If
      // the platform reports a mutex error, the worker catch below still
      // completes shutdown instead of allowing an exception to escape.
    }
  }

  void WorkerMain() {
    for (;;) {
      QueuedTask task;
      {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(
            lock, [this]() { return worker_should_stop_ || !queue_.empty(); });
        if (queue_.empty() && worker_should_stop_) {
          return;
        }
        task = std::move(queue_.front());
        queue_.pop_front();
      }
      try {
        task.function();
      } catch (const std::exception& error) {
        Log(3, error.what());
        if (task.request_id != 0) {
          Complete(task.request_id, XLANG_BRIDGE_STATUS_INTERNAL_ERROR, nullptr,
                   "Unhandled XLang bridge exception");
        }
      } catch (...) {
        Log(3, "Unhandled non-standard XLang bridge exception");
        if (task.request_id != 0) {
          Complete(task.request_id, XLANG_BRIDGE_STATUS_INTERNAL_ERROR, nullptr,
                   "Unhandled non-standard XLang bridge exception");
        }
      }
    }
  }

  xlang_bridge_status InitializeOnWorker(uint32_t flags,
                                         const std::string& app_path,
                                         const std::string& extra_paths) {
    config_ = std::make_unique<X::Config>();
    config_->enablePython = (flags & XLANG_BRIDGE_INIT_ENABLE_PYTHON) != 0;
    config_->dbg = (flags & XLANG_BRIDGE_INIT_DEBUG) != 0;
    config_->enterEventLoop = false;
    config_->runEventLoopInThread = false;
    config_->appPath = DuplicateCString(app_path);

    std::string search_paths = app_path;
    if (!extra_paths.empty()) {
      search_paths.push_back('\n');
      search_paths.append(extra_paths);
    }
    config_->dllSearchPath = DuplicateCString(search_paths);

    if (xload_.Load(config_.get()) != 0) {
      ResetConfig();
      return XLANG_BRIDGE_STATUS_ENGINE_LOAD_FAILED;
    }
    if (X::g_pXHost == nullptr) {
      xload_.Unload();
      ResetConfig();
      return XLANG_BRIDGE_STATUS_ENGINE_LOAD_FAILED;
    }
    if (xload_.Run() != 0) {
      xload_.Unload();
      X::g_pXHost = nullptr;
      ResetConfig();
      return XLANG_BRIDGE_STATUS_ENGINE_LOAD_FAILED;
    }

    runtime_ = X::g_pXHost->CreateRuntime(true);
    if (runtime_ == nullptr) {
      xload_.Unload();
      X::g_pXHost = nullptr;
      ResetConfig();
      return XLANG_BRIDGE_STATUS_ENGINE_LOAD_FAILED;
    }

    event_thread_ = std::thread([this]() { xload_.EventLoop(); });
    accepting_event_callbacks_.store(true, std::memory_order_release);
    Log(1, "XLang engine initialized");
    return XLANG_BRIDGE_STATUS_OK;
  }

  void ShutdownOnWorker() {
    accepting_event_callbacks_.store(false, std::memory_order_release);
    Log(1, "Stopping XLang event subscriptions");
    std::vector<xlang_bridge_subscription_id> subscription_ids;
    {
      std::lock_guard<std::mutex> lock(subscription_mutex_);
      subscription_ids.reserve(subscriptions_.size());
      for (const auto& entry : subscriptions_) {
        subscription_ids.push_back(entry.first);
      }
    }
    for (const auto id : subscription_ids) {
      UnsubscribeOnWorker(id);
    }

    Log(1, "Waiting for in-flight XLang event callbacks");
    {
      std::unique_lock<std::mutex> lock(event_barrier_mutex_);
      event_barrier_cv_.wait(lock, [this]() {
        return active_event_callbacks_.load(std::memory_order_acquire) == 0;
      });
    }

    Log(1, "Releasing XLang bridge handles");
    {
      std::lock_guard<std::mutex> lock(subscription_mutex_);
      subscriptions_.clear();
    }
    {
      std::lock_guard<std::mutex> lock(handle_mutex_);
      handles_.clear();
    }
    runtime_ = nullptr;

    Log(1, "Stopping XLang event loop");
    if (X::g_pXHost != nullptr) {
      xload_.QuitEventLoop();
    }
    if (event_thread_.joinable()) {
      event_thread_.join();
    }
    Log(1, "Unloading XLang engine");
    xload_.Unload();
    X::g_pXHost = nullptr;
    ResetConfig();
    Log(1, "XLang engine stopped");
  }

  void ResetConfig() {
    if (config_) {
      /*
       * xlang_eng writes xlangEnginePath using its own allocator. XLang's
       * Windows builds use a static CRT, so deleting that buffer from this
       * bridge's CRT corrupts the heap. The engine owns that field across the
       * DLL boundary; do not let Config's inline destructor free it here.
       */
      config_->xlangEnginePath = nullptr;
    }
    config_.reset();
  }

  bool BeginEventCallback() {
    active_event_callbacks_.fetch_add(1, std::memory_order_acq_rel);
    if (!accepting_event_callbacks_.load(std::memory_order_acquire)) {
      EndEventCallback();
      return false;
    }
    return true;
  }

  void EndEventCallback() {
    if (active_event_callbacks_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      std::lock_guard<std::mutex> lock(event_barrier_mutex_);
      event_barrier_cv_.notify_all();
    }
  }

  X::Value QueryMember(X::Value& object, const std::string& member) {
    if (!object.IsObject() || runtime_ == nullptr) {
      return X::Value();
    }
    X::Value result = object.GetObj()->Member(runtime_, member.c_str());
    return result;
  }

  bool IsProperty(X::Value& value) {
    if (!value.IsObject()) {
      return false;
    }
    X::XObj* object = value.GetObj();
    if (object->GetType() == X::ObjType::Prop) {
      return true;
    }
    if (object->GetType() != X::ObjType::RemoteObject) {
      return false;
    }
    auto* remote = dynamic_cast<X::XRemoteObject*>(object);
    return remote != nullptr &&
           (remote->GetMemberFlags() & 0xFF) ==
               static_cast<int>(X::PackageMemberType::Prop);
  }

  bool Invoke(X::Value& callable,
              X::ARGS& args,
              X::KWARGS& kwargs,
              X::Value& result) {
    if (!callable.IsObject()) {
      return false;
    }
    X::XObj* object = callable.GetObj();
    X::XRuntime* runtime = object->RT() != nullptr ? object->RT() : runtime_;
    const bool ok =
        object->Call(runtime, object->Parent(), args, kwargs, result);
    if (ok && result.IsObject()) {
      result.GetObj()->SetContext(runtime, object->Parent());
    }
    return ok;
  }

  bool CopyInputs(const xlang_bridge_value* args,
                  uint32_t arg_count,
                  const xlang_bridge_named_value* kwargs,
                  uint32_t kwarg_count,
                  std::vector<InputValue>& copied_args,
                  std::vector<InputNamedValue>& copied_kwargs) {
    if ((arg_count != 0 && args == nullptr) ||
        (kwarg_count != 0 && kwargs == nullptr) ||
        arg_count > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        kwarg_count > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
      return false;
    }
    copied_args.reserve(arg_count);
    for (uint32_t index = 0; index < arg_count; ++index) {
      InputValue value;
      if (!CopyInputValue(args[index], value)) {
        return false;
      }
      copied_args.push_back(std::move(value));
    }
    copied_kwargs.reserve(kwarg_count);
    for (uint32_t index = 0; index < kwarg_count; ++index) {
      if (!IsRequiredViewValid(kwargs[index].name)) {
        return false;
      }
      InputNamedValue named;
      if (!CopyView(kwargs[index].name, named.name)) {
        return false;
      }
      if (!CopyInputValue(kwargs[index].value, named.value)) {
        return false;
      }
      copied_kwargs.push_back(std::move(named));
    }
    return true;
  }

  bool BuildCallInputs(const std::vector<InputValue>& inputs,
                       const std::vector<InputNamedValue>& named_inputs,
                       X::ARGS& args,
                       X::KWARGS& kwargs) {
    for (const auto& input : inputs) {
      X::Value value;
      if (!ConvertInput(input, value)) {
        return false;
      }
      args.push_back(value);
    }
    for (const auto& input : named_inputs) {
      X::Value value;
      if (!ConvertInput(input.value, value)) {
        return false;
      }
      kwargs.Add(input.name.c_str(), value, true);
    }
    return true;
  }

  void CallValue(xlang_bridge_request_id request_id,
                 X::Value& callable,
                 const std::vector<InputValue>& input_args,
                 const std::vector<InputNamedValue>& input_kwargs) {
    X::ARGS args(static_cast<int>(input_args.size()));
    X::KWARGS kwargs(static_cast<int>(input_kwargs.size()));
    if (!BuildCallInputs(input_args, input_kwargs, args, kwargs)) {
      Complete(request_id, XLANG_BRIDGE_STATUS_HANDLE_NOT_FOUND, nullptr,
               "An argument referenced an unknown XLang handle");
      return;
    }
    X::Value result;
    if (!Invoke(callable, args, kwargs, result)) {
      Complete(request_id, XLANG_BRIDGE_STATUS_CALL_FAILED, nullptr,
               TakeException("XLang call failed"));
      return;
    }
    ConvertedValue converted = ConvertOutput(result);
    converted.RefreshView();
    Complete(request_id, XLANG_BRIDGE_STATUS_OK, &converted.value, {});
  }

  bool ConvertInput(const InputValue& input, X::Value& output) {
    switch (input.type) {
      case XLANG_BRIDGE_VALUE_UNDEFINED:
        output = X::Value();
        return true;
      case XLANG_BRIDGE_VALUE_NULL:
        output = X::Value(static_cast<X::XObj*>(nullptr));
        return true;
      case XLANG_BRIDGE_VALUE_BOOL:
        output = X::Value(input.boolean_value != 0);
        return true;
      case XLANG_BRIDGE_VALUE_INT64:
        output = X::Value(static_cast<long long>(input.int64_value));
        return true;
      case XLANG_BRIDGE_VALUE_DOUBLE:
        output = X::Value(input.double_value);
        return true;
      case XLANG_BRIDGE_VALUE_STRING: {
        std::string text;
        if (!input.bytes.empty()) {
          text.assign(reinterpret_cast<const char*>(input.bytes.data()),
                      input.bytes.size());
        }
        output = X::Value(text);
        return true;
      }
      case XLANG_BRIDGE_VALUE_BINARY: {
        char* bytes = nullptr;
        if (!input.bytes.empty()) {
          bytes = new char[input.bytes.size()];
          std::memcpy(bytes, input.bytes.data(), input.bytes.size());
        }
        X::Bin binary(bytes, input.bytes.size(), true);
        output = X::Value(binary);
        return true;
      }
      case XLANG_BRIDGE_VALUE_HANDLE:
        return LookupHandle(input.handle_value, output);
      default:
        return false;
    }
  }

  ConvertedValue ConvertOutput(X::Value& input) {
    ConvertedValue converted;
    if (!input.IsValid()) {
      converted.value.type = XLANG_BRIDGE_VALUE_UNDEFINED;
      return converted;
    }
    if (input.IsNone()) {
      converted.value.type = XLANG_BRIDGE_VALUE_NULL;
      return converted;
    }
    if (input.IsBool()) {
      converted.value.type = XLANG_BRIDGE_VALUE_BOOL;
      converted.value.data.boolean_value = input.GetBool() ? 1 : 0;
      return converted;
    }
    if (input.IsLong()) {
      converted.value.type = XLANG_BRIDGE_VALUE_INT64;
      converted.value.data.int64_value = input.GetLongLong();
      return converted;
    }
    if (input.IsDouble()) {
      converted.value.type = XLANG_BRIDGE_VALUE_DOUBLE;
      converted.value.data.double_value = input.GetDouble();
      return converted;
    }
    if (input.IsString()) {
      converted.value.type = XLANG_BRIDGE_VALUE_STRING;
      converted.string_storage = input.ToString(false);
      return converted;
    }
    if (input.IsBin()) {
      auto* binary = dynamic_cast<X::XBin*>(input.GetObj());
      if (binary != nullptr) {
        converted.value.type = XLANG_BRIDGE_VALUE_BINARY;
        const size_t size = static_cast<size_t>(input.Size());
        const char* data = binary->Data();
        if (size != 0 && data != nullptr) {
          converted.binary_storage.assign(
              reinterpret_cast<const uint8_t*>(data),
              reinterpret_cast<const uint8_t*>(data) + size);
        }
        return converted;
      }
    }

    converted.value.type = XLANG_BRIDGE_VALUE_HANDLE;
    converted.value.flags =
        input.IsObject() ? static_cast<uint32_t>(input.GetObj()->GetType()) : 0;
    converted.value.data.handle_value = StoreHandle(input);
    return converted;
  }

  xlang_bridge_handle StoreHandle(const X::Value& value) {
    const xlang_bridge_handle handle =
        next_handle_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(handle_mutex_);
    handles_.emplace(handle, value);
    return handle;
  }

  bool LookupHandle(xlang_bridge_handle handle, X::Value& value) {
    std::lock_guard<std::mutex> lock(handle_mutex_);
    const auto found = handles_.find(handle);
    if (found == handles_.end()) {
      return false;
    }
    value = found->second;
    return true;
  }

  bool UnsubscribeOnWorker(xlang_bridge_subscription_id subscription_id) {
    Subscription subscription;
    {
      std::lock_guard<std::mutex> lock(subscription_mutex_);
      const auto found = subscriptions_.find(subscription_id);
      if (found == subscriptions_.end()) {
        return false;
      }
      subscription = found->second;
    }

    X::ARGS args(1);
    X::Value cookie(static_cast<long long>(subscription.cookie));
    args.push_back(cookie);
    X::KWARGS kwargs(1);
    X::Value operation{std::string(X::EventUnsubscribeOperation)};
    kwargs.Add(X::EventOperationKeyword, operation);
    X::Value result;
    const bool ok =
        Invoke(subscription.event, args, kwargs, result) && result.ToBool();
    if (ok) {
      std::lock_guard<std::mutex> lock(subscription_mutex_);
      subscriptions_.erase(subscription_id);
    }
    return ok;
  }

  void EmitEvent(xlang_bridge_subscription_id subscription_id,
                 X::ARGS& args,
                 X::KWARGS& kwargs) {
    {
      std::lock_guard<std::mutex> lock(subscription_mutex_);
      if (subscriptions_.find(subscription_id) == subscriptions_.end()) {
        return;
      }
    }

    std::vector<ConvertedValue> converted_args;
    converted_args.reserve(args.size());
    for (size_t index = 0; index < args.size(); ++index) {
      converted_args.push_back(ConvertOutput(args[index]));
    }
    for (auto& value : converted_args) {
      value.RefreshView();
    }

    std::vector<xlang_bridge_value> argument_values;
    argument_values.reserve(converted_args.size());
    for (const auto& value : converted_args) {
      argument_values.push_back(value.value);
    }

    std::vector<ConvertedNamedValue> converted_kwargs;
    converted_kwargs.reserve(kwargs.size());
    for (auto& item : kwargs) {
      ConvertedNamedValue converted;
      converted.name_storage = item.key == nullptr ? "" : item.key;
      converted.converted = ConvertOutput(item.val);
      converted_kwargs.push_back(std::move(converted));
    }
    for (auto& value : converted_kwargs) {
      value.RefreshView();
    }

    std::vector<xlang_bridge_named_value> named_values;
    named_values.reserve(converted_kwargs.size());
    for (const auto& value : converted_kwargs) {
      named_values.push_back(value.named);
    }

    xlang_bridge_host_callbacks host = HostSnapshot();
    if (host.event != nullptr) {
      host.event(host.user_data, subscription_id, argument_values.data(),
                 static_cast<uint32_t>(argument_values.size()),
                 named_values.data(),
                 static_cast<uint32_t>(named_values.size()));
    }
  }

  std::string TakeException(const char* fallback) {
    if (runtime_ == nullptr) {
      return fallback;
    }
    X::Value exception = runtime_->GetException();
    runtime_->ClearException();
    if (!exception.IsValid()) {
      return fallback;
    }
    if (exception.IsObject() &&
        exception.GetObj()->GetType() == X::ObjType::Error) {
      auto* error = dynamic_cast<X::XError*>(exception.GetObj());
      if (error != nullptr && error->GetInfo() != nullptr) {
        return error->GetInfo();
      }
    }
    const std::string text = exception.ToString(false);
    return text.empty() ? fallback : text;
  }

  xlang_bridge_host_callbacks HostSnapshot() {
    std::lock_guard<std::mutex> lock(host_mutex_);
    return host_;
  }

  void Complete(xlang_bridge_request_id request_id,
                xlang_bridge_status status,
                const xlang_bridge_value* value,
                std::string_view error) noexcept {
    try {
      const xlang_bridge_host_callbacks host = HostSnapshot();
      if (host.completion != nullptr) {
        host.completion(host.user_data, request_id, status, value,
                        ViewOf(error));
      }
    } catch (...) {
      // Host callbacks also cross the C ABI. A callback owned by the embedding
      // process must not unwind through the bridge worker or shutdown thread.
    }
  }

  void Log(int32_t level, std::string_view message) noexcept {
    try {
      const xlang_bridge_host_callbacks host = HostSnapshot();
      if (host.log != nullptr) {
        host.log(host.user_data, level, ViewOf(message));
      }
    } catch (...) {
    }
  }

  std::atomic<Lifecycle> lifecycle_{Lifecycle::kIdle};
  std::mutex host_mutex_;
  xlang_bridge_host_callbacks host_{};

  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<QueuedTask> queue_;
  std::thread worker_;
  bool worker_should_stop_ = false;
  std::mutex shutdown_result_mutex_;
  xlang_bridge_status shutdown_status_ = XLANG_BRIDGE_STATUS_OK;
  std::string shutdown_error_;

  X::XLoad xload_;
  std::unique_ptr<X::Config> config_;
  X::XRuntime* runtime_ = nullptr;
  std::thread event_thread_;

  std::atomic<xlang_bridge_handle> next_handle_{1};
  std::mutex handle_mutex_;
  std::unordered_map<xlang_bridge_handle, X::Value> handles_;

  std::mutex subscription_mutex_;
  std::unordered_map<xlang_bridge_subscription_id, Subscription> subscriptions_;
  std::atomic<bool> accepting_event_callbacks_{false};
  std::atomic<uint64_t> active_event_callbacks_{0};
  std::mutex event_barrier_mutex_;
  std::condition_variable event_barrier_cv_;
};

template <typename Callback>
xlang_bridge_status GuardApiCall(Callback&& callback) noexcept {
  try {
    return std::forward<Callback>(callback)();
  } catch (...) {
    // Never propagate C++ exceptions across the versioned C ABI or across a
    // potentially different CRT/runtime in the Electron process.
    return XLANG_BRIDGE_STATUS_INTERNAL_ERROR;
  }
}

xlang_bridge_status ApiInitialize(
    xlang_bridge_request_id request_id,
    const xlang_bridge_initialize_options* options) noexcept {
  return GuardApiCall(
      [&]() { return Bridge::Get().Initialize(request_id, options); });
}

xlang_bridge_status ApiShutdown(xlang_bridge_request_id request_id) noexcept {
  return GuardApiCall([&]() { return Bridge::Get().Shutdown(request_id); });
}

xlang_bridge_status ApiImportModule(xlang_bridge_request_id request_id,
                                    xlang_bridge_bytes_view module_name,
                                    xlang_bridge_bytes_view from_path,
                                    xlang_bridge_bytes_view thru) noexcept {
  return GuardApiCall([&]() {
    return Bridge::Get().ImportModule(request_id, module_name, from_path, thru);
  });
}

xlang_bridge_status ApiGetMember(xlang_bridge_request_id request_id,
                                 xlang_bridge_handle object,
                                 xlang_bridge_bytes_view member_name) noexcept {
  return GuardApiCall([&]() {
    return Bridge::Get().GetMember(request_id, object, member_name);
  });
}

xlang_bridge_status ApiSetMember(xlang_bridge_request_id request_id,
                                 xlang_bridge_handle object,
                                 xlang_bridge_bytes_view member_name,
                                 const xlang_bridge_value* value) noexcept {
  return GuardApiCall([&]() {
    return Bridge::Get().SetMember(request_id, object, member_name, value);
  });
}

xlang_bridge_status ApiCall(xlang_bridge_request_id request_id,
                            xlang_bridge_handle callable,
                            const xlang_bridge_value* args,
                            uint32_t arg_count,
                            const xlang_bridge_named_value* kwargs,
                            uint32_t kwarg_count) noexcept {
  return GuardApiCall([&]() {
    return Bridge::Get().Call(request_id, callable, args, arg_count, kwargs,
                              kwarg_count);
  });
}

xlang_bridge_status ApiCallMember(xlang_bridge_request_id request_id,
                                  xlang_bridge_handle object,
                                  xlang_bridge_bytes_view member_name,
                                  const xlang_bridge_value* args,
                                  uint32_t arg_count,
                                  const xlang_bridge_named_value* kwargs,
                                  uint32_t kwarg_count) noexcept {
  return GuardApiCall([&]() {
    return Bridge::Get().CallMember(request_id, object, member_name, args,
                                    arg_count, kwargs, kwarg_count);
  });
}

xlang_bridge_status ApiEventOn(
    xlang_bridge_request_id request_id,
    xlang_bridge_handle object,
    xlang_bridge_bytes_view event_name,
    xlang_bridge_subscription_id subscription_id) noexcept {
  return GuardApiCall([&]() {
    return Bridge::Get().EventOn(request_id, object, event_name,
                                 subscription_id);
  });
}

xlang_bridge_status ApiEventOff(
    xlang_bridge_request_id request_id,
    xlang_bridge_subscription_id subscription_id) noexcept {
  return GuardApiCall(
      [&]() { return Bridge::Get().EventOff(request_id, subscription_id); });
}

xlang_bridge_status ApiReleaseHandle(xlang_bridge_handle handle) noexcept {
  return GuardApiCall([&]() { return Bridge::Get().ReleaseHandle(handle); });
}

}  // namespace

extern "C" XLANG_BRIDGE_EXPORT xlang_bridge_status
xlang_bridge_get_api(uint32_t requested_abi_version,
                     const xlang_bridge_host_callbacks* host_callbacks,
                     xlang_bridge_api* out_api) {
  return GuardApiCall([&]() {
    if (requested_abi_version != XLANG_BRIDGE_ABI_VERSION) {
      return XLANG_BRIDGE_STATUS_INCOMPATIBLE_ABI;
    }
    if (host_callbacks == nullptr ||
        host_callbacks->struct_size < kMinimumHostCallbacksSize ||
        host_callbacks->abi_version != XLANG_BRIDGE_ABI_VERSION ||
        host_callbacks->completion == nullptr || out_api == nullptr ||
        out_api->struct_size < kMinimumApiSize) {
      return XLANG_BRIDGE_STATUS_INVALID_ARGUMENT;
    }

    Bridge::Get().ConfigureHost(*host_callbacks);

    xlang_bridge_api api{};
    api.struct_size = sizeof(api);
    api.abi_version = XLANG_BRIDGE_ABI_VERSION;
    api.feature_flags = XLANG_BRIDGE_FEATURE_KWARGS |
                        XLANG_BRIDGE_FEATURE_BINARY |
                        XLANG_BRIDGE_FEATURE_EVENTS | XLANG_BRIDGE_FEATURE_LRPC;
    api.initialize = &ApiInitialize;
    api.shutdown = &ApiShutdown;
    api.import_module = &ApiImportModule;
    api.get_member = &ApiGetMember;
    api.set_member = &ApiSetMember;
    api.call = &ApiCall;
    api.call_member = &ApiCallMember;
    api.event_on = &ApiEventOn;
    api.event_off = &ApiEventOff;
    api.release_handle = &ApiReleaseHandle;

    const size_t caller_size = out_api->struct_size;
    const size_t copy_size = std::min(caller_size, sizeof(api));
    std::memcpy(out_api, &api, copy_size);
    out_api->struct_size = static_cast<uint32_t>(copy_size);
    return XLANG_BRIDGE_STATUS_OK;
  });
}
