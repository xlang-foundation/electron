// Copyright (c) 2026 XLang Foundation
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/api/electron_api_xlang.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/scoped_native_library.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "build/build_config.h"
#include "gin/converter.h"
#include "gin/dictionary.h"
#include "shell/browser/javascript_environment.h"
#include "shell/common/gin_converters/file_path_converter.h"
#include "shell/common/gin_helper/cleaned_up_at_exit.h"
#include "shell/common/gin_helper/dictionary.h"
#include "shell/common/gin_helper/promise.h"
#include "shell/common/node_includes.h"
#include "shell/common/node_util.h"
#include "shell/common/thread_restrictions.h"
#include "xlang_bridge/include/xlang_bridge.h"

namespace electron::api {

namespace {

constexpr char kBridgeEntryPoint[] = "xlang_bridge_get_api";

#if BUILDFLAG(IS_WIN)
constexpr char kBridgeLibraryName[] = "electron_xlang_bridge.dll";
#elif BUILDFLAG(IS_MAC)
constexpr char kBridgeLibraryName[] = "electron_xlang_bridge.dylib";
#else
constexpr char kBridgeLibraryName[] = "electron_xlang_bridge.so";
#endif

const char* StatusName(xlang_bridge_status status) {
  switch (status) {
    case XLANG_BRIDGE_STATUS_OK:
      return "ok";
    case XLANG_BRIDGE_STATUS_INVALID_ARGUMENT:
      return "invalid argument";
    case XLANG_BRIDGE_STATUS_INCOMPATIBLE_ABI:
      return "incompatible ABI";
    case XLANG_BRIDGE_STATUS_NOT_INITIALIZED:
      return "not initialized";
    case XLANG_BRIDGE_STATUS_ALREADY_INITIALIZED:
      return "already initialized";
    case XLANG_BRIDGE_STATUS_ENGINE_LOAD_FAILED:
      return "engine load failed";
    case XLANG_BRIDGE_STATUS_IMPORT_FAILED:
      return "module import failed";
    case XLANG_BRIDGE_STATUS_HANDLE_NOT_FOUND:
      return "handle not found";
    case XLANG_BRIDGE_STATUS_MEMBER_NOT_FOUND:
      return "member not found";
    case XLANG_BRIDGE_STATUS_CALL_FAILED:
      return "call failed";
    case XLANG_BRIDGE_STATUS_UNSUPPORTED:
      return "unsupported";
    case XLANG_BRIDGE_STATUS_SHUTTING_DOWN:
      return "shutting down";
    case XLANG_BRIDGE_STATUS_INTERNAL_ERROR:
      return "internal error";
    case XLANG_BRIDGE_STATUS_EVENT_FAILED:
      return "event operation failed";
  }
  return "unknown bridge error";
}

std::string StatusMessage(xlang_bridge_status status,
                          std::string_view detail = {}) {
  std::string message = "XLang bridge: ";
  message.append(StatusName(status));
  if (!detail.empty()) {
    message.append(": ");
    message.append(detail);
  }
  return message;
}

xlang_bridge_bytes_view BorrowedBytes(std::string_view value) {
  return {
      .data = reinterpret_cast<const uint8_t*>(value.data()),
      .size = static_cast<uint64_t>(value.size()),
  };
}

std::optional<std::string> CopyString(xlang_bridge_bytes_view view) {
  if (view.size == 0)
    return std::string();
  if (!view.data ||
      view.size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    return std::nullopt;
  }
  return std::string(reinterpret_cast<const char*>(view.data),
                     static_cast<size_t>(view.size));
}

struct OwnedBridgeValue {
  uint32_t type = XLANG_BRIDGE_VALUE_UNDEFINED;
  uint32_t flags = 0;
  int32_t boolean_value = 0;
  int64_t int64_value = 0;
  double double_value = 0;
  uint64_t handle_value = 0;
  std::vector<uint8_t> bytes;
};

OwnedBridgeValue CopyBridgeValue(const xlang_bridge_value* value) {
  OwnedBridgeValue copy;
  constexpr size_t kMinimumValueSize =
      offsetof(xlang_bridge_value, data) +
      sizeof(decltype(xlang_bridge_value::data));
  if (!value || value->struct_size < kMinimumValueSize)
    return copy;

  copy.type = value->type;
  copy.flags = value->flags;
  switch (value->type) {
    case XLANG_BRIDGE_VALUE_BOOL:
      copy.boolean_value = value->data.boolean_value;
      break;
    case XLANG_BRIDGE_VALUE_INT64:
      copy.int64_value = value->data.int64_value;
      break;
    case XLANG_BRIDGE_VALUE_DOUBLE:
      copy.double_value = value->data.double_value;
      break;
    case XLANG_BRIDGE_VALUE_HANDLE:
      copy.handle_value = value->data.handle_value;
      break;
    case XLANG_BRIDGE_VALUE_SUBSCRIPTION:
      copy.handle_value = value->data.subscription_value;
      break;
    case XLANG_BRIDGE_VALUE_STRING:
    case XLANG_BRIDGE_VALUE_BINARY: {
      const auto view = value->data.bytes_value;
      if (view.data && view.size <= static_cast<uint64_t>(
                                        std::numeric_limits<size_t>::max())) {
        copy.bytes.assign(view.data,
                          view.data + static_cast<size_t>(view.size));
      }
      break;
    }
    case XLANG_BRIDGE_VALUE_UNDEFINED:
    case XLANG_BRIDGE_VALUE_NULL:
      break;
  }
  return copy;
}

struct OwnedNamedValue {
  std::string name;
  OwnedBridgeValue value;
};

class XLangBridgeBinding;

// This object deliberately outlives the JavaScript binding if XLang is still
// shutting down during process teardown. It is tiny, and the operating system
// reclaims it with the process. Keeping the callback target alive prevents a
// late bridge callback from dereferencing a destroyed binding.
class CallbackState : public base::RefCountedThreadSafe<CallbackState> {
 public:
  CallbackState(XLangBridgeBinding* binding,
                scoped_refptr<base::SequencedTaskRunner> task_runner)
      : owner(binding), task_runner(std::move(task_runner)) {}

