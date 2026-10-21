/**
 * Jest setup: stand-ins for the native modules the app talks to.
 *
 * Every one of these is a native module with no JavaScript implementation, so
 * without a mock the app cannot even be imported in a test - the import of
 * App.tsx constructs a BleManager, which reaches for a native event emitter
 * that does not exist outside a device.
 *
 * These are deliberately dumb. They exist so the render test can run, not so
 * behaviour can be asserted against them.
 *
 * @format
 */

/* eslint-env jest */

jest.mock('react-native-ble-plx', () => {
  const State = {
    PoweredOn: 'PoweredOn',
    PoweredOff: 'PoweredOff',
    Unsupported: 'Unsupported',
    Unauthorized: 'Unauthorized',
    Resetting: 'Resetting',
    Unknown: 'Unknown',
  };
  return {
    State,
    BleManager: jest.fn(() => ({
      state: jest.fn(async () => State.PoweredOn),
      startDeviceScan: jest.fn(async () => undefined),
      stopDeviceScan: jest.fn(async () => undefined),
    })),
  };
});

jest.mock('react-native-sound', () => {
  class Sound {
    static MAIN_BUNDLE = '';
    static setCategory() {}
    constructor() {}
    play() {}
    stop() {}
    release() {}
    isPlaying() {
      return false;
    }
    setCurrentTime() {}
  }
  return Sound;
});

jest.mock('react-native-file-access', () => ({
  Dirs: {DocumentDir: '/tmp/cattrack'},
  FileSystem: {
    readFile: jest.fn(async () => ''),
    writeFile: jest.fn(async () => undefined),
    appendFile: jest.fn(async () => undefined),
  },
}));

jest.mock('react-native-share', () => ({
  open: jest.fn(async () => undefined),
  default: {open: jest.fn(async () => undefined)},
}));

/**
 * The safe-area context's own mock.
 *
 * Without it the real SafeAreaProvider renders NO children at all: it waits
 * for a layout event to work out the insets, and in a test that event never
 * arrives. The App is inside that provider, so the whole screen renders as
 * an empty provider - and a render test that asserts nothing still passes.
 *
 * That is not a hypothetical. The first render test in this repo was passing
 * while drawing nothing, which is why a duplicated line of text survived
 * several builds.
 */
jest.mock('react-native-safe-area-context', () =>
  // The mock is a default export, so it needs .default to line up with the
  // named imports the app uses.
  require('react-native-safe-area-context/jest/mock').default,
);
