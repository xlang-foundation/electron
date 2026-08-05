import { expect } from 'chai';

import { createXLangFacade, XLangEvent, XLangNativeBinding, XLangTaggedValue } from '../lib/browser/api/xlang-remote';

type NativeEventCallback = Parameters<XLangNativeBinding['on']>[2];

const deferred = <T>() => {
  let resolvePromise!: (value: T | PromiseLike<T>) => void;
  const promise = new Promise<T>((resolve) => {
    resolvePromise = resolve;
  });
  return { promise, resolve: resolvePromise };
};

const makeNativeBinding = () => {
  let loaded = false;
  let nextToken = 1n;
  const callbacks = new Map<bigint, NativeEventCallback>();
  const calls = {
    load: [] as Array<string | undefined>,
    initialize: [] as Array<Record<string, unknown>>,
    imports: [] as Array<{ name: string; options: { fromPath?: string; thru?: string } }>,
    get: [] as Array<{ handle: bigint; name: string }>,
    set: [] as Array<{ handle: bigint; name: string; value: XLangTaggedValue }>,
    call: [] as Array<{
      handle: bigint;
      name: string;
      args: XLangTaggedValue[];
      kwargs: Record<string, XLangTaggedValue>;
    }>,
    invoke: [] as Array<{
      handle: bigint;
      args: XLangTaggedValue[];
      kwargs: Record<string, XLangTaggedValue>;
    }>,
    release: [] as bigint[],
    on: [] as Array<{ handle: bigint; eventName: string }>,
    off: [] as bigint[],
    shutdown: 0
  };

  const native: XLangNativeBinding = {
    load(libraryPath) {
      calls.load.push(libraryPath);
      loaded = true;
    },

    isLoaded() {
      return loaded;
    },

    async initialize(options) {
      calls.initialize.push(options);
    },

    async shutdown() {
      calls.shutdown++;
    },

    async importModule(name, options) {
      calls.imports.push({ name, options });
      return { type: 'handle', value: 1n, objectType: 10 };
    },

    async get(handle, name) {
      calls.get.push({ handle, name });
      if (name === 'large') {
        return { type: 'int64', value: 9_007_199_254_740_992n };
      }
      return { type: 'int64', value: 42n };
    },

    async set(handle, name, value) {
      calls.set.push({ handle, name, value });
      return { type: 'undefined' };
    },

    async invoke(handle, args, kwargs) {
      calls.invoke.push({ handle, args, kwargs });
      return { type: 'string', value: 'invoke' };
    },

    async call(handle, name, args, kwargs) {
      calls.call.push({ handle, name, args, kwargs });
      return { type: 'string', value: name };
    },

    callMemberSync(handle, name, args, kwargs) {
      calls.call.push({ handle, name, args, kwargs });
      return { type: 'string', value: name };
    },

    async release(handle) {
      calls.release.push(handle);
    },

    async on(handle, eventName, callback) {
      calls.on.push({ handle, eventName });
      const token = nextToken++;
      callbacks.set(token, callback);
      return token;
    },

    async off(token) {
      calls.off.push(token);
      callbacks.delete(token);
    }
  };

  return { native, calls, callbacks };
};