  std::atomic<XLangBridgeBinding*> owner;
  scoped_refptr<base::SequencedTaskRunner> task_runner;

 private:
  friend class base::RefCountedThreadSafe<CallbackState>;
  ~CallbackState() = default;
};

struct TaggedValueStorage {
  xlang_bridge_value native = {
      .struct_size = sizeof(xlang_bridge_value),
      .type = XLANG_BRIDGE_VALUE_UNDEFINED,
  };
  std::string string_value;
  std::vector<uint8_t> binary_value;

  void RefreshView() {
    if (native.type == XLANG_BRIDGE_VALUE_STRING)
      native.data.bytes_value = BorrowedBytes(string_value);
    if (native.type == XLANG_BRIDGE_VALUE_BINARY) {
      native.data.bytes_value = {
          .data = binary_value.data(),
          .size = static_cast<uint64_t>(binary_value.size()),
      };
    }
  }
};

struct NamedValueStorage {
  std::string name;
  TaggedValueStorage value;
  xlang_bridge_named_value native = {};

  void RefreshView() {
    value.RefreshView();
    native.name = BorrowedBytes(name);
    native.value = value.native;
  }
};

bool ReadBigInt(v8::Local<v8::Value> input, uint64_t* output) {
  if (!input->IsBigInt())
    return false;
  bool lossless = false;
  *output = input.As<v8::BigInt>()->Uint64Value(&lossless);
  return lossless;
}

bool ReadTaggedValue(v8::Isolate* isolate,
                     v8::Local<v8::Value> input,
                     TaggedValueStorage* output,
                     std::string* error) {
  if (!input->IsObject() || input->IsNull()) {
    *error = "XLang values must use the internal tagged-value format";
    return false;
  }

  gin_helper::Dictionary dictionary(isolate, input.As<v8::Object>());
  std::string type;
  if (!dictionary.Get("type", &type)) {
    *error = "XLang tagged value is missing a string 'type'";
    return false;
  }

  v8::Local<v8::Value> value;
  if (type == "undefined") {
    output->native.type = XLANG_BRIDGE_VALUE_UNDEFINED;
  } else if (type == "null") {
    output->native.type = XLANG_BRIDGE_VALUE_NULL;
  } else if (type == "boolean") {
    bool converted = false;
    if (!dictionary.Get("value", &converted)) {
      *error = "XLang boolean value must contain a boolean 'value'";
      return false;
    }
    output->native.type = XLANG_BRIDGE_VALUE_BOOL;
    output->native.data.boolean_value = converted;
  } else if (type == "int64") {
    if (!dictionary.Get("value", &value) || !value->IsBigInt()) {
      *error = "XLang int64 value must contain a BigInt 'value'";
      return false;
    }
    bool lossless = false;
    const int64_t converted = value.As<v8::BigInt>()->Int64Value(&lossless);
    if (!lossless) {
      *error = "XLang int64 value is outside the signed 64-bit range";
      return false;
    }
    output->native.type = XLANG_BRIDGE_VALUE_INT64;
    output->native.data.int64_value = converted;
  } else if (type == "double") {
    double converted = 0;
    if (!dictionary.Get("value", &converted)) {
      *error = "XLang double value must contain a number 'value'";
      return false;
    }
    output->native.type = XLANG_BRIDGE_VALUE_DOUBLE;
    output->native.data.double_value = converted;
  } else if (type == "string") {
    if (!dictionary.Get("value", &output->string_value)) {
      *error = "XLang string value must contain a string 'value'";
      return false;
    }
    output->native.type = XLANG_BRIDGE_VALUE_STRING;
  } else if (type == "binary") {
    if (!dictionary.Get("value", &value) || !node::Buffer::HasInstance(value)) {
      *error = "XLang binary value must contain a Buffer 'value'";
      return false;
    }
    const auto* data =
        reinterpret_cast<const uint8_t*>(node::Buffer::Data(value));
    const size_t size = node::Buffer::Length(value);
    if (size > 0)
      output->binary_value.assign(data, data + size);
    output->native.type = XLANG_BRIDGE_VALUE_BINARY;
  } else if (type == "handle") {
    if (!dictionary.Get("value", &value) ||
        !ReadBigInt(value, &output->native.data.handle_value)) {
      *error = "XLang handle value must contain an unsigned BigInt 'value'";
      return false;
    }
    uint32_t object_type = 0;
    dictionary.Get("objectType", &object_type);
    output->native.type = XLANG_BRIDGE_VALUE_HANDLE;
    output->native.flags = object_type;
  } else if (type == "subscription") {
    if (!dictionary.Get("value", &value) ||
        !ReadBigInt(value, &output->native.data.subscription_value)) {
      *error =
          "XLang subscription value must contain an unsigned BigInt 'value'";
      return false;
    }
    output->native.type = XLANG_BRIDGE_VALUE_SUBSCRIPTION;
  } else {
    *error = "Unknown XLang tagged value type: " + type;
    return false;
  }

  output->RefreshView();
  return true;
}

bool ReadTaggedArray(v8::Isolate* isolate,
                     v8::Local<v8::Value> input,
                     std::vector<TaggedValueStorage>* storage,
                     std::vector<xlang_bridge_value>* values,
                     std::string* error) {
  if (!input->IsArray()) {
    *error = "XLang call arguments must be an array";
    return false;
  }

  auto array = input.As<v8::Array>();
  storage->resize(array->Length());
  values->resize(array->Length());
  auto context = isolate->GetCurrentContext();
  for (uint32_t i = 0; i < array->Length(); ++i) {
    v8::Local<v8::Value> item;
    if (!array->Get(context, i).ToLocal(&item) ||
        !ReadTaggedValue(isolate, item, &(*storage)[i], error)) {
      return false;
    }
  }
  for (uint32_t i = 0; i < array->Length(); ++i) {
    (*storage)[i].RefreshView();
    (*values)[i] = (*storage)[i].native;
  }
  return true;
}

bool ReadTaggedKwargs(v8::Isolate* isolate,
                      v8::Local<v8::Value> input,
                      std::vector<NamedValueStorage>* storage,
                      std::vector<xlang_bridge_named_value>* values,
                      std::string* error) {
  if (input->IsUndefined() || input->IsNull())
    return true;
  if (!input->IsObject() || input->IsArray()) {
    *error = "XLang keyword arguments must be an object";
    return false;
  }

  auto context = isolate->GetCurrentContext();
  auto object = input.As<v8::Object>();
  v8::Local<v8::Array> keys;
  if (!object->GetOwnPropertyNames(context).ToLocal(&keys)) {
    *error = "Could not enumerate XLang keyword arguments";
    return false;
  }

  storage->resize(keys->Length());
  values->resize(keys->Length());
  for (uint32_t i = 0; i < keys->Length(); ++i) {
    v8::Local<v8::Value> key;
    v8::Local<v8::Value> value;
    if (!keys->Get(context, i).ToLocal(&key) || !key->IsString() ||
        !object->Get(context, key).ToLocal(&value) ||
        !gin::ConvertFromV8(isolate, key, &(*storage)[i].name) ||
        !ReadTaggedValue(isolate, value, &(*storage)[i].value, error)) {
      if (error->empty())
        *error = "Could not read an XLang keyword argument";
      return false;
    }
  }
  for (uint32_t i = 0; i < keys->Length(); ++i) {
    (*storage)[i].RefreshView();
    (*values)[i] = (*storage)[i].native;
  }
  return true;
}

enum class PendingKind {
  kNormal,
  kInitialize,
  kShutdown,
  kEventOn,
  kEventOff,
};

struct PendingRequest {
  PendingRequest(gin_helper::Promise<v8::Local<v8::Value>> promise,
                 PendingKind kind,
                 xlang_bridge_subscription_id subscription_id)
      : promise(std::move(promise)),
        kind(kind),
        subscription_id(subscription_id) {}

