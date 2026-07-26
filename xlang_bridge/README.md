# Electron XLang bridge

This directory builds the XLang runtime adapter loaded by Electron's native
`xlang` binding. It is intentionally a separate CMake target so Chromium's GN
toolchain never exchanges C++ objects with XLang.

The boundary in `include/xlang_bridge.h` is a versioned C ABI containing only
fixed-width values, borrowed byte views, callbacks, and opaque handles. The
bridge owns the XLang engine, runtime, worker queue, event loop, XLang values,
and LRPC objects.

## Build

```text
cmake -S xlang_bridge -B out/xlang_bridge -DXLANG_ROOT=/path/to/xlang
cmake --build out/xlang_bridge --config Release
```

When `XLANG_ROOT` is omitted, CMake searches common sibling-workspace layouts.
The output has no Unix `lib` prefix:

- `electron_xlang_bridge.dll`
- `electron_xlang_bridge.so`
- `electron_xlang_bridge.dylib`

Place the bridge and `xlang_eng` for the target platform under the packaged
application's `resources/xlang` directory. No bridge manifest is used.

## Smoke test

Building the smoke target also builds a test-only native module named
`xlang_bridge_event_test` beside the executable. The default invocation imports
that local module and verifies event subscribe, positional and named values,
unsubscribe, and suppression of events after unsubscribe:

```text
electron_xlang_bridge_smoke <runtime-directory>
```

Use `--probe` to additionally import any local XLang native module by path. An
optional member name calls that member without arguments after import:

```text
electron_xlang_bridge_smoke <runtime-directory> --probe <module-name> <from-path> [member]
```

For example, this follows the same pattern as Garnet's Python tests,
`xlang.importModule("garnet", fromPath=garnet_dll_path)`, and then calls its
no-op `runTest` member:

```text
electron_xlang_bridge_smoke <runtime-directory> --probe garnet <path-to-garnet.dll> runTest
```

The probe module's directory is added to XLang's native library search paths.
If it has dependencies in other directories, add those directories to the
platform loader path just as the Python examples use `os.add_dll_directory`.

## Thread and lifetime rules

- API calls copy all input views before returning.
- Completion callbacks run on the bridge worker, except shutdown completion,
  which runs after that worker exits.
- Event callbacks can run on an XLang or LRPC thread.
- Callback string/binary/array views are valid only until the callback returns.
- Returned object handles stay valid until `release_handle()` or shutdown.
- Electron must marshal every callback to its owning V8 sequence.
- A failed initialization can be retried. A completed shutdown is final for
  the process because imported XLang extension DLLs retain process-global
  engine state; a new Electron process is required for another initialization.

`import_module(module, from, thru)` maps directly to XLang import semantics.
For example, `thru = "lrpc:1000"` imports a remote process module.

Object events use the same operation for local and LRPC objects. `event_on`
creates an XLang callable and sends the `subscribe` operation; `event_off`
sends the native subscription cookie using `unsubscribe`.