describe('XLang JavaScript facade', () => {
  it('lazily loads, initializes, and imports with XLang path options', async () => {
    const { native, calls } = makeNativeBinding();
    const xlang = createXLangFacade(native, {
      getDefaultLibraryPath: () => 'resources/xlang'
    });

    const module = await xlang.importModule('yaml', {
      fromPath: 'xlang_yaml',
      thru: 'lrpc:9089'
    });

    expect(calls.load).to.deep.equal(['resources/xlang']);
    expect(calls.initialize).to.deep.equal([{}]);
    expect(calls.imports).to.deep.equal([
      {
        name: 'yaml',
        options: { fromPath: 'xlang_yaml', thru: 'lrpc:9089' }
      }
    ]);
    expect((module as { then?: unknown }).then).to.equal(undefined);
    expect(await Promise.resolve(module)).to.equal(module);
  });

  it('uses an explicit library path and only initializes once', async () => {
    const { native, calls } = makeNativeBinding();
    const xlang = createXLangFacade(native);

    await Promise.all([
      xlang.initialize({ libraryPath: 'dev/electron_xlang_bridge', appPath: 'app' }),
      xlang.initialize()
    ]);
    await xlang.importModule('first');
    await xlang.importModule('second');

    expect(calls.load).to.deep.equal(['dev/electron_xlang_bridge']);
    expect(calls.initialize).to.deep.equal([{ appPath: 'app' }]);
    expect(calls.imports.map(({ name }) => name)).to.deep.equal(['first', 'second']);
  });

  it('can retry initialization after an initialization failure', async () => {
    const { native, calls } = makeNativeBinding();
    let attempts = 0;
    native.initialize = async (options) => {
      calls.initialize.push(options);
      if (attempts++ === 0) throw new Error('temporary initialization failure');
    };
    const xlang = createXLangFacade(native);

    await expect(xlang.importModule('first')).to.be.rejectedWith('temporary initialization failure');
    await xlang.importModule('second');

    expect(calls.initialize).to.deep.equal([{}, {}]);
    expect(calls.imports.map(({ name }) => name)).to.deep.equal(['second']);
  });

  it('shuts down a loaded bridge after initialization fails', async () => {
    const { native, calls } = makeNativeBinding();
    native.initialize = async (options) => {
      calls.initialize.push(options);
      throw new Error('initialization failure');
    };
    const xlang = createXLangFacade(native);

    await expect(xlang.initialize()).to.be.rejectedWith('initialization failure');
    await xlang.shutdown();

    expect(calls.load).to.deep.equal([undefined]);
    expect(calls.shutdown).to.equal(1);
  });

  it('supports natural calls and explicit get, set, and call operations', async () => {
    const { native, calls } = makeNativeBinding();
    const xlang = createXLangFacade(native);
    const module = await xlang.importModule('cantor');

    expect(await module.get('answer')).to.equal(42);
    expect(await module.get('large')).to.equal(9_007_199_254_740_992n);
    await module.set('enabled', true);
    const naturalModule = module as unknown as {
      render(...args: unknown[]): Promise<unknown>;
    };
    expect(await naturalModule.render('hello', 7, 8n, Buffer.from([1, 2]))).to.equal('render');
    expect(
      await module.call('get', ['remote member'], {
        kwargs: { fallback: false }
      })
    ).to.equal('get');
    expect(module.callSync('statement', ['select 1'])).to.equal('statement');
    expect(await module.invoke(['direct'], { kwargs: { mode: 'fast' } })).to.equal('invoke');

    expect(calls.set).to.deep.equal([
      {
        handle: 1n,
        name: 'enabled',
        value: { type: 'boolean', value: true }
      }
    ]);
    expect(calls.call[0]).to.deep.equal({
      handle: 1n,
      name: 'render',
      args: [
        { type: 'string', value: 'hello' },
        { type: 'int64', value: 7n },
        { type: 'int64', value: 8n },
        { type: 'binary', value: Buffer.from([1, 2]) }
      ],
      kwargs: {}
    });
    expect(calls.call[1].kwargs).to.deep.equal({
      fallback: { type: 'boolean', value: false }
    });
    expect(calls.call[2]).to.deep.equal({
      handle: 1n,
      name: 'statement',
      args: [{ type: 'string', value: 'select 1' }],
      kwargs: {}
    });
    expect(calls.invoke).to.deep.equal([
      {
        handle: 1n,
        args: [{ type: 'string', value: 'direct' }],
        kwargs: { mode: { type: 'string', value: 'fast' } }
      }
    ]);
  });

  it('awaits event registration and removal and decodes event values', async () => {
    const { native, calls, callbacks } = makeNativeBinding();
    const xlang = createXLangFacade(native);
    const module = await xlang.importModule('cantor');
    const received: XLangEvent[] = [];
    const listener = (event: XLangEvent) => received.push(event);

    await module.on('ready', listener);
    expect(calls.on).to.deep.equal([{ handle: 1n, eventName: 'ready' }]);

    callbacks.get(1n)?.(
      [
        { type: 'string', value: 'ok' },
        { type: 'int64', value: 9_007_199_254_740_992n }
      ],
      {
        source: { type: 'string', value: 'lrpc' }
      }
    );
    expect(received).to.deep.equal([
      {
        args: ['ok', 9_007_199_254_740_992n],
        kwargs: { source: 'lrpc' }
      }
    ]);

    await module.off('ready', listener);
    expect(calls.off).to.deep.equal([1n]);
    expect(callbacks.size).to.equal(0);
  });

  it('releases subscriptions and its native handle exactly once', async () => {
    const { native, calls } = makeNativeBinding();
    const xlang = createXLangFacade(native);
    const module = await xlang.importModule('cantor');

    await module.on('ready', () => {});
    await module.dispose();
    await module.dispose();

    expect(calls.off).to.deep.equal([1n]);
    expect(calls.release).to.deep.equal([1n]);
    await expect(module.get('answer')).to.be.rejectedWith('This XLang object has been disposed');
  });

  it('removes a subscription that finishes registering during disposal', async () => {
    const { native, calls } = makeNativeBinding();
    const registration = deferred<bigint>();
    const pending: { callback?: NativeEventCallback } = {};
    native.on = async (_handle, _eventName, callback) => {
      pending.callback = callback;
      return registration.promise;
    };
    const xlang = createXLangFacade(native);
    const module = await xlang.importModule('cantor');

    const registering = module.on('ready', () => {});
    const registrationRejection = expect(registering).to.be.rejectedWith('This XLang object has been disposed');
    const disposing = module.dispose();
    pending.callback?.([{ type: 'handle', value: 22n, objectType: 5 }], {});
    registration.resolve(99n);

    await registrationRejection;
    await disposing;
    expect(calls.off).to.deep.equal([99n]);
    expect(calls.release).to.have.members([22n, 1n]);
  });

  it('cancels registration when an early event listener calls off', async () => {
    const { native, calls } = makeNativeBinding();
    const registration = deferred<bigint>();
    native.on = async (_handle, _eventName, callback) => {
      callback([{ type: 'string', value: 'early' }], {});
      return registration.promise;
    };
    const xlang = createXLangFacade(native);
    const module = await xlang.importModule('cantor');
    let removal: Promise<void> | undefined;
    const listener = () => {
      removal = module.off('ready', listener);
    };

    const registering = module.on('ready', listener);
    registration.resolve(41n);
    await registering;
    await removal;

    expect(calls.off).to.deep.equal([41n]);
  });

  it('keeps a failed event removal so off can retry it', async () => {
    const { native, calls } = makeNativeBinding();
    let shouldFail = true;
    native.off = async (token) => {
      calls.off.push(token);
      if (shouldFail) throw new Error('temporary off failure');
    };
    const xlang = createXLangFacade(native);
    const module = await xlang.importModule('cantor');
    const listener = () => {};
    await module.on('ready', listener);

    await expect(module.off('ready', listener)).to.be.rejectedWith('temporary off failure');
    shouldFail = false;
    await module.off('ready', listener);

    expect(calls.off).to.deep.equal([1n, 1n]);
  });

  it('retries disposal cleanup before releasing the object handle', async () => {
    const { native, calls } = makeNativeBinding();
    let shouldFail = true;
    native.off = async (token) => {
      calls.off.push(token);
      if (shouldFail) throw new Error('temporary dispose failure');
    };
    const xlang = createXLangFacade(native);
    const module = await xlang.importModule('cantor');
    await module.on('ready', () => {});

    await expect(module.dispose()).to.be.rejectedWith('temporary dispose failure');
    expect(calls.release).to.deep.equal([]);

    shouldFail = false;
    await module.dispose();
    expect(calls.off).to.deep.equal([1n, 1n]);
    expect(calls.release).to.deep.equal([1n]);
  });

  it('releases event handles when decoding fails before delivery', async () => {
    const { native, calls, callbacks } = makeNativeBinding();
    const xlang = createXLangFacade(native);
    const module = await xlang.importModule('cantor');
    let delivered = false;
    await module.on('ready', () => {
      delivered = true;
    });

    expect(() => {
      callbacks.get(1n)?.(
        [
          { type: 'handle', value: 55n, objectType: 5 },
          { type: 'boolean', value: 'invalid' } as unknown as XLangTaggedValue
        ],
        {}
      );
    }).to.throw('invalid boolean value');
    expect(delivered).to.equal(false);
    expect(calls.release).to.include(55n);
  });

  it('rejects unsupported values and direct proxy assignment', async () => {
    const { native } = makeNativeBinding();
    const xlang = createXLangFacade(native);
    const module = await xlang.importModule('cantor');

    await expect(module.call('takeList', [[]])).to.be.rejectedWith('Unsupported XLang value');
    await expect(module.call('takeInteger', [Number.MAX_SAFE_INTEGER + 1])).to.be.rejectedWith(
      'Unsafe integer arguments must be passed as bigint'
    );
    await expect(module.call('takeInteger', [1n << 63n])).to.be.rejectedWith(
      'XLang bigint values must fit in a signed 64-bit integer'
    );
    expect(() => {
      (module as unknown as { value: unknown }).value = 1;
    }).to.throw('use object.set(name, value)');
  });

  it('invalidates existing proxies when the runtime shuts down', async () => {
    const { native, calls } = makeNativeBinding();
    const xlang = createXLangFacade(native);
    const module = await xlang.importModule('cantor');

    await xlang.shutdown();

    expect(calls.shutdown).to.equal(1);
    await expect(module.call('render')).to.be.rejectedWith('This XLang object has been disposed');
  });

  it('serializes an import during shutdown and surfaces process-final restart rejection', async () => {
    const { native, calls } = makeNativeBinding();
    const shutdown = deferred<void>();
    let stopped = false;
    native.shutdown = async () => {
      calls.shutdown++;
      await shutdown.promise;
      stopped = true;
    };
    native.initialize = async (options) => {
      calls.initialize.push(options);
      if (stopped) throw new Error('XLang bridge: unsupported');
    };
    const xlang = createXLangFacade(native);
    await xlang.importModule('first');

    const stopping = xlang.shutdown();
    await Promise.resolve();
    const importing = xlang.importModule('second');
    const importRejection = expect(importing).to.be.rejectedWith('XLang bridge: unsupported');
    await Promise.resolve();

    expect(calls.imports.map(({ name }) => name)).to.deep.equal(['first']);
    shutdown.resolve(undefined);
    await stopping;
    await importRejection;

    expect(calls.initialize).to.deep.equal([{}, {}]);
    expect(calls.imports.map(({ name }) => name)).to.deep.equal(['first']);
  });
});