  PendingRequest(PendingRequest&&) = default;
  PendingRequest& operator=(PendingRequest&&) = default;

  gin_helper::Promise<v8::Local<v8::Value>> promise;
  PendingKind kind;
  xlang_bridge_subscription_id subscription_id;
};

struct QueuedEvent {
  std::vector<OwnedBridgeValue> args;
  std::vector<OwnedNamedValue> kwargs;
};

struct EventHandler {
  EventHandler(v8::Isolate* isolate, v8::Local<v8::Function> function)
      : callback(isolate, function) {}

  EventHandler(EventHandler&&) = default;
  EventHandler& operator=(EventHandler&&) = default;

  v8::Global<v8::Function> callback;
  // Events can be raised synchronously by XLang while event_on() is still
  // registering. Buffer them until the registration promise has settled so JS
  // always receives its subscription token before its first callback.
  bool active = false;
  std::vector<QueuedEvent> queued_events;
};

class XLangBridgeBinding : public gin_helper::CleanedUpAtExit {
 public:
  static XLangBridgeBinding* GetInstance() {
    // Deleted by gin_helper::CleanedUpAtExit immediately before V8 teardown.
    static XLangBridgeBinding* instance = new XLangBridgeBinding();
    return instance;
  }

  XLangBridgeBinding(const XLangBridgeBinding&) = delete;
  XLangBridgeBinding& operator=(const XLangBridgeBinding&) = delete;

  ~XLangBridgeBinding() override {
    const bool bridge_may_call_back = shutdown_required_ || initialized_ ||
                                      initializing_ || shutting_down_ ||
                                      !pending_.empty();
    if (bridge_may_call_back) {
      // The process is exiting without a completed shutdown. Keep the small
      // callback state alive for any final native callbacks.
      callback_state_->AddRef();
    }
    callback_state_->owner.store(nullptr, std::memory_order_release);

    ClearEventHandlers();
    pending_.clear();

    if (bridge_may_call_back && api_.shutdown) {
      api_.shutdown(NextRequestId());
      // The async shutdown callback may still execute. Keep the native module
      // mapped until process exit so its worker and callback code remain valid.
      [[maybe_unused]] base::NativeLibrary leaked_library = library_.release();
    }
  }

  bool Load(v8::Isolate* isolate, const base::FilePath& requested_path) {
    electron::ScopedAllowBlockingForElectron allow_blocking;
    base::FilePath resolved_path = requested_path;
    if (base::DirectoryExists(resolved_path))
      resolved_path = resolved_path.AppendASCII(kBridgeLibraryName);
    if (base::FilePath absolute_path =
            base::MakeAbsoluteFilePath(resolved_path);
        !absolute_path.empty()) {
      resolved_path = std::move(absolute_path);
    }

    if (library_.is_valid()) {
      if (resolved_path == library_path_)
        return true;
      isolate->ThrowException(v8::Exception::Error(gin::StringToV8(
          isolate,
          "The XLang bridge is already loaded from a different path")));
      return false;
    }

    base::ScopedNativeLibrary candidate(resolved_path);
    if (!candidate.is_valid()) {
      std::string detail;
      if (candidate.GetError())
        detail = candidate.GetError()->ToString();
      isolate->ThrowException(v8::Exception::Error(gin::StringToV8(
          isolate, "Unable to load XLang bridge '" +
                       resolved_path.AsUTF8Unsafe() + "': " + detail)));
      return false;
    }

    auto* entry_point = reinterpret_cast<xlang_bridge_get_api_fn>(
        candidate.GetFunctionPointer(kBridgeEntryPoint));
    if (!entry_point) {
      isolate->ThrowException(v8::Exception::Error(gin::StringToV8(
          isolate, "XLang bridge does not export xlang_bridge_get_api")));
      return false;
    }

    xlang_bridge_api candidate_api = {};
    candidate_api.struct_size = sizeof(candidate_api);
    const xlang_bridge_status status =
        entry_point(XLANG_BRIDGE_ABI_VERSION, &host_callbacks_, &candidate_api);
    constexpr size_t kRequiredApiSize =
        offsetof(xlang_bridge_api, release_handle) +
        sizeof(decltype(xlang_bridge_api::release_handle));
    if (status != XLANG_BRIDGE_STATUS_OK ||
        candidate_api.abi_version != XLANG_BRIDGE_ABI_VERSION ||
        candidate_api.struct_size < kRequiredApiSize ||
        !candidate_api.initialize || !candidate_api.shutdown ||
        !candidate_api.import_module || !candidate_api.get_member ||
        !candidate_api.set_member || !candidate_api.call ||
        !candidate_api.call_member || !candidate_api.event_on ||
        !candidate_api.event_off || !candidate_api.release_handle) {
      isolate->ThrowException(v8::Exception::Error(gin::StringToV8(
          isolate, status != XLANG_BRIDGE_STATUS_OK
                       ? StatusMessage(status)
                       : "XLang bridge returned an incomplete ABI v1 table")));
      return false;
    }

    api_ = candidate_api;
    library_path_ = resolved_path;
    library_ = std::move(candidate);
    return true;
  }

