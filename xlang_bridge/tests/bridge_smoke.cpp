/*
 * Copyright (c) 2026 XLang Foundation
 * SPDX-License-Identifier: MIT
 */

#include "xlang_bridge.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

xlang_bridge_bytes_view View(const std::string& value) {
  return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}

struct Result {
  xlang_bridge_status status = XLANG_BRIDGE_STATUS_INTERNAL_ERROR;
  xlang_bridge_value value{};
  bool has_value = false;
  std::string error;
};

struct EventValue {
  uint32_t type = XLANG_BRIDGE_VALUE_UNDEFINED;
  int32_t boolean_value = 0;
  int64_t int64_value = 0;
  double double_value = 0.0;
  uint64_t object_value = 0;
  std::string bytes;
};

struct NamedEventValue {
  std::string name;
  EventValue value;
};

struct EventResult {
  bool received = false;
  xlang_bridge_subscription_id subscription_id = 0;
  std::vector<EventValue> args;
  std::vector<NamedEventValue> kwargs;
  std::string error;
};

EventValue CopyEventValue(const xlang_bridge_value& input) {
  EventValue output;
  output.type = input.type;
  switch (input.type) {
    case XLANG_BRIDGE_VALUE_BOOL:
      output.boolean_value = input.data.boolean_value;
      break;
    case XLANG_BRIDGE_VALUE_INT64:
      output.int64_value = input.data.int64_value;
      break;
    case XLANG_BRIDGE_VALUE_DOUBLE:
      output.double_value = input.data.double_value;
      break;
    case XLANG_BRIDGE_VALUE_STRING:
    case XLANG_BRIDGE_VALUE_BINARY:
      if (input.data.bytes_value.data != nullptr &&
          input.data.bytes_value.size != 0) {
        output.bytes.assign(
            reinterpret_cast<const char*>(input.data.bytes_value.data),
            static_cast<size_t>(input.data.bytes_value.size));
      }
      break;
    case XLANG_BRIDGE_VALUE_HANDLE:
      output.object_value = input.data.handle_value;
      break;
    case XLANG_BRIDGE_VALUE_SUBSCRIPTION:
      output.object_value = input.data.subscription_value;
      break;
    default:
      break;
  }
  return output;
}

class Results {
 public:
  static void Completion(void* context,
                         xlang_bridge_request_id request_id,
                         xlang_bridge_status status,
                         const xlang_bridge_value* value,
                         xlang_bridge_bytes_view error) {
    auto* self = static_cast<Results*>(context);
    Result result;
    result.status = status;
    if (value != nullptr) {
      result.value = *value;
      result.has_value = true;
    }
    if (error.data != nullptr && error.size != 0) {
      result.error.assign(reinterpret_cast<const char*>(error.data),
                          static_cast<size_t>(error.size));
    }
    {
      std::lock_guard<std::mutex> lock(self->mutex_);
      self->results_.emplace(request_id, std::move(result));
    }
    self->ready_.notify_all();
  }

  static void Log(void*, int32_t level, xlang_bridge_bytes_view message) {
    std::cerr << "[bridge:" << level << "] ";
    if (message.data != nullptr && message.size != 0) {
      std::cerr.write(reinterpret_cast<const char*>(message.data),
                      static_cast<std::streamsize>(message.size));
    }
    std::cerr << '\n';
  }

  static void Event(void* context,
                    xlang_bridge_subscription_id subscription_id,
                    const xlang_bridge_value* args,
                    uint32_t arg_count,
                    const xlang_bridge_named_value* kwargs,
                    uint32_t kwarg_count) noexcept {
    auto* self = static_cast<Results*>(context);
    try {
      EventResult event;
      event.received = true;
      event.subscription_id = subscription_id;
      event.args.reserve(arg_count);
      for (uint32_t index = 0; index < arg_count; ++index) {
        event.args.push_back(CopyEventValue(args[index]));
      }
      event.kwargs.reserve(kwarg_count);
      for (uint32_t index = 0; index < kwarg_count; ++index) {
        NamedEventValue named;
        if (kwargs[index].name.data != nullptr &&
            kwargs[index].name.size != 0) {
          named.name.assign(
              reinterpret_cast<const char*>(kwargs[index].name.data),
              static_cast<size_t>(kwargs[index].name.size));
        }
        named.value = CopyEventValue(kwargs[index].value);
        event.kwargs.push_back(std::move(named));
      }
      {
        std::lock_guard<std::mutex> lock(self->mutex_);
        ++self->event_counts_[subscription_id];
        self->events_[subscription_id] = std::move(event);
      }
    } catch (const std::exception& error) {
      std::lock_guard<std::mutex> lock(self->mutex_);
      self->event_error_ = error.what();
    } catch (...) {
      std::lock_guard<std::mutex> lock(self->mutex_);
      self->event_error_ = "Unknown exception while copying bridge event";
    }
    self->ready_.notify_all();
  }

