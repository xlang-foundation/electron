export type XLangTaggedValue =
  | { type: 'undefined' }
  | { type: 'null' }
  | { type: 'boolean'; value: boolean }
  | { type: 'int64'; value: bigint }
  | { type: 'double'; value: number }
  | { type: 'string'; value: string }
  | { type: 'binary'; value: Buffer }
  | { type: 'handle'; value: bigint; objectType: number };

type XLangNativeEventCallback = (
  args: XLangTaggedValue[],
  kwargs: Record<string, XLangTaggedValue>
) => void;

export interface XLangNativeBinding {
  load(libraryPath?: string): unknown;
  isLoaded(): boolean;
  initialize(options: Record<string, unknown>): Promise<void>;
  shutdown(): Promise<void>;
  importModule(name: string, options: XLangImportOptions): Promise<XLangTaggedValue>;
  get(handle: bigint, name: string): Promise<XLangTaggedValue>;
  set(handle: bigint, name: string, value: XLangTaggedValue): Promise<XLangTaggedValue | void>;
  invoke(
    handle: bigint,
    args: XLangTaggedValue[],
    kwargs: Record<string, XLangTaggedValue>
  ): Promise<XLangTaggedValue>;
  call(
    handle: bigint,
    name: string,
    args: XLangTaggedValue[],
    kwargs: Record<string, XLangTaggedValue>
  ): Promise<XLangTaggedValue>;
  release(handle: bigint): Promise<void>;
  on(handle: bigint, eventName: string, callback: XLangNativeEventCallback): Promise<bigint>;
  off(token: bigint): Promise<void>;
}

export interface XLangInitializeOptions extends Record<string, unknown> {
  libraryPath?: string;
  appPath?: string;
  librarySearchPaths?: string[];
  enablePython?: boolean;
  debug?: boolean;
}

export interface XLangImportOptions {
  fromPath?: string;
  thru?: string;
}

export interface XLangCallOptions {
  kwargs?: Record<string, unknown>;
}

export interface XLangEvent {
  args: unknown[];
  kwargs: Record<string, unknown>;
}

export type XLangListener = (event: XLangEvent) => void;

export interface XLangObjectMethods {
  get(name: string): Promise<unknown>;
  set(name: string, value: unknown): Promise<void>;
  invoke(args?: unknown[], options?: XLangCallOptions): Promise<unknown>;
  call(name: string, args?: unknown[], options?: XLangCallOptions): Promise<unknown>;
  on(eventName: string, listener: XLangListener): Promise<void>;
  off(eventName: string, listener?: XLangListener): Promise<void>;
  dispose(): Promise<void>;
}

export type XLangObject = XLangObjectMethods;

export interface XLangFacade {
  initialize(options?: XLangInitializeOptions): Promise<void>;
  importModule(name: string, options?: XLangImportOptions): Promise<XLangObject>;
  shutdown(): Promise<void>;
}

export interface XLangFacadeConfiguration {
  getDefaultLibraryPath?(): string | undefined;
}

const int64Minimum = -(1n << 63n);
const int64Maximum = (1n << 63n) - 1n;
const minimumSafeInteger = BigInt(Number.MIN_SAFE_INTEGER);
const maximumSafeInteger = BigInt(Number.MAX_SAFE_INTEGER);
const inspectSymbol = Symbol.for('nodejs.util.inspect.custom');
const explicitProxyMethods = new Set(['get', 'set', 'invoke', 'call', 'on', 'off', 'dispose']);

const remoteCoreByObject = new WeakMap<object, XLangRemoteObjectCore>();

function validateName(value: unknown, label: string): asserts value is string {
  if (typeof value !== 'string' || value.length === 0) {
    throw new TypeError(`${label} must be a non-empty string`);
  }
}

const validateImportOptions = (options: XLangImportOptions): void => {
  if (options === null || typeof options !== 'object' || Array.isArray(options)) {
    throw new TypeError('options must be an object');
  }
  if (options.fromPath !== undefined && typeof options.fromPath !== 'string') {
    throw new TypeError('options.fromPath must be a string');
  }
  if (options.thru !== undefined && typeof options.thru !== 'string') {
    throw new TypeError('options.thru must be a string');
  }
};

class XLangContext {
  private readonly objects = new Map<bigint, XLangRemoteObjectCore>();

  constructor(readonly native: XLangNativeBinding) {}