  bool IsLoaded() const { return library_.is_valid(); }

  gin_helper::Dictionary GetBridgeInfo(v8::Isolate* isolate) const {
    auto result = gin_helper::Dictionary::CreateEmpty(isolate);
    result.Set("loaded", IsLoaded());
    if (IsLoaded()) {
      result.Set("path", library_path_);
      result.Set("abiVersion", api_.abi_version);
      result.Set("featureFlags",
                 v8::BigInt::NewFromUnsigned(isolate, api_.feature_flags));
    }
    return result;
  }

  v8::Local<v8::Promise> Initialize(v8::Isolate* isolate,
                                    const gin_helper::Dictionary& options) {
    if (initializing_ || initialized_) {
      return RejectedPromise(isolate,
                             "The XLang bridge is already initialized");
    }

    std::string app_path;
    options.Get("appPath", &app_path);
    if (app_path.empty() && !library_path_.empty())
      app_path = library_path_.DirName().AsUTF8Unsafe();
    if (app_path.empty()) {
      return RejectedPromise(
          isolate,
          "xlang.initialize requires appPath when no bridge has been loaded");
    }

    std::vector<std::string> search_paths;
    options.Get("librarySearchPaths", &search_paths);
    const std::string joined_search_paths =
        base::JoinString(search_paths, "\n");
    uint32_t flags = 0;
    if (options.ValueOrDefault("enablePython", false))
      flags |= XLANG_BRIDGE_INIT_ENABLE_PYTHON;
    if (options.ValueOrDefault("debug", false))
      flags |= XLANG_BRIDGE_INIT_DEBUG;

    xlang_bridge_initialize_options native_options = {
        .struct_size = sizeof(xlang_bridge_initialize_options),
        .flags = flags,
        .app_path = BorrowedBytes(app_path),
        .library_search_paths = BorrowedBytes(joined_search_paths),
    };

    initializing_ = true;
    return Submit(isolate, PendingKind::kInitialize, 0,
                  [this, &native_options](xlang_bridge_request_id request_id) {
                    return api_.initialize(request_id, &native_options);
                  });
  }

  v8::Local<v8::Promise> Shutdown(v8::Isolate* isolate) {
    if (shutting_down_)
      return RejectedPromise(isolate, "The XLang bridge is shutting down");
    if (!shutdown_required_ && !initializing_ && !initialized_) {
      gin_helper::Promise<v8::Local<v8::Value>> promise(isolate);
      auto result = promise.GetHandle();
      promise.Resolve(v8::Undefined(isolate));
      return result;
    }
    shutting_down_ = true;
    return Submit(isolate, PendingKind::kShutdown, 0,
                  [this](xlang_bridge_request_id request_id) {
                    return api_.shutdown(request_id);
                  });
  }

  v8::Local<v8::Promise> ImportModule(v8::Isolate* isolate,
                                      const std::string& module_name,
                                      const gin_helper::Dictionary& options) {
    std::string from_path;
    std::string thru;
    options.Get("fromPath", &from_path);
    options.Get("thru", &thru);
    return Submit(isolate, PendingKind::kNormal, 0,
                  [this, &module_name, &from_path,
                   &thru](xlang_bridge_request_id request_id) {
                    return api_.import_module(
                        request_id, BorrowedBytes(module_name),
                        BorrowedBytes(from_path), BorrowedBytes(thru));
                  });
  }

  v8::Local<v8::Promise> Get(v8::Isolate* isolate,
                             v8::Local<v8::Value> object_value,
                             const std::string& member_name) {
    xlang_bridge_handle object = 0;
    if (!ReadBigInt(object_value, &object)) {
      return RejectedPromise(isolate, "XLang object handle must be a BigInt");
    }
    return Submit(
        isolate, PendingKind::kNormal, 0,
        [this, object, &member_name](xlang_bridge_request_id request_id) {
          return api_.get_member(request_id, object,
                                 BorrowedBytes(member_name));
        });
  }

  v8::Local<v8::Promise> Set(v8::Isolate* isolate,
                             v8::Local<v8::Value> object_value,
                             const std::string& member_name,
                             v8::Local<v8::Value> input) {
    xlang_bridge_handle object = 0;
    if (!ReadBigInt(object_value, &object)) {
      return RejectedPromise(isolate, "XLang object handle must be a BigInt");
    }
    TaggedValueStorage storage;
    std::string error;
    if (!ReadTaggedValue(isolate, input, &storage, &error))
      return RejectedPromise(isolate, error);
    storage.RefreshView();
    return Submit(isolate, PendingKind::kNormal, 0,
                  [this, object, &member_name,
                   &storage](xlang_bridge_request_id request_id) {
                    return api_.set_member(request_id, object,
                                           BorrowedBytes(member_name),
                                           &storage.native);
                  });
  }