  Result Wait(xlang_bridge_request_id request_id) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!ready_.wait_for(lock, std::chrono::seconds(20), [&]() {
          return results_.find(request_id) != results_.end();
        })) {
      return {XLANG_BRIDGE_STATUS_INTERNAL_ERROR,
              {},
              false,
              "Timed out waiting for bridge completion"};
    }
    Result result = std::move(results_.at(request_id));
    results_.erase(request_id);
    return result;
  }

  Result WaitIndefinitely(xlang_bridge_request_id request_id) {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait(lock,
                [&]() { return results_.find(request_id) != results_.end(); });
    Result result = std::move(results_.at(request_id));
    results_.erase(request_id);
    return result;
  }

  EventResult WaitEvent(xlang_bridge_subscription_id subscription_id) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!ready_.wait_for(lock, std::chrono::seconds(20), [&]() {
          return events_.find(subscription_id) != events_.end() ||
                 !event_error_.empty();
        })) {
      EventResult result;
      result.error = "Timed out waiting for bridge event";
      return result;
    }
    if (!event_error_.empty()) {
      EventResult result;
      result.error = std::move(event_error_);
      event_error_.clear();
      return result;
    }
    EventResult result = std::move(events_.at(subscription_id));
    events_.erase(subscription_id);
    return result;
  }

  uint64_t EventCount(xlang_bridge_subscription_id subscription_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = event_counts_.find(subscription_id);
    return found == event_counts_.end() ? 0 : found->second;
  }

  bool EventCountChangesWithin(xlang_bridge_subscription_id subscription_id,
                               uint64_t expected_count,
                               std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return ready_.wait_for(lock, timeout, [&]() {
      const auto found = event_counts_.find(subscription_id);
      const uint64_t count = found == event_counts_.end() ? 0 : found->second;
      return count != expected_count || !event_error_.empty();
    });
  }

 private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::unordered_map<xlang_bridge_request_id, Result> results_;
  std::unordered_map<xlang_bridge_subscription_id, EventResult> events_;
  std::unordered_map<xlang_bridge_subscription_id, uint64_t> event_counts_;
  std::string event_error_;
};

bool Check(const char* operation, const Result& result) {
  if (result.status == XLANG_BRIDGE_STATUS_OK) {
    return true;
  }
  std::cerr << operation << " failed (" << result.status
            << "): " << result.error << '\n';
  return false;
}

const EventValue* FindKwarg(const EventResult& event, const char* name) {
  for (const auto& named : event.kwargs) {
    if (named.name == name) {
      return &named.value;
    }
  }
  return nullptr;
}

class ScopedBridgeHandle {
 public:
  ScopedBridgeHandle(const xlang_bridge_api& api, xlang_bridge_handle handle)
      : api_(api), handle_(handle) {}

  ScopedBridgeHandle(const ScopedBridgeHandle&) = delete;
  ScopedBridgeHandle& operator=(const ScopedBridgeHandle&) = delete;

  ~ScopedBridgeHandle() {
    if (handle_ != 0) {
      if (api_.release_handle(handle_) != XLANG_BRIDGE_STATUS_OK) {
        std::cerr << "bridge handle cleanup was not accepted\n";
      }
    }
  }

  bool Release() {
    if (handle_ == 0) {
      return true;
    }
    if (api_.release_handle(handle_) != XLANG_BRIDGE_STATUS_OK) {
      return false;
    }
    handle_ = 0;
    return true;
  }

 private:
  const xlang_bridge_api& api_;
  xlang_bridge_handle handle_;
};

class ScopedBridgeRuntime {
 public:
  ScopedBridgeRuntime(const xlang_bridge_api& api, Results& results)
      : api_(api), results_(results) {}

  ScopedBridgeRuntime(const ScopedBridgeRuntime&) = delete;
  ScopedBridgeRuntime& operator=(const ScopedBridgeRuntime&) = delete;