  encode(value: unknown): XLangTaggedValue {
    if (value === undefined) return { type: 'undefined' };
    if (value === null) return { type: 'null' };

    if (typeof value === 'boolean') {
      return { type: 'boolean', value };
    }

    if (typeof value === 'bigint') {
      if (value < int64Minimum || value > int64Maximum) {
        throw new RangeError('XLang bigint values must fit in a signed 64-bit integer');
      }
      return { type: 'int64', value };
    }

    if (typeof value === 'number') {
      if (Number.isInteger(value) && !Object.is(value, -0)) {
        if (!Number.isSafeInteger(value)) {
          throw new RangeError('Unsafe integer arguments must be passed as bigint');
        }
        return { type: 'int64', value: BigInt(value) };
      }
      return { type: 'double', value };
    }

    if (typeof value === 'string') {
      return { type: 'string', value };
    }

    if (Buffer.isBuffer(value)) {
      return { type: 'binary', value };
    }

    if ((typeof value === 'object' || typeof value === 'function') && value !== null) {
      const core = remoteCoreByObject.get(value);
      if (core !== undefined) {
        if (core.context !== this) {
          throw new TypeError('An XLang object cannot be passed to a different XLang runtime');
        }
        core.assertActive();
        return {
          type: 'handle',
          value: core.handle,
          objectType: core.objectType
        };
      }
    }

    throw new TypeError(
      'Unsupported XLang value; expected a scalar, Buffer, bigint, or XLang object'
    );
  }

  validate(value: XLangTaggedValue): void {
    if (value === null || typeof value !== 'object' || typeof value.type !== 'string') {
      throw new TypeError('The XLang bridge returned an invalid tagged value');
    }

    switch (value.type) {
      case 'undefined':
        return;
      case 'null':
        return;
      case 'boolean':
        if (typeof value.value !== 'boolean') break;
        return;
      case 'int64':
        if (typeof value.value !== 'bigint') break;
        return;
      case 'double':
        if (typeof value.value !== 'number') break;
        return;
      case 'string':
        if (typeof value.value !== 'string') break;
        return;
      case 'binary':
        if (!Buffer.isBuffer(value.value)) break;
        return;
      case 'handle':
        if (typeof value.value !== 'bigint' || typeof value.objectType !== 'number') break;
        return;
    }

    throw new TypeError(`The XLang bridge returned an invalid ${value.type} value`);
  }

  decode(value: XLangTaggedValue): unknown {
    this.validate(value);
    switch (value.type) {
      case 'undefined':
        return undefined;
      case 'null':
        return null;
      case 'boolean':
      case 'double':
      case 'string':
      case 'binary':
        return value.value;
      case 'int64':
        if (value.value >= minimumSafeInteger && value.value <= maximumSafeInteger) {
          return Number(value.value);
        }
        return value.value;
      case 'handle':
        return this.getOrCreateObject(value.value, value.objectType);
    }
  }

  releaseUndelivered(values: XLangTaggedValue[]): void {
    const handles = new Set<bigint>();
    for (const value of values) {
      if (value?.type === 'handle' && typeof value.value === 'bigint') {
        handles.add(value.value);
      }
    }
    void Promise.allSettled(
      [...handles].map(async (handle) => this.native.release(handle))
    );
  }

  isObject(value: unknown): value is XLangObject {
    return (
      (typeof value === 'object' || typeof value === 'function') &&
      value !== null &&
      remoteCoreByObject.get(value)?.context === this
    );
  }

  forget(core: XLangRemoteObjectCore): void {
    if (this.objects.get(core.handle) === core) {
      this.objects.delete(core.handle);
    }
  }

  invalidateAll(): void {
    for (const core of this.objects.values()) {
      core.invalidate();
    }
    this.objects.clear();
  }

  private getOrCreateObject(handle: bigint, objectType: number): XLangObject {
    const existing = this.objects.get(handle);
    if (existing !== undefined && !existing.isDisposed) {
      return existing.proxy;
    }

    const core = new XLangRemoteObjectCore(this, handle, objectType);
    this.objects.set(handle, core);
    remoteCoreByObject.set(core, core);
    remoteCoreByObject.set(core.proxy, core);
    return core.proxy;
  }
}

interface XLangSubscriptionRecord {
  eventName: string;
  listener: XLangListener;
  active: boolean;
  cancelRequested: boolean;
  removed: boolean;
  token?: bigint;
  tokenPromise: Promise<bigint>;
  removalPromise?: Promise<void>;
}