  v8::Local<v8::Promise> Call(v8::Isolate* isolate,
                              v8::Local<v8::Value> callable_value,
                              v8::Local<v8::Value> args_value,
                              v8::Local<v8::Value> kwargs_value) {
    xlang_bridge_handle callable = 0;
    if (!ReadBigInt(callable_value, &callable)) {
      return RejectedPromise(isolate, "XLang callable handle must be a BigInt");
    }

    std::vector<TaggedValueStorage> arg_storage;
    std::vector<xlang_bridge_value> args;
    std::vector<NamedValueStorage> kwarg_storage;
    std::vector<xlang_bridge_named_value> kwargs;
    std::string error;
    if (!ReadTaggedArray(isolate, args_value, &arg_storage, &args, &error) ||
        !ReadTaggedKwargs(isolate, kwargs_value, &kwarg_storage, &kwargs,
                          &error)) {
      return RejectedPromise(isolate, error);
    }

    return Submit(
        isolate, PendingKind::kNormal, 0,
        [this, callable, &args, &kwargs](xlang_bridge_request_id request_id) {
          return api_.call(request_id, callable, args.data(),
                           static_cast<uint32_t>(args.size()), kwargs.data(),
                           static_cast<uint32_t>(kwargs.size()));
        });
  }

  v8::Local<v8::Promise> CallMember(v8::Isolate* isolate,
                                    v8::Local<v8::Value> object_value,
                                    const std::string& member_name,
                                    v8::Local<v8::Value> args_value,
                                    v8::Local<v8::Value> kwargs_value) {
    xlang_bridge_handle object = 0;
    if (!ReadBigInt(object_value, &object)) {
      return RejectedPromise(isolate, "XLang object handle must be a BigInt");
    }

    std::vector<TaggedValueStorage> arg_storage;
    std::vector<xlang_bridge_value> args;
    std::vector<NamedValueStorage> kwarg_storage;
    std::vector<xlang_bridge_named_value> kwargs;
    std::string error;
    if (!ReadTaggedArray(isolate, args_value, &arg_storage, &args, &error) ||
        !ReadTaggedKwargs(isolate, kwargs_value, &kwarg_storage, &kwargs,
                          &error)) {
      return RejectedPromise(isolate, error);
    }

    return Submit(isolate, PendingKind::kNormal, 0,
                  [this, object, &member_name, &args,
                   &kwargs](xlang_bridge_request_id request_id) {
                    return api_.call_member(
                        request_id, object, BorrowedBytes(member_name),
                        args.data(), static_cast<uint32_t>(args.size()),
                        kwargs.data(), static_cast<uint32_t>(kwargs.size()));
                  });
  }

  v8::Local<v8::Promise> Release(v8::Isolate* isolate,
                                 v8::Local<v8::Value> handle_value) {
    gin_helper::Promise<v8::Local<v8::Value>> promise(isolate);
    auto result = promise.GetHandle();
    xlang_bridge_handle handle = 0;
    if (!ReadBigInt(handle_value, &handle)) {
      promise.RejectWithErrorMessage("XLang object handle must be a BigInt");
      return result;
    }
    if (!IsLoaded()) {
      promise.RejectWithErrorMessage("The XLang bridge is not loaded");
      return result;
    }
    const xlang_bridge_status status = api_.release_handle(handle);
    if (status == XLANG_BRIDGE_STATUS_OK) {
      promise.Resolve(v8::Undefined(isolate));
    } else {
      promise.RejectWithErrorMessage(StatusMessage(status));
    }
    return result;
  }

  v8::Local<v8::Promise> On(v8::Isolate* isolate,
                            v8::Local<v8::Value> object_value,
                            const std::string& event_name,
                            v8::Local<v8::Function> callback) {
    xlang_bridge_handle object = 0;
    if (!ReadBigInt(object_value, &object)) {
      return RejectedPromise(isolate, "XLang object handle must be a BigInt");
    }

    const xlang_bridge_subscription_id subscription_id = NextSubscriptionId();
    event_handlers_.try_emplace(subscription_id, isolate, callback);
    return Submit(isolate, PendingKind::kEventOn, subscription_id,
                  [this, object, &event_name,
                   subscription_id](xlang_bridge_request_id request_id) {
                    return api_.event_on(request_id, object,
                                         BorrowedBytes(event_name),
                                         subscription_id);
                  });
  }

  v8::Local<v8::Promise> Off(v8::Isolate* isolate,
                             v8::Local<v8::Value> subscription_value) {
    xlang_bridge_subscription_id subscription_id = 0;
    if (!ReadBigInt(subscription_value, &subscription_id)) {
      return RejectedPromise(isolate, "XLang subscription id must be a BigInt");
    }
    if (!event_handlers_.contains(subscription_id)) {
      return RejectedPromise(isolate, "Unknown XLang event subscription");
    }
    return Submit(isolate, PendingKind::kEventOff, subscription_id,
                  [this, subscription_id](xlang_bridge_request_id request_id) {
                    return api_.event_off(request_id, subscription_id);
                  });
  }

 private:
  XLangBridgeBinding()
      : callback_state_(base::MakeRefCounted<CallbackState>(
            this,
            base::SequencedTaskRunner::GetCurrentDefault())) {
    host_callbacks_.struct_size = sizeof(host_callbacks_);
    host_callbacks_.abi_version = XLANG_BRIDGE_ABI_VERSION;
    host_callbacks_.user_data = callback_state_.get();
    host_callbacks_.completion = &CompletionCallback;
    host_callbacks_.event = &EventCallback;
    host_callbacks_.log = &LogCallback;
  }

  template <typename Submitter>
  v8::Local<v8::Promise> Submit(v8::Isolate* isolate,
                                PendingKind kind,
                                xlang_bridge_subscription_id subscription_id,
                                Submitter&& submitter) {
    gin_helper::Promise<v8::Local<v8::Value>> promise(isolate);
    auto result = promise.GetHandle();
    if (!IsLoaded()) {
      promise.RejectWithErrorMessage("The XLang bridge is not loaded");
      UndoPendingState(kind, subscription_id);
      return result;
    }
    if (shutting_down_ && kind != PendingKind::kShutdown) {
      promise.RejectWithErrorMessage("The XLang bridge is shutting down");
      UndoPendingState(kind, subscription_id);
      return result;
    }

    const xlang_bridge_request_id request_id = NextRequestId();
    auto [iterator, inserted] = pending_.try_emplace(
        request_id, std::move(promise), kind, subscription_id);
    DCHECK(inserted);
    const xlang_bridge_status status = submitter(request_id);
    if (status == XLANG_BRIDGE_STATUS_OK && kind == PendingKind::kInitialize) {
      // An accepted initialize starts (or reuses) the bridge worker. That
      // worker remains owned by the loaded module even when initialization
      // later completes with an error, so process teardown must still ask the
      // bridge to stop before allowing the module to unload.
      shutdown_required_ = true;
    }
    if (status != XLANG_BRIDGE_STATUS_OK) {
      auto rejected = std::move(iterator->second.promise);
      pending_.erase(iterator);
      UndoPendingState(kind, subscription_id);
      rejected.RejectWithErrorMessage(StatusMessage(status));
    }
    return result;
  }