  ~ScopedBridgeRuntime() {
    if (!shutdown_needed_) {
      return;
    }
    constexpr xlang_bridge_request_id kFallbackShutdownRequest = 9000;
    const xlang_bridge_status status = api_.shutdown(kFallbackShutdownRequest);
    if (status != XLANG_BRIDGE_STATUS_OK) {
      std::cerr << "fallback shutdown was not accepted: " << status << '\n';
      return;
    }
    const Result result = results_.WaitIndefinitely(kFallbackShutdownRequest);
    shutdown_needed_ = false;
    if (!Check("fallback shutdown", result)) {
      std::cerr << "fallback shutdown did not complete cleanly\n";
    }
  }

  void MarkInitializeAccepted() { shutdown_needed_ = true; }

  bool Shutdown(xlang_bridge_request_id request_id) {
    if (!shutdown_needed_) {
      return true;
    }
    const xlang_bridge_status status = api_.shutdown(request_id);
    if (status != XLANG_BRIDGE_STATUS_OK) {
      std::cerr << "shutdown was not accepted: " << status << '\n';
      return false;
    }
    const Result result = results_.WaitIndefinitely(request_id);
    shutdown_needed_ = false;
    return Check("shutdown", result);
  }

 private:
  const xlang_bridge_api& api_;
  Results& results_;
  bool shutdown_needed_ = false;
};

bool RunEventScenario(const char* label,
                      const xlang_bridge_api& api,
                      Results& results,
                      xlang_bridge_request_id first_request_id,
                      xlang_bridge_subscription_id subscription_id) {
  const std::string event_module_name = "bridge_event_test";
  const std::string event_module_path = "xlang_bridge_event_test";
  if (api.import_module(first_request_id, View(event_module_name),
                        View(event_module_path),
                        {}) != XLANG_BRIDGE_STATUS_OK) {
    std::cerr << label << " import_module was not accepted\n";
    return false;
  }
  Result event_module = results.Wait(first_request_id);
  const std::string import_operation = std::string(label) + " import_module";
  if (!Check(import_operation.c_str(), event_module) ||
      !event_module.has_value ||
      event_module.value.type != XLANG_BRIDGE_VALUE_HANDLE) {
    return false;
  }
  ScopedBridgeHandle module_handle(api, event_module.value.data.handle_value);

  const std::string changed_event = "changed";
  if (api.event_on(first_request_id + 1, event_module.value.data.handle_value,
                   View(changed_event),
                   subscription_id) != XLANG_BRIDGE_STATUS_OK) {
    std::cerr << label << " event_on was not accepted\n";
    return false;
  }
  Result subscribed = results.Wait(first_request_id + 1);
  const std::string subscribe_operation = std::string(label) + " event_on";
  if (!Check(subscribe_operation.c_str(), subscribed) ||
      !subscribed.has_value ||
      subscribed.value.type != XLANG_BRIDGE_VALUE_SUBSCRIPTION ||
      subscribed.value.data.subscription_value != subscription_id) {
    std::cerr << label << " event_on returned an unexpected subscription\n";
    return false;
  }

  const std::string emit = "emit";
  const std::string event_message = "bridge-event";
  xlang_bridge_value event_arguments[2]{};
  event_arguments[0].struct_size = sizeof(event_arguments[0]);
  event_arguments[0].type = XLANG_BRIDGE_VALUE_INT64;
  event_arguments[0].data.int64_value = 17;
  event_arguments[1].struct_size = sizeof(event_arguments[1]);
  event_arguments[1].type = XLANG_BRIDGE_VALUE_STRING;
  event_arguments[1].data.bytes_value = View(event_message);
  if (api.call_member(first_request_id + 2,
                      event_module.value.data.handle_value, View(emit),
                      event_arguments, 2, nullptr,
                      0) != XLANG_BRIDGE_STATUS_OK) {
    std::cerr << label << " event emitter call was not accepted\n";
    return false;
  }
  Result emitted = results.Wait(first_request_id + 2);
  const std::string emit_operation = std::string(label) + " event emitter";
  if (!Check(emit_operation.c_str(), emitted) || !emitted.has_value ||
      emitted.value.type != XLANG_BRIDGE_VALUE_BOOL ||
      emitted.value.data.boolean_value == 0) {
    return false;
  }

  const EventResult event = results.WaitEvent(subscription_id);
  const EventValue* source = FindKwarg(event, "source");
  if (!event.received || !event.error.empty() ||
      event.subscription_id != subscription_id || event.args.size() != 2 ||
      event.args[0].type != XLANG_BRIDGE_VALUE_INT64 ||
      event.args[0].int64_value != 17 ||
      event.args[1].type != XLANG_BRIDGE_VALUE_STRING ||
      event.args[1].bytes != event_message || event.kwargs.size() != 1 ||
      source == nullptr || source->type != XLANG_BRIDGE_VALUE_STRING ||
      source->bytes != "native-test-module") {
    std::cerr << label
              << " event callback did not preserve expected args/kwargs: "
              << event.error << '\n';
    return false;
  }

  if (api.event_off(first_request_id + 3, subscription_id) !=
      XLANG_BRIDGE_STATUS_OK) {
    std::cerr << label << " event_off was not accepted\n";
    return false;
  }
  Result unsubscribed = results.Wait(first_request_id + 3);
  const std::string unsubscribe_operation = std::string(label) + " event_off";
  if (!Check(unsubscribe_operation.c_str(), unsubscribed) ||
      !unsubscribed.has_value ||
      unsubscribed.value.type != XLANG_BRIDGE_VALUE_BOOL ||
      unsubscribed.value.data.boolean_value == 0) {
    return false;
  }

  const uint64_t event_count_after_unsubscribe =
      results.EventCount(subscription_id);
  if (api.call_member(first_request_id + 4,
                      event_module.value.data.handle_value, View(emit),
                      event_arguments, 2, nullptr,
                      0) != XLANG_BRIDGE_STATUS_OK) {
    std::cerr << label << " post-unsubscribe emitter call was not accepted\n";
    return false;
  }
  const std::string post_off_operation =
      std::string(label) + " post-unsubscribe event emitter";
  if (!Check(post_off_operation.c_str(), results.Wait(first_request_id + 4))) {
    return false;
  }
  if (results.EventCountChangesWithin(subscription_id,
                                      event_count_after_unsubscribe,
                                      std::chrono::milliseconds(500))) {
    std::cerr << label << " event callback fired after event_off completed\n";
    return false;
  }

  if (!module_handle.Release()) {
    std::cerr << label << " release_handle was not accepted\n";
    return false;
  }
  return true;
}