class XLangRemoteObjectCore implements XLangObjectMethods {
  private readonly methodCache = new Map<PropertyKey, unknown>();
  private readonly subscriptions = new Map<
    string,
    Map<XLangListener, Set<XLangSubscriptionRecord>>
  >();
  private disposePromise?: Promise<void>;
  private disposed = false;
  private released = false;

  readonly proxy: XLangObject;

  constructor(
    readonly context: XLangContext,
    readonly handle: bigint,
    readonly objectType: number
  ) {
    this.proxy = new Proxy(this, {
      get: (target, property) => target.getProxyProperty(property),
      set: () => {
        throw new TypeError('Assigning XLang properties is asynchronous; use object.set(name, value)');
      },
      defineProperty: () => {
        throw new TypeError('XLang proxy properties cannot be defined');
      },
      deleteProperty: () => {
        throw new TypeError('XLang proxy properties cannot be deleted');
      },
      setPrototypeOf: () => {
        throw new TypeError('The XLang proxy prototype cannot be changed');
      },
      preventExtensions: () => {
        throw new TypeError('XLang proxies cannot be made non-extensible');
      },
      has: (_target, property) => typeof property === 'string' && explicitProxyMethods.has(property),
      ownKeys: () => [],
      getOwnPropertyDescriptor: () => undefined,
      getPrototypeOf: () => null
    }) as unknown as XLangObject;
  }

  get isDisposed(): boolean {
    return this.disposed;
  }

  assertActive(): void {
    if (this.disposed) {
      throw new Error('This XLang object has been disposed');
    }
  }

  async get(name: string): Promise<unknown> {
    this.assertActive();
    validateName(name, 'name');
    return this.context.decode(await this.context.native.get(this.handle, name));
  }

  async set(name: string, value: unknown): Promise<void> {
    this.assertActive();
    validateName(name, 'name');
    await this.context.native.set(this.handle, name, this.context.encode(value));
  }

  async invoke(args: unknown[] = [], options: XLangCallOptions = {}): Promise<unknown> {
    this.assertActive();
    const encoded = this.encodeCallInputs(args, options);
    return this.context.decode(
      await this.context.native.invoke(this.handle, encoded.args, encoded.kwargs)
    );
  }

  async call(
    name: string,
    args: unknown[] = [],
    options: XLangCallOptions = {}
  ): Promise<unknown> {
    this.assertActive();
    validateName(name, 'name');
    const encoded = this.encodeCallInputs(args, options);
    return this.context.decode(
      await this.context.native.call(this.handle, name, encoded.args, encoded.kwargs)
    );
  }

  async on(eventName: string, listener: XLangListener): Promise<void> {
    this.assertActive();
    validateName(eventName, 'eventName');
    if (typeof listener !== 'function') {
      throw new TypeError('listener must be a function');
    }

    let resolveToken!: (token: bigint) => void;
    let rejectToken!: (error: unknown) => void;
    const tokenPromise = new Promise<bigint>((resolve, reject) => {
      resolveToken = resolve;
      rejectToken = reject;
    });
    const record: XLangSubscriptionRecord = {
      eventName,
      listener,
      active: true,
      cancelRequested: false,
      removed: false,
      tokenPromise
    };
    this.addSubscription(record);

    const callback: XLangNativeEventCallback = (eventArgs, eventKwargs) => {
      this.dispatchEvent(record, eventArgs, eventKwargs);
    };

    try {
      const registration = this.context.native.on(this.handle, eventName, callback);
      void registration.then(
        (token) => {
          record.token = token;
          resolveToken(token);
        },
        (error) => {
          record.active = false;
          record.removed = true;
          this.deleteSubscription(record);
          rejectToken(error);
        }
      );
    } catch (error) {
      record.active = false;
      record.removed = true;
      this.deleteSubscription(record);
      rejectToken(error);
    }

    await tokenPromise;
    if (this.disposed) {
      await this.ensureSubscriptionRemoved(record);
      throw new Error('This XLang object has been disposed');
    }
    if (record.cancelRequested) {
      await this.ensureSubscriptionRemoved(record);
    }
  }

  async off(eventName: string, listener?: XLangListener): Promise<void> {
    this.assertActive();
    validateName(eventName, 'eventName');
    if (listener !== undefined && typeof listener !== 'function') {
      throw new TypeError('listener must be a function');
    }

    const eventListeners = this.subscriptions.get(eventName);
    if (eventListeners === undefined) return;

    const records =
      listener === undefined
        ? [...eventListeners.values()].flatMap((listenerRecords) => [...listenerRecords])
        : [...(eventListeners.get(listener) ?? [])];
    await this.removeSubscriptions(records);
  }