  v8::Local<v8::Promise> RejectedPromise(v8::Isolate* isolate,
                                         std::string_view message) {
    gin_helper::Promise<v8::Local<v8::Value>> promise(isolate);
    auto result = promise.GetHandle();
    promise.RejectWithErrorMessage(message);
    return result;
  }

  void UndoPendingState(PendingKind kind,
                        xlang_bridge_subscription_id subscription_id) {
    if (kind == PendingKind::kInitialize)
      initializing_ = false;
    if (kind == PendingKind::kShutdown)
      shutting_down_ = false;
    if (kind == PendingKind::kEventOn)
      RemoveEventHandler(subscription_id);
  }

  xlang_bridge_request_id NextRequestId() {
    do {
      ++last_request_id_;
    } while (last_request_id_ == 0 || pending_.contains(last_request_id_));
    return last_request_id_;
  }

  xlang_bridge_subscription_id NextSubscriptionId() {
    do {
      ++last_subscription_id_;
    } while (last_subscription_id_ == 0 ||
             event_handlers_.contains(last_subscription_id_));
    return last_subscription_id_;
  }

  static void CompletionCallback(void* user_data,
                                 xlang_bridge_request_id request_id,
                                 xlang_bridge_status status,
                                 const xlang_bridge_value* value,
                                 xlang_bridge_bytes_view error_message) {
    scoped_refptr<CallbackState> state(static_cast<CallbackState*>(user_data));
    if (!state->owner.load(std::memory_order_acquire))
      return;
    OwnedBridgeValue value_copy = CopyBridgeValue(value);
    std::string error_copy =
        CopyString(error_message).value_or("invalid bridge error message");
    state->task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(&DispatchCompletion, std::move(state), request_id,
                       status, std::move(value_copy), std::move(error_copy)));
  }

  static void EventCallback(void* user_data,
                            xlang_bridge_subscription_id subscription_id,
                            const xlang_bridge_value* args,
                            uint32_t arg_count,
                            const xlang_bridge_named_value* kwargs,
                            uint32_t kwarg_count) {
    scoped_refptr<CallbackState> state(static_cast<CallbackState*>(user_data));
    if (!state->owner.load(std::memory_order_acquire))
      return;

    std::vector<OwnedBridgeValue> args_copy;
    args_copy.reserve(arg_count);
    for (uint32_t i = 0; args && i < arg_count; ++i)
      args_copy.push_back(CopyBridgeValue(&args[i]));

    std::vector<OwnedNamedValue> kwargs_copy;
    kwargs_copy.reserve(kwarg_count);
    for (uint32_t i = 0; kwargs && i < kwarg_count; ++i) {
      kwargs_copy.push_back(
          {.name = CopyString(kwargs[i].name).value_or(std::string()),
           .value = CopyBridgeValue(&kwargs[i].value)});
    }

    state->task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(&DispatchEvent, std::move(state), subscription_id,
                       std::move(args_copy), std::move(kwargs_copy)));
  }

