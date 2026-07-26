# xlang

> Load XLang modules and call them from the Electron main process.

Process: [Main](../glossary.md#main-process)

The `xlang` module exposes XLang objects as asynchronous JavaScript proxies.
Importing a module lazily loads and initializes the XLang runtime, so the
common case does not require a separate initialization call:

```js
const { xlang } = require('electron/main')

xlang.importModule('yaml', {
  fromPath: 'xlang_yaml'
}).then(async (yaml) => {
  // XLang member names are dynamic, so this cast is only for type-checking.
  const dynamicYaml = /** @type {any} */ (yaml)
  const document = await dynamicYaml.loads('answer: 42')
  console.log(await document.get('answer'))

  await document.dispose()
  await yaml.dispose()
})
```

An unknown string property on an XLang proxy is treated as a remote method.
Every remote operation returns a `Promise`. Properties must be read and
written with `get` and `set`, because JavaScript property assignment cannot
wait for a remote operation.

The first call uses the XLang runtime under `process.resourcesPath/xlang` by
default. Set `ELECTRON_XLANG_LIBRARY_PATH` while developing or call
`xlang.initialize({ libraryPath })` before the first import to use another
location.

A native package such as Garnet uses the same local import pattern as XLang's
Python API. Passing `fromPath` without `thru` loads the module in the Electron
process; it does not require an LRPC server:

```js
const { xlang } = require('electron/main')

const garnet = await xlang.importModule('garnet', {
  fromPath: String.raw`C:\path\to\garnet.dll`
})

await garnet.runTest()
await garnet.dispose()
```

## XLang values

The initial bridge keeps XLang objects as opaque remote handles instead of
recursively copying containers:

* XLang null, Boolean, floating-point, string, and binary values become
  JavaScript `null`, `boolean`, `number`, `string`, and `Buffer` values.
* Signed 64-bit integers within JavaScript's safe integer range become
  `number` values. Larger integers become `bigint` values.
* XLang lists, dictionaries, modules, functions, and other objects remain
  [`XLangObject`](#class-xlangobject) proxies.

JavaScript arguments can be scalar values, `Buffer` values, signed 64-bit
`bigint` values, or XLang object proxies from the same runtime. Access
container contents through their XLang members and methods.

The `args` array passed to `call` or `invoke` is the call envelope. A nested
JavaScript array is not converted to an XLang list in this initial bridge.

## Methods

The `xlang` module has the following methods:

### `xlang.initialize([options])`

* `options` [XLangInitializeOptions](structures/xlang-initialize-options.md)
  (optional)

Returns `Promise<void>` - Resolves when the XLang native library and runtime
are ready.

Calling this method is optional. `xlang.importModule` initializes the runtime
with default options on first use. Concurrent calls share the same
initialization.

### `xlang.importModule(name[, options])`

* `name` string - XLang module name.
* `options` [XLangImportOptions](structures/xlang-import-options.md) (optional)

Returns `Promise<XLangObject>` - Resolves with the imported module's
asynchronous proxy.

The returned proxy is intentionally not a JavaScript thenable. If an XLang
module defines a member named `then`, invoke it explicitly with
`module.call('then', args)`.

### `xlang.shutdown()`

Returns `Promise<void>` - Resolves after the XLang runtime shuts down.

All existing [`XLangObject`](#class-xlangobject) proxies become disposed.
Shutdown is final for the current Electron process. XLang extension modules
currently retain process-global engine state, so start a new Electron process
to initialize XLang again. An initialization attempt that fails before a
runtime starts can be corrected and retried without calling `shutdown`.

## Class: XLangObject

> An asynchronous proxy for an object owned by the XLang runtime.

Process: [Main](../glossary.md#main-process)

`XLangObject` instances are created by `xlang.importModule`, by remote method
results, and by event arguments. They cannot be constructed directly.

Remote method names that collide with the explicit methods below can still be
invoked through `object.call(name, args)`.

### Instance Methods

#### `object.get(name)`

* `name` string - Member name.

Returns `Promise<any>` - Resolves with the member value.

#### `object.set(name, value)`

* `name` string - Member name.
* `value` any - New member value.

Returns `Promise<void>` - Resolves after the member is updated.

Direct assignment such as `object.name = value` throws. Use this method so
the asynchronous update can be awaited.

#### `object.invoke([args][, options])`

* `args` any[] (optional) - Positional arguments.
* `options` [XLangCallOptions](structures/xlang-call-options.md) (optional)

Returns `Promise<any>` - Resolves with the result of invoking this XLang
object itself.

Use `invoke` for a callable handle returned by `get`, a method result, or an
event. Use `call` to invoke a named member on an object.

#### `object.call(name[, args][, options])`

* `name` string - Method name.
* `args` any[] (optional) - Positional arguments.
* `options` [XLangCallOptions](structures/xlang-call-options.md) (optional)

Returns `Promise<any>` - Resolves with the method result.

This is the explicit form of a natural proxy call. For example,
`await object.call('render', [input])` and `await object.render(input)` perform
the same XLang call.

Natural proxy calls accept positional arguments. Use the explicit form to pass
named arguments:

```js
const { xlang } = require('electron/main')

xlang.importModule('renderer').then(async (renderer) => {
  await renderer.call('render', ['input'], {
    kwargs: { quality: 'high' }
  })
  await renderer.dispose()
})
```

#### `object.on(eventName, listener)`

* `eventName` string - XLang event name.
* `listener` Function
  * `event` [XLangEvent](structures/xlang-event.md)

Returns `Promise<void>` - Resolves after the event listener is registered.

Await registration before relying on the listener or calling `object.off`.
Positional and named XLang event values are kept separate:

```js
const { xlang } = require('electron/main')

xlang.importModule('events').then(async (events) => {
  const listener = ({ args, kwargs }) => {
    console.log(args, kwargs)
  }

  await events.on('changed', listener)
  // ...
  await events.off('changed', listener)
  await events.dispose()
})
```

An `XLangObject` delivered in `event.args` or `event.kwargs` owns a native
handle and should be disposed when it is no longer needed. The facade releases
event handles automatically when an event is canceled before delivery.

#### `object.off(eventName[, listener])`

* `eventName` string - XLang event name.
* `listener` Function (optional) - Previously registered listener. When
  omitted, all listeners registered through this proxy for `eventName` are
  removed.
  * `event` [XLangEvent](structures/xlang-event.md)

Returns `Promise<void>` - Resolves after matching native event subscriptions
are removed.

#### `object.dispose()`

Returns `Promise<void>` - Resolves after event subscriptions and the native
XLang handle are released.

Disposal is idempotent. Other operations reject after disposal.