  async dispose(): Promise<void> {
    if (this.released) return;
    if (this.disposePromise !== undefined) {
      return this.disposePromise;
    }

    this.disposed = true;
    const attempt = this.release();
    this.disposePromise = attempt;
    try {
      await attempt;
    } finally {
      if (!this.released && this.disposePromise === attempt) {
        this.disposePromise = undefined;
      }
    }
  }

  invalidate(): void {
    this.disposed = true;
    this.released = true;
    for (const record of this.getSubscriptionRecords()) {
      record.active = false;
      record.removed = true;
    }
    this.subscriptions.clear();
    this.disposePromise = Promise.resolve();
  }

  private encodeCallInputs(
    args: unknown[],
    options: XLangCallOptions
  ): {
    args: XLangTaggedValue[];
    kwargs: Record<string, XLangTaggedValue>;
  } {
    if (!Array.isArray(args)) {
      throw new TypeError('args must be an array');
    }
    if (options === null || typeof options !== 'object' || Array.isArray(options)) {
      throw new TypeError('options must be an object');
    }
    if (
      options.kwargs !== undefined &&
      (options.kwargs === null || typeof options.kwargs !== 'object' || Array.isArray(options.kwargs))
    ) {
      throw new TypeError('options.kwargs must be an object');
    }

    const encodedArgs = args.map((arg) => this.context.encode(arg));
    const encodedKwargs = Object.create(null) as Record<string, XLangTaggedValue>;
    for (const [key, value] of Object.entries(options.kwargs ?? {})) {
      validateName(key, 'keyword name');
      encodedKwargs[key] = this.context.encode(value);
    }
    return { args: encodedArgs, kwargs: encodedKwargs };
  }

  private dispatchEvent(
    record: XLangSubscriptionRecord,
    args: XLangTaggedValue[],
    kwargs: Record<string, XLangTaggedValue>
  ): void {
    const taggedValues = [...args, ...Object.values(kwargs)];
    if (this.disposed || !record.active) {
      this.context.releaseUndelivered(taggedValues);
      return;
    }

    for (const value of taggedValues) {
      try {
        this.context.validate(value);
      } catch (error) {
        this.context.releaseUndelivered(taggedValues);
        throw error;
      }
    }

    let event: XLangEvent;
    try {
      const decodedKwargs: Record<string, unknown> = {};
      for (const [key, value] of Object.entries(kwargs)) {
        Object.defineProperty(decodedKwargs, key, {
          configurable: true,
          enumerable: true,
          value: this.context.decode(value),
          writable: true
        });
      }
      event = {
        args: args.map((arg) => this.context.decode(arg)),
        kwargs: decodedKwargs
      };
    } catch (error) {
      this.context.releaseUndelivered(taggedValues);
      throw error;
    }

    record.listener.call(this.proxy, event);
  }

  private addSubscription(record: XLangSubscriptionRecord): void {
    let eventListeners = this.subscriptions.get(record.eventName);
    if (eventListeners === undefined) {
      eventListeners = new Map();
      this.subscriptions.set(record.eventName, eventListeners);
    }

    let records = eventListeners.get(record.listener);
    if (records === undefined) {
      records = new Set();
      eventListeners.set(record.listener, records);
    }
    records.add(record);
  }

  private deleteSubscription(record: XLangSubscriptionRecord): void {
    const eventListeners = this.subscriptions.get(record.eventName);
    if (eventListeners === undefined) return;
    const records = eventListeners.get(record.listener);
    if (records === undefined) return;
    records.delete(record);
    if (records.size === 0) {
      eventListeners.delete(record.listener);
    }
    if (eventListeners.size === 0) {
      this.subscriptions.delete(record.eventName);
    }
  }

  private ensureSubscriptionRemoved(record: XLangSubscriptionRecord): Promise<void> {
    record.active = false;
    record.cancelRequested = true;
    if (record.removed) return Promise.resolve();
    if (record.removalPromise !== undefined) return record.removalPromise;

    record.removalPromise = (async () => {
      const token = record.token ?? (await record.tokenPromise);
      await this.context.native.off(token);
      record.removed = true;
      this.deleteSubscription(record);
    })().catch((error) => {
      record.removalPromise = undefined;
      throw error;
    });
    return record.removalPromise;
  }

