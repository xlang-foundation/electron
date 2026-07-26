# XLangInitializeOptions Object

* `libraryPath` string (optional) - Path passed to the XLang native library
  loader. When omitted, Electron checks the `ELECTRON_XLANG_LIBRARY_PATH`
  environment variable and then uses the `xlang` directory under
  `process.resourcesPath`.
* `appPath` string (optional) - Absolute XLang application/resources directory.
  Defaults to the directory containing the loaded XLang bridge.
* `librarySearchPaths` string[] (optional) - Additional module and native
  library search directories. `appPath` is searched first.
* `enablePython` boolean (optional) - Whether to enable XLang's Python
  integration. Default is `false`.
* `debug` boolean (optional) - Whether to enable XLang bridge debug logging.
  Default is `false`.
