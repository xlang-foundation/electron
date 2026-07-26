/*
 * Copyright (c) 2026 XLang Foundation
 * SPDX-License-Identifier: MIT
 */

#include "xhost.h"
#include "xpackage.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#define XLANG_EVENT_TEST_EXPORT __declspec(dllexport)
#define XLANG_EVENT_TEST_LIBRARY_NAME "xlang_bridge_event_test"
#else
#define XLANG_EVENT_TEST_EXPORT __attribute__((visibility("default")))
#define XLANG_EVENT_TEST_LIBRARY_NAME "libxlang_bridge_event_test"
#endif

namespace X {

XHost* g_pXHost = nullptr;

class BridgeEventTestModule {
 public:
  BEGIN_PACKAGE(BridgeEventTestModule)
  APISET().AddEvent("changed");
  APISET().AddFunc<2>("emit", &BridgeEventTestModule::Emit);
  APISET().AddFunc<1>("echo", &BridgeEventTestModule::Echo);
  APISET().AddFunc<0>("emit_blocked", &BridgeEventTestModule::EmitBlocked);
  APISET().AddFunc<1>("wait_until_blocked",
                      &BridgeEventTestModule::WaitUntilBlocked);
  END_PACKAGE

 public:
  ~BridgeEventTestModule() { JoinBlockedEmission(); }

  bool Emit(int number, std::string message) {
    X::ARGS args(2);
    args.push_back(X::Value(number));
    args.push_back(X::Value(message));

    X::KWARGS kwargs(1);
    X::Value source(std::string("native-test-module"));
    kwargs.Add("source", source);

    Fire(0, args, kwargs);
    return true;
  }

  X::Value Echo(X::Value value) { return value; }

  void InstallBlockingHandler() {
    X::XEvent* event = GetEvent(0);
    if (event == nullptr) {
      return;
    }
    event->AddHandler([this](X::XRuntime*, X::XObj*, X::ARGS&, X::KWARGS&,
                             X::Value& ret_value) {
      {
        std::lock_guard<std::mutex> lock(block_mutex_);
        blocked_ = true;
      }
      blocked_ready_.notify_all();
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      ret_value = X::Value(true);
    });
    event->DecRef();
  }

  bool EmitBlocked() {
    JoinBlockedEmission();
    {
      std::lock_guard<std::mutex> lock(block_mutex_);
      blocked_ = false;
    }
    blocked_emission_ = std::thread([this]() {
      X::ARGS args;
      X::KWARGS kwargs;
      Fire(0, args, kwargs);
    });
    return true;
  }

  bool WaitUntilBlocked(int timeout_milliseconds) {
    std::unique_lock<std::mutex> lock(block_mutex_);
    return blocked_ready_.wait_for(
        lock, std::chrono::milliseconds(timeout_milliseconds),
        [this]() { return blocked_; });
  }

  void JoinBlockedEmission() {
    if (blocked_emission_.joinable()) {
      blocked_emission_.join();
    }
  }

 private:
  std::mutex block_mutex_;
  std::condition_variable blocked_ready_;
  bool blocked_ = false;
  std::thread blocked_emission_;
};

BridgeEventTestModule g_bridge_event_test_module;

}  // namespace X

extern "C" XLANG_EVENT_TEST_EXPORT void Load(void* host,
                                             X::Value current_module) {
  (void)current_module;
  X::g_pXHost = static_cast<X::XHost*>(host);
  X::RegisterPackage<X::BridgeEventTestModule>(XLANG_EVENT_TEST_LIBRARY_NAME,
                                               "bridge_event_test",
                                               &X::g_bridge_event_test_module);
  X::g_bridge_event_test_module.InstallBlockingHandler();
}

extern "C" XLANG_EVENT_TEST_EXPORT void Unload() {
  X::g_bridge_event_test_module.JoinBlockedEmission();
  X::g_pXHost = nullptr;
}
