import { xlang } from 'electron/main';

import { expect } from 'chai';

describe('xlang module', () => {
  it('exports the asynchronous facade in the main process', () => {
    expect(xlang).to.have.property('initialize').that.is.a('function');
    expect(xlang).to.have.property('importModule').that.is.a('function');
    expect(xlang).to.have.property('shutdown').that.is.a('function');
  });
});