bool RunModuleProbe(const xlang_bridge_api& api,
                    Results& results,
                    xlang_bridge_request_id first_request_id,
                    const std::string& module_name,
                    const std::string& from_path,
                    const std::string& member_name) {
  if (api.import_module(first_request_id, View(module_name), View(from_path),
                        {}) != XLANG_BRIDGE_STATUS_OK) {
    std::cerr << "module probe import was not accepted\n";
    return false;
  }
  Result imported = results.Wait(first_request_id);
  if (!Check("module probe import", imported) || !imported.has_value ||
      imported.value.type != XLANG_BRIDGE_VALUE_HANDLE) {
    return false;
  }
  ScopedBridgeHandle module_handle(api, imported.value.data.handle_value);

  if (!member_name.empty()) {
    if (api.call_member(first_request_id + 1, imported.value.data.handle_value,
                        View(member_name), nullptr, 0, nullptr,
                        0) != XLANG_BRIDGE_STATUS_OK) {
      std::cerr << "module probe member call was not accepted\n";
      return false;
    }
    Result called = results.Wait(first_request_id + 1);
    if (!Check("module probe member call", called)) {
      return false;
    }
    if (called.has_value && called.value.type == XLANG_BRIDGE_VALUE_HANDLE &&
        api.release_handle(called.value.data.handle_value) !=
            XLANG_BRIDGE_STATUS_OK) {
      std::cerr << "module probe result release was not accepted\n";
      return false;
    }
  }

  if (!module_handle.Release()) {
    std::cerr << "module probe release_handle was not accepted\n";
    return false;
  }
  std::cout << "XLang local module probe passed: " << module_name;
  if (!member_name.empty()) {
    std::cout << '.' << member_name << "()";
  }
  std::cout << '\n';
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const bool has_module_probe =
      (argc == 5 || argc == 6) && std::string(argv[2]) == "--probe";
  if (argc != 2 && !has_module_probe) {
    std::cerr << "usage: electron_xlang_bridge_smoke <runtime-directory>\n"
              << "       electron_xlang_bridge_smoke <runtime-directory> "
                 "--probe <module-name> <from-path> [member]\n";
    return 2;
  }
  const std::string probe_module_name = has_module_probe ? argv[3] : "";
  const std::string probe_from_path = has_module_probe ? argv[4] : "";
  const std::string probe_member_name =
      has_module_probe && argc == 6 ? argv[5] : "";
  if (has_module_probe &&
      (probe_module_name.empty() || probe_from_path.empty())) {
    std::cerr << "module-name and from-path must not be empty\n";
    return 2;
  }

  Results results;
  xlang_bridge_host_callbacks callbacks{};
  callbacks.struct_size = sizeof(callbacks);
  callbacks.abi_version = XLANG_BRIDGE_ABI_VERSION;
  callbacks.user_data = &results;
  callbacks.completion = &Results::Completion;
  callbacks.event = &Results::Event;
  callbacks.log = &Results::Log;

  xlang_bridge_api api{};
  api.struct_size = sizeof(api);
  const auto get_status =
      xlang_bridge_get_api(XLANG_BRIDGE_ABI_VERSION, &callbacks, &api);
  if (get_status != XLANG_BRIDGE_STATUS_OK ||
      api.abi_version != XLANG_BRIDGE_ABI_VERSION) {
    std::cerr << "xlang_bridge_get_api failed: " << get_status << '\n';
    return 1;
  }
  if ((api.feature_flags & XLANG_BRIDGE_FEATURE_EVENTS) == 0) {
    std::cerr << "bridge does not advertise event support\n";
    return 1;
  }
  ScopedBridgeRuntime runtime(api, results);

  const std::string runtime_directory = argv[1];
  std::error_code path_error;
  const std::filesystem::path executable_path =
      std::filesystem::absolute(argv[0], path_error);
  if (path_error || !executable_path.has_parent_path()) {
    std::cerr << "unable to locate test module beside smoke executable\n";
    return 1;
  }
  const std::string test_module_directory =
      executable_path.parent_path().string();
  std::string library_search_paths = test_module_directory;
  if (has_module_probe) {
    const std::filesystem::path probe_path =
        std::filesystem::absolute(probe_from_path, path_error);
    if (path_error || !probe_path.has_parent_path()) {
      std::cerr << "unable to resolve module probe path\n";
      return 2;
    }
    library_search_paths.push_back('\n');
    library_search_paths.append(probe_path.parent_path().string());
  }
  xlang_bridge_initialize_options options{};
  options.struct_size = sizeof(options);
  options.app_path = View(runtime_directory);
  options.library_search_paths = View(library_search_paths);

  const uint8_t sentinel = 0;
  const xlang_bridge_bytes_view oversized_view{
      &sentinel, std::numeric_limits<uint64_t>::max()};
  xlang_bridge_initialize_options oversized_options = options;
  oversized_options.app_path = oversized_view;
  if (api.initialize(99, &oversized_options) !=
      XLANG_BRIDGE_STATUS_INVALID_ARGUMENT) {
    std::cerr << "oversized initialize view should be rejected\n";
    return 1;
  }

  const std::string missing_runtime_directory =
      runtime_directory + "/does-not-exist";
  xlang_bridge_initialize_options missing_options = options;
  missing_options.app_path = View(missing_runtime_directory);
  missing_options.library_search_paths = {};
  if (api.initialize(100, &missing_options) != XLANG_BRIDGE_STATUS_OK) {
    std::cerr << "failed initialize was not accepted for asynchronous test\n";
    return 1;
  }
  runtime.MarkInitializeAccepted();
  const Result failed_initialize = results.Wait(100);
  if (failed_initialize.status != XLANG_BRIDGE_STATUS_ENGINE_LOAD_FAILED) {
    std::cerr << "missing runtime should fail initialization safely: "
              << failed_initialize.status << '\n';
    return 1;
  }

  const auto initialize_status = api.initialize(1, &options);
  if (initialize_status != XLANG_BRIDGE_STATUS_OK) {
    std::cerr << "initialize was not accepted: " << initialize_status << '\n';
    return 1;
  }
  runtime.MarkInitializeAccepted();
  if (!Check("initialize", results.Wait(1))) {
    return 1;
  }

  if (!RunEventScenario("local event", api, results, 20, 7001)) {
    return 1;
  }
  if (has_module_probe && !RunModuleProbe(api, results, 40, probe_module_name,
                                          probe_from_path, probe_member_name)) {
    return 1;
  }

  const std::string module_name = "yaml";
  const std::string from_path = "xlang_yaml";
  const xlang_bridge_bytes_view invalid_optional_view{nullptr, 1};
  if (api.import_module(101, View(module_name), invalid_optional_view, {}) !=
      XLANG_BRIDGE_STATUS_INVALID_ARGUMENT) {
    std::cerr << "invalid optional import view should be rejected\n";
    return 1;
  }
  if (api.import_module(2, View(module_name), View(from_path), {}) !=
      XLANG_BRIDGE_STATUS_OK) {
    std::cerr << "import_module was not accepted\n";
    return 1;
  }
  Result imported = results.Wait(2);
  if (!Check("import_module", imported) || !imported.has_value ||
      imported.value.type != XLANG_BRIDGE_VALUE_HANDLE) {
    return 1;
  }

  const std::string loads = "loads";
  const std::string yaml = "answer: 42\n";
  xlang_bridge_value argument{};
  argument.struct_size = sizeof(argument);
  argument.type = XLANG_BRIDGE_VALUE_STRING;
  argument.data.bytes_value = View(yaml);
  if (api.call_member(102, imported.value.data.handle_value, View(loads),
                      &argument, std::numeric_limits<uint32_t>::max(), nullptr,
                      0) != XLANG_BRIDGE_STATUS_INVALID_ARGUMENT) {
    std::cerr << "oversized argument count should be rejected\n";
    return 1;
  }
  xlang_bridge_value oversized_argument = argument;
  oversized_argument.data.bytes_value = oversized_view;
  if (api.call_member(103, imported.value.data.handle_value, View(loads),
                      &oversized_argument, 1, nullptr,
                      0) != XLANG_BRIDGE_STATUS_INVALID_ARGUMENT) {
    std::cerr << "oversized argument view should be rejected\n";
    return 1;
  }
  if (api.call_member(3, imported.value.data.handle_value, View(loads),
                      &argument, 1, nullptr, 0) != XLANG_BRIDGE_STATUS_OK) {
    std::cerr << "call_member was not accepted\n";
    return 1;
  }
  Result loaded = results.Wait(3);
  if (!Check("yaml.loads", loaded) || !loaded.has_value ||
      loaded.value.type != XLANG_BRIDGE_VALUE_HANDLE) {
    return 1;
  }

  const std::string answer = "answer";
  if (api.get_member(4, loaded.value.data.handle_value, View(answer)) !=
      XLANG_BRIDGE_STATUS_OK) {
    std::cerr << "get_member was not accepted\n";
    return 1;
  }
  Result original_answer = results.Wait(4);
  if (!Check("get answer", original_answer) || !original_answer.has_value ||
      original_answer.value.type != XLANG_BRIDGE_VALUE_INT64 ||
      original_answer.value.data.int64_value != 42) {
    std::cerr << "expected answer to equal 42\n";
    return 1;
  }

  xlang_bridge_value replacement{};
  replacement.struct_size = sizeof(replacement);
  replacement.type = XLANG_BRIDGE_VALUE_INT64;
  replacement.data.int64_value = 43;
  if (api.set_member(5, loaded.value.data.handle_value, View(answer),
                     &replacement) != XLANG_BRIDGE_STATUS_OK) {
    std::cerr << "set_member was not accepted\n";
    return 1;
  }
  Result set_answer = results.Wait(5);
  if (!Check("set answer", set_answer) || !set_answer.has_value ||
      set_answer.value.type != XLANG_BRIDGE_VALUE_BOOL ||
      set_answer.value.data.boolean_value == 0) {
    return 1;
  }

  if (api.get_member(6, loaded.value.data.handle_value, View(answer)) !=
      XLANG_BRIDGE_STATUS_OK) {
    std::cerr << "second get_member was not accepted\n";
    return 1;
  }
  Result changed_answer = results.Wait(6);
  if (!Check("get changed answer", changed_answer) ||
      !changed_answer.has_value ||
      changed_answer.value.type != XLANG_BRIDGE_VALUE_INT64 ||
      changed_answer.value.data.int64_value != 43) {
    std::cerr << "expected changed answer to equal 43\n";
    return 1;
  }

  api.release_handle(loaded.value.data.handle_value);
  api.release_handle(imported.value.data.handle_value);
  if (!runtime.Shutdown(7)) {
    return 1;
  }

  // A completed shutdown is process-final. XLang extension DLLs currently
  // retain process-global references to xlang_eng, so attempting a second
  // engine Load after imported modules were torn down is unsafe.
  const auto restart_status = api.initialize(8, &options);
  if (restart_status != XLANG_BRIDGE_STATUS_UNSUPPORTED) {
    std::cerr << "post-shutdown initialize should be rejected safely: "
              << restart_status << '\n';
    return 1;
  }

  std::cout << "XLang bridge smoke test passed\n";
  return 0;
}