  private getSubscriptionRecords(): XLangSubscriptionRecord[] {
    const records: XLangSubscriptionRecord[] = [];
    for (const eventListeners of this.subscriptions.values()) {
      for (const listenerRecords of eventListeners.values()) {
        records.push(...listenerRecords);
      }
    }
    return records;
  }

  private async removeSubscriptions(records: XLangSubscriptionRecord[]): Promise<void> {
    const results = await Promise.allSettled(
      records.map(async (record) => this.ensureSubscriptionRemoved(record))
    );
    const failure = results.find(
      (result): result is PromiseRejectedResult => result.status === 'rejected'
    );
    if (failure !== undefined) {
      throw failure.reason;
    }
  }

  private getProxyProperty(property: string | symbol): unknown {
    if (property === 'then' || property === 'toJSON') {
      return undefined;
    }
    if (property === Symbol.toStringTag) {
      return 'XLangObject';
    }
    if (property === inspectSymbol) {
      return () => `XLangObject { type: ${this.objectType}, handle: ${this.handle}n }`;
    }
    if (typeof property === 'symbol') {
      return Reflect.get(this, property, this);
    }

    const cached = this.methodCache.get(property);
    if (cached !== undefined) return cached;

    let method: unknown;
    if (explicitProxyMethods.has(property)) {
      const value = Reflect.get(this, property, this);
      method = typeof value === 'function' ? value.bind(this) : value;
    } else {
      method = (...args: unknown[]) => this.call(property, args);
    }
    this.methodCache.set(property, method);
    return method;
  }

  private async release(): Promise<void> {
    await this.removeSubscriptions(this.getSubscriptionRecords());
    await this.context.native.release(this.handle);
    this.released = true;
    this.context.forget(this);
  }
}

export const createXLangFacade = (
  native: XLangNativeBinding,
  configuration: XLangFacadeConfiguration = {}
): XLangFacade => {
  const context = new XLangContext(native);
  let initialized = false;
  let lifecycle = Promise.resolve();

  const serializeLifecycle = <T>(operation: () => Promise<T>): Promise<T> => {
    const result = lifecycle.then(operation, operation);
    lifecycle = result.then(
      () => undefined,
      () => undefined
    );
    return result;
  };

  const initializeUnlocked = async (options: XLangInitializeOptions = {}): Promise<void> => {
    if (options === null || typeof options !== 'object' || Array.isArray(options)) {
      throw new TypeError('options must be an object');
    }
    if (options.libraryPath !== undefined && typeof options.libraryPath !== 'string') {
      throw new TypeError('options.libraryPath must be a string');
    }
    if (options.appPath !== undefined && typeof options.appPath !== 'string') {
      throw new TypeError('options.appPath must be a string');
    }
    if (
      options.librarySearchPaths !== undefined &&
      (!Array.isArray(options.librarySearchPaths) ||
        options.librarySearchPaths.some((searchPath) => typeof searchPath !== 'string'))
    ) {
      throw new TypeError('options.librarySearchPaths must be an array of strings');
    }
    if (options.enablePython !== undefined && typeof options.enablePython !== 'boolean') {
      throw new TypeError('options.enablePython must be a boolean');
    }
    if (options.debug !== undefined && typeof options.debug !== 'boolean') {
      throw new TypeError('options.debug must be a boolean');
    }
    if (initialized) return;

    const { libraryPath, ...runtimeOptions } = options;
    if (!native.isLoaded()) {
      native.load(libraryPath ?? configuration.getDefaultLibraryPath?.());
    }
    await native.initialize(runtimeOptions);
    initialized = true;
  };

  const facade: XLangFacade = {
    initialize: (options = {}) => serializeLifecycle(async () => initializeUnlocked(options)),

    importModule: (name: string, options: XLangImportOptions = {}) =>
      serializeLifecycle(async () => {
        validateName(name, 'name');
        validateImportOptions(options);
        await initializeUnlocked();
        const imported = context.decode(await native.importModule(name, options));
        if (!context.isObject(imported)) {
          throw new TypeError('The XLang bridge did not return a module handle');
        }
        return imported;
      }),

    shutdown: () =>
      serializeLifecycle(async () => {
        if (!native.isLoaded()) {
          context.invalidateAll();
          return;
        }

        try {
          await native.shutdown();
        } finally {
          initialized = false;
          context.invalidateAll();
        }
      })
  };

  return facade;
};
