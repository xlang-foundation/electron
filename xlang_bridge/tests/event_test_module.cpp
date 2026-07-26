/*
 * Copyright (c) 2026 XLang Foundation
 * SPDX-License-Identifier: MIT
 */

#include "xhost.h"
#include "xpackage.h"

#include <string>

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
  END_PACKAGE

 public:
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
}

extern "C" XLANG_EVENT_TEST_EXPORT void Unload() {
  X::g_pXHost = nullptr;
}