  static void LogCallback(void* user_data,
                          int32_t level,
                          xlang_bridge_bytes_view message) {
    scoped_refptr<CallbackState> state(static_cast<CallbackState*>(user_data));
    if (!state->owner.load(std::memory_order_acquire))
      return;
    state->task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(&DispatchLog, std::move(state), level,
                       CopyString(message).value_or("invalid log message")));
  }

  static void DispatchCompletion(scoped_refptr<CallbackState> state,
                                 xlang_bridge_request_id request_id,
                                 xlang_bridge_status status,
                                 OwnedBridgeValue value,
                                 std::string error_message) {
    if (auto* owner = state->owner.load(std::memory_order_acquire)) {
      owner->OnCompletion(request_id, status, std::move(value),
                          std::move(error_message));
    }
  }

  static void DispatchEvent(scoped_refptr<CallbackState> state,
                            xlang_bridge_subscription_id subscription_id,
                            std::vector<OwnedBridgeValue> args,
                            std::vector<OwnedNamedValue> kwargs) {
    if (auto* owner = state->owner.load(std::memory_order_acquire)) {
      owner->OnEvent(subscription_id, std::move(args), std::move(kwargs));
    }
  }

  static void DispatchQueuedEvents(
      scoped_refptr<CallbackState> state,
      xlang_bridge_subscription_id subscription_id) {
    if (auto* owner = state->owner.load(std::memory_order_acquire))
      owner->FlushQueuedEvents(subscription_id);
  }

  static void DispatchLog(scoped_refptr<CallbackState> state,
                          int32_t level,
                          std::string message) {
    if (state->owner.load(std::memory_order_acquire))
      LOG(INFO) << "[XLang bridge " << level << "] " << message;
  }

  void OnCompletion(xlang_bridge_request_id request_id,
                    xlang_bridge_status status,
                    OwnedBridgeValue value,
                    std::string error_message) {
    auto iterator = pending_.find(request_id);
    if (iterator == pending_.end()) {
      ReleaseIfHandle(value);
      return;
    }

    PendingRequest request = std::move(iterator->second);
    pending_.erase(iterator);
    v8::Isolate* isolate = JavascriptEnvironment::GetIsolate();
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = request.promise.GetContext();
    v8::Context::Scope context_scope(context);

    if (request.kind != PendingKind::kShutdown && shutting_down_) {
      UndoPendingState(request.kind, request.subscription_id);
      request.promise.RejectWithErrorMessage(
          "XLang is shutting down; the request result was discarded");
      ReleaseIfHandle(value);
      return;
    }

    // A shutdown completion is a terminal lifecycle boundary even when the
    // runtime reports an error while tearing down. The bridge delivers this
    // callback only after its worker has exited and has transitioned to the
    // stopped state, so keeping |initialized_| set would leave stale handlers
    // reachable from JS and make later shutdown calls inconsistent.
    if (request.kind == PendingKind::kShutdown)
      FinalizeShutdown();

    if (status != XLANG_BRIDGE_STATUS_OK) {
      UndoPendingState(request.kind, request.subscription_id);
      request.promise.RejectWithErrorMessage(
          StatusMessage(status, error_message));
      ReleaseIfHandle(value);
      return;
    }

    if (request.kind == PendingKind::kInitialize) {
      initializing_ = false;
      initialized_ = true;
    } else if (request.kind == PendingKind::kEventOff) {
      RemoveEventHandler(request.subscription_id);
    }

    if (request.kind == PendingKind::kEventOn) {
      request.promise.Resolve(
          v8::BigInt::NewFromUnsigned(isolate, request.subscription_id));
      // Promise reactions run at the microtask checkpoint at the end of this
      // task. Delivering buffered events in a following task guarantees that
      // the JS facade has recorded the resolved subscription id first.
      callback_state_->task_runner->PostTask(
          FROM_HERE, base::BindOnce(&DispatchQueuedEvents, callback_state_,
                                    request.subscription_id));
    } else if (request.kind == PendingKind::kInitialize ||
               request.kind == PendingKind::kEventOff ||
               request.kind == PendingKind::kShutdown) {
      request.promise.Resolve(v8::Undefined(isolate));
    } else {
      request.promise.Resolve(ToV8(isolate, value));
    }
  }

  void OnEvent(xlang_bridge_subscription_id subscription_id,
               std::vector<OwnedBridgeValue> args,
               std::vector<OwnedNamedValue> kwargs) {
    if (shutting_down_) {
      ReleaseHandles(args);
      for (const auto& item : kwargs)
        ReleaseIfHandle(item.value);
      return;
    }

    auto iterator = event_handlers_.find(subscription_id);
    if (iterator == event_handlers_.end()) {
      ReleaseHandles(args);
      for (const auto& item : kwargs)
        ReleaseIfHandle(item.value);
      return;
    }

    if (!iterator->second.active) {
      iterator->second.queued_events.push_back(
          {.args = std::move(args), .kwargs = std::move(kwargs)});
      return;
    }

    v8::Isolate* isolate = JavascriptEnvironment::GetIsolate();
    v8::HandleScope handle_scope(isolate);
    DeliverEvent(isolate, iterator->second.callback.Get(isolate),
                 std::move(args), std::move(kwargs));
  }

  void FlushQueuedEvents(xlang_bridge_subscription_id subscription_id) {
    auto iterator = event_handlers_.find(subscription_id);
    if (iterator == event_handlers_.end())
      return;

    iterator->second.active = true;
    std::vector<QueuedEvent> queued_events =
        std::move(iterator->second.queued_events);
    v8::Isolate* isolate = JavascriptEnvironment::GetIsolate();
    v8::HandleScope handle_scope(isolate);
    for (size_t i = 0; i < queued_events.size(); ++i) {
      iterator = event_handlers_.find(subscription_id);
      if (iterator == event_handlers_.end()) {
        for (; i < queued_events.size(); ++i)
          ReleaseEvent(queued_events[i]);
        return;
      }
      DeliverEvent(isolate, iterator->second.callback.Get(isolate),
                   std::move(queued_events[i].args),
                   std::move(queued_events[i].kwargs));
    }
  }

  void DeliverEvent(v8::Isolate* isolate,
                    v8::Local<v8::Function> callback,
                    std::vector<OwnedBridgeValue> args,
                    std::vector<OwnedNamedValue> kwargs) {
    v8::Local<v8::Context> context = callback->GetCreationContextChecked();
    v8::Context::Scope context_scope(context);

    v8::Local<v8::Array> js_args =
        v8::Array::New(isolate, static_cast<int>(args.size()));
    for (size_t i = 0; i < args.size(); ++i) {
      js_args->Set(context, static_cast<uint32_t>(i), ToV8(isolate, args[i]))
          .Check();
    }
    auto js_kwargs = gin_helper::Dictionary::CreateEmpty(isolate);
    for (const auto& item : kwargs) {
      // Define an own data property so an XLang kwarg named "__proto__" cannot
      // invoke Object.prototype's legacy setter and alter the event envelope.
      js_kwargs.GetHandle()
          ->DefineOwnProperty(context, gin::StringToV8(isolate, item.name),
                              ToV8(isolate, item.value), v8::None)
          .Check();
    }

    v8::Local<v8::Value> callback_args[] = {js_args, js_kwargs.GetHandle()};
    v8::TryCatch try_catch(isolate);
    callback->Call(context, v8::Undefined(isolate), 2, callback_args).IsEmpty();
    if (try_catch.HasCaught())
      node::errors::TriggerUncaughtException(isolate, try_catch);
  }

  v8::Local<v8::Value> ToV8(v8::Isolate* isolate,
                            const OwnedBridgeValue& value) {
    auto result = gin_helper::Dictionary::CreateEmpty(isolate);
    switch (value.type) {
      case XLANG_BRIDGE_VALUE_NULL:
        result.Set("type", "null");
        break;
      case XLANG_BRIDGE_VALUE_BOOL:
        result.Set("type", "boolean");
        result.Set("value", value.boolean_value != 0);
        break;
      case XLANG_BRIDGE_VALUE_INT64:
        result.Set("type", "int64");
        result.Set("value", v8::BigInt::New(isolate, value.int64_value));
        break;
      case XLANG_BRIDGE_VALUE_DOUBLE:
        result.Set("type", "double");
        result.Set("value", value.double_value);
        break;
      case XLANG_BRIDGE_VALUE_STRING:
        result.Set("type", "string");
        result.Set("value",
                   gin::StringToV8(
                       isolate, std::string(value.bytes.empty()
                                                ? ""
                                                : reinterpret_cast<const char*>(
                                                      value.bytes.data()),
                                            value.bytes.size())));
        break;
      case XLANG_BRIDGE_VALUE_BINARY:
        result.Set("type", "binary");
        result.Set("value",
                   electron::Buffer::Copy(
                       isolate,
                       value.bytes.empty()
                           ? ""
                           : reinterpret_cast<const char*>(value.bytes.data()),
                       value.bytes.size())
                       .ToLocalChecked());
        break;
      case XLANG_BRIDGE_VALUE_HANDLE:
        result.Set("type", "handle");
        result.Set("value",
                   v8::BigInt::NewFromUnsigned(isolate, value.handle_value));
        result.Set("objectType", value.flags);
        break;
      case XLANG_BRIDGE_VALUE_SUBSCRIPTION:
        result.Set("type", "subscription");
        result.Set("value",
                   v8::BigInt::NewFromUnsigned(isolate, value.handle_value));
        break;
      case XLANG_BRIDGE_VALUE_UNDEFINED:
      default:
        result.Set("type", "undefined");
        break;
    }
    return result.GetHandle();
  }

  void RejectOutstandingAfterShutdown() {
    for (auto& entry : pending_) {
      PendingRequest& request = entry.second;
      UndoPendingState(request.kind, request.subscription_id);
      request.promise.RejectWithErrorMessage(
          "XLang shut down before the request completed");
    }
    pending_.clear();
  }

  void FinalizeShutdown() {
    shutdown_required_ = false;
    shutting_down_ = false;
    initialized_ = false;
    initializing_ = false;
    RejectOutstandingAfterShutdown();
    ClearEventHandlers();
  }

  void ReleaseIfHandle(const OwnedBridgeValue& value) {
    if (value.type == XLANG_BRIDGE_VALUE_HANDLE && api_.release_handle)
      api_.release_handle(value.handle_value);
  }

  void ReleaseHandles(const std::vector<OwnedBridgeValue>& values) {
    for (const auto& value : values)
      ReleaseIfHandle(value);
  }

  void ReleaseEvent(const QueuedEvent& event) {
    ReleaseHandles(event.args);
    for (const auto& item : event.kwargs)
      ReleaseIfHandle(item.value);
  }

  void RemoveEventHandler(xlang_bridge_subscription_id subscription_id) {
    auto iterator = event_handlers_.find(subscription_id);
    if (iterator == event_handlers_.end())
      return;
    for (const auto& event : iterator->second.queued_events)
      ReleaseEvent(event);
    iterator->second.callback.Reset();
    event_handlers_.erase(iterator);
  }

  void ClearEventHandlers() {
    for (auto& entry : event_handlers_) {
      for (const auto& event : entry.second.queued_events)
        ReleaseEvent(event);
      entry.second.callback.Reset();
    }
    event_handlers_.clear();
  }

  scoped_refptr<CallbackState> callback_state_;
  xlang_bridge_host_callbacks host_callbacks_ = {};
  base::ScopedNativeLibrary library_;
  base::FilePath library_path_;
  xlang_bridge_api api_ = {};
  std::unordered_map<xlang_bridge_request_id, PendingRequest> pending_;
  std::unordered_map<xlang_bridge_subscription_id, EventHandler>
      event_handlers_;
  xlang_bridge_request_id last_request_id_ = 0;
  xlang_bridge_subscription_id last_subscription_id_ = 0;
  bool initializing_ = false;
  bool initialized_ = false;
  bool shutting_down_ = false;
  bool shutdown_required_ = false;
};

}  // namespace

