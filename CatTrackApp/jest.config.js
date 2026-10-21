/**
 * Jest configuration.
 *
 * The transformIgnorePatterns default shipped with the React Native preset
 * leaves every node_modules package untransformed, and several of the native
 * modules this app uses are published as ES modules. Jest then chokes on
 * "export { ... }" the moment it tries to load them, and the app test fails
 * before it runs a single assertion - which is the same as having no test at
 * all, because a suite that always fails is a suite nobody reads.
 *
 * @format
 */

module.exports = {
  preset: '@react-native/jest-preset',
  setupFiles: ['<rootDir>/jest.setup.js'],
  transformIgnorePatterns: [
    'node_modules/(?!((jest-)?react-native|@react-native(-community)?' +
      '|react-native-ble-plx|react-native-file-access|react-native-share' +
      '|react-native-sound)/)',
  ],
};
