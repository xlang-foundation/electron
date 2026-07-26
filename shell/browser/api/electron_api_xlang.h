// Copyright (c) 2026 XLang Foundation
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef ELECTRON_SHELL_BROWSER_API_ELECTRON_API_XLANG_H_
#define ELECTRON_SHELL_BROWSER_API_ELECTRON_API_XLANG_H_

#include "v8/include/v8-forward.h"

namespace electron::api {

// Installs the main-process-only XLang bridge binding on |exports|.
void InitializeXLang(v8::Local<v8::Object> exports);

}  // namespace electron::api

#endif  // ELECTRON_SHELL_BROWSER_API_ELECTRON_API_XLANG_H_
