# CatTrackApp

The phone app. It connects to the handheld over BLE and shows one row per cat:
name, the dBm and SNR of that cat's latest packet, battery, and how long ago
that cat was heard. The cat being followed also gets its dBm in the largest
type on the screen.

The project documentation — what the app shows, how to build the release APK,
and how to check the handheld's half of the link without a phone — is in the
[repository README](../README.md). This directory is only the React Native
project itself.

For day-to-day development the stock React Native workflow applies:

```bash
npm install
npm test            # jest, including the mocked-BLE render tests
npm start           # Metro
npm run android     # debug build; needs Metro running
```

The release build (`../tools/build_release.sh --install`) embeds the JS bundle,
so it runs with no Mac, no Metro and no cable.