void InitializeXLang(v8::Local<v8::Object> exports) {
  v8::Isolate* isolate = JavascriptEnvironment::GetIsolate();
  auto binding = gin_helper::Dictionary::CreateEmpty(isolate);
  XLangBridgeBinding* xlang = XLangBridgeBinding::GetInstance();
  binding.SetMethod("load", base::BindRepeating(&XLangBridgeBinding::Load,
                                                base::Unretained(xlang)));
  binding.SetMethod("isLoaded",
                    base::BindRepeating(&XLangBridgeBinding::IsLoaded,
                                        base::Unretained(xlang)));
  binding.SetMethod("getBridgeInfo",
                    base::BindRepeating(&XLangBridgeBinding::GetBridgeInfo,
                                        base::Unretained(xlang)));
  binding.SetMethod("initialize",
                    base::BindRepeating(&XLangBridgeBinding::Initialize,
                                        base::Unretained(xlang)));
  binding.SetMethod("shutdown",
                    base::BindRepeating(&XLangBridgeBinding::Shutdown,
                                        base::Unretained(xlang)));
  binding.SetMethod("importModule",
                    base::BindRepeating(&XLangBridgeBinding::ImportModule,
                                        base::Unretained(xlang)));
  binding.SetMethod("get", base::BindRepeating(&XLangBridgeBinding::Get,
                                               base::Unretained(xlang)));
  binding.SetMethod("set", base::BindRepeating(&XLangBridgeBinding::Set,
                                               base::Unretained(xlang)));
  binding.SetMethod("invoke", base::BindRepeating(&XLangBridgeBinding::Call,
                                                  base::Unretained(xlang)));
  binding.SetMethod("call", base::BindRepeating(&XLangBridgeBinding::CallMember,
                                                base::Unretained(xlang)));
  binding.SetMethod("callMember",
                    base::BindRepeating(&XLangBridgeBinding::CallMember,
                                        base::Unretained(xlang)));
  binding.SetMethod("release", base::BindRepeating(&XLangBridgeBinding::Release,
                                                   base::Unretained(xlang)));
  binding.SetMethod("on", base::BindRepeating(&XLangBridgeBinding::On,
                                              base::Unretained(xlang)));
  binding.SetMethod("off", base::BindRepeating(&XLangBridgeBinding::Off,
                                               base::Unretained(xlang)));

  gin_helper::Dictionary dictionary(isolate, exports);
  dictionary.Set("xlang", binding);
}

}  // namespace electron::api

namespace {

void Initialize(v8::Local<v8::Object> exports,
                v8::Local<v8::Value> unused,
                v8::Local<v8::Context> context,
                void* priv) {
  electron::api::InitializeXLang(exports);
}

}  // namespace

NODE_LINKED_BINDING_CONTEXT_AWARE(electron_browser_xlang, Initialize)
