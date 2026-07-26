import { createXLangFacade } from '@electron/internal/browser/api/xlang-remote';

import * as path from 'path';

const { xlang: native } = process._linkedBinding('electron_browser_xlang');

export default createXLangFacade(native, {
  getDefaultLibraryPath: () =>
    process.env.ELECTRON_XLANG_LIBRARY_PATH
      ? path.resolve(process.env.ELECTRON_XLANG_LIBRARY_PATH)
      : path.join(process.resourcesPath, 'xlang')
});
