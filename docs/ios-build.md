# Building the app for an iPhone

**Status: the native project builds up to the archive step; the archive itself was not produced on this
machine.** The Xcode build service (an XPC service outside the agent's sandbox) is refused write access
here, so the last mile — `xcodebuild archive` — has to run on your Mac. Everything before it works:
`expo prebuild --platform ios` generates a valid project, `pod install` resolves all pods **including the
local `KeystoneNet` module**, and the workspace opens in Xcode. What *was* checked on the Linux dev box:
`expo-doctor` (21/21), the JavaScript bundle for iOS (`expo export --platform ios`, Metro
stage: 615 modules, includes the native-module bindings), `expo prebuild --platform ios` (a valid Xcode
project with the right Info.plist keys, deployment target 16.4, autolinking finds `KeystoneNet`),
the Swift module against the Expo Modules API signatures (`Function`, `Events`, `OnDestroy`, `sendEvent`),
and a syntax check of the Swift. Expect to fix a compile error or two: send them to whoever maintains
the module (see "If it does not compile").

**Signing:** the project is set to automatic signing, team `5H749MZFPN`, bundle id
`com.kliuchnikovv.keystone`. The provisioning profile on this machine (team `5H749MZFPN`, device
`00008130-0004250E0E62001C`) **expired on 2026-10-24**, and Xcode's stored Apple ID credentials are
invalid ("missing Xcode-Username"), so nothing could be renewed from here. Sign in to Xcode
(▸ Settings ▸ Accounts) before building for a device; a free Apple ID is enough for your own phone
(the profile then lives 7 days).

## Scene lifecycle (the first launch-time crash, Xcode 27)

On Xcode 27 / iOS 27 the app **starts and is killed immediately**:

```
EXC_BREAKPOINT (code=1)
_UIApplicationEvaluateRuntimeIssueForNoSceneLifecycleAdoption
"Application failed to launch: UIScene lifecycle is required for apps built with this SDK."
```

The Expo template's `AppDelegate` is window-based — `ExpoAppDelegate` conforms to
`UIApplicationDelegate` only and implements none of `configurationForConnecting`,
`windowScene(_:willConnectTo:options:)`, so iOS refuses the launch. Two things are needed:

1. `UIApplicationSceneManifest` in `app.json` → `ios.infoPlist` (present), and
2. an AppDelegate that adopts scenes — `plugins/withSceneLifecycle.cjs` writes
   `plugins/AppDelegate.swift.tmpl` over `ios/Keystone/AppDelegate.swift` on every prebuild.

Note that `expo prebuild` **without `--clean` does not touch an existing `Info.plist`**, so a change
made only in `app.json` will not reach the project until `ios/` is deleted or `--clean` is passed.
Declaring the manifest alone is not enough and makes things worse: pointing
`UISceneDelegateClassName` at an AppDelegate that cannot handle `willConnectTo` yields a black screen.

## Why a development build, not Expo Go

Pinning the hub's certificate and browsing mDNS need native code (`app/modules/keystone-net`: a
`URLSession` delegate that compares the certificate's SHA-256, and `NetServiceBrowser`). Expo Go cannot
load it, so the app is built as its own *development client* (the app plus the Expo dev menu, loading
JavaScript from your computer), or later as a release build.

## What is already configured (`app/`)

- `app.json`: bundle id `dev.keystone.app` (**change it to your own**, see below), version 0.1.0, build 1,
  iOS keys `NSLocalNetworkUsageDescription`, `NSBonjourServices` (`_keystone._tcp`),
  `NSAppTransportSecurity/NSAllowsLocalNetworking` (plain `ws://` to an IP or `.local` name works),
  `ITSAppUsesNonExemptEncryption = false` (the app only uses the system's standard TLS), a placeholder icon.
- `expo-dev-client` is a dependency; `eas.json` has profiles `development` (device), `development-simulator`
  (iOS Simulator), `preview` and `production`.
- `modules/keystone-net/` is picked up automatically (autolinking): the module carries its own
  `package.json` and `app/package.json` points Expo at it with
  `expo.autolinking.nativeModulesDir = "./modules"`. Without both, the module is silently *not* linked —
  the iOS app then builds without the pinning socket and falls back to the JavaScript one (no pinning).
  Its podspec targets iOS 16.4 and Swift 5 language mode (Expo's own modules are Swift 6; this one
  avoids strict concurrency on purpose).
- `app/ios/` and `app/android/` are generated and **gitignored**: change `app.json` or the module, not the
  generated project.

## What you need

| | Route A: your Mac | Route B: EAS cloud build |
|---|---|---|
| Computer | a Mac with **Xcode 16.1+** and CocoaPods | any computer |
| Apple account | a free Apple ID is enough for your own iPhone (the app expires after 7 days) | a paid **Apple Developer Program** account (99 USD/year): EAS signs with it and registers your phone |
| Network | the phone and the computer on one network (for the dev server) | same |
| Good for | the first try; you see Xcode's errors directly | no Mac; the same build you can later put on TestFlight |

Both need Node 22+ and `npm install` in `app/`. On the iPhone: **Settings → Privacy & Security →
Developer Mode → on** (it restarts the phone), and trust your developer certificate (Settings → General →
VPN & Device Management) the first time.

### Before either route

1. Pick a bundle identifier nobody else uses, e.g. `com.<yourname>.keystone`, and set it in
   `app.json` (`ios.bundleIdentifier`, and `android.package` if you will build Android too). A free Apple ID
   cannot use an identifier another team already holds.
2. `cd app && npm install`.

### Route A — local build

```bash
cd app
npx expo prebuild --platform ios        # generates ios/ (Xcode project + Podfile); runs pod install
npx expo run:ios --device               # builds, installs on the connected iPhone, starts the dev server
```

If `run:ios` cannot sign, open `ios/Keystone.xcworkspace` in Xcode, select the *Keystone* target →
*Signing & Capabilities*, choose your Team, and run from Xcode. For the Simulator: `npx expo run:ios`
(the Simulator can reach your LAN, but cannot test Local Network permission behaviour the way a phone does).

### Release build (one command)

```bash
cd app
./scripts/ios-release.sh                 # device (arm64), archived and signed
./scripts/ios-release.sh simulator       # simulator build, no signing
```

The script does the whole chain: patches React Native's prebuilt-dependencies podspec (its cleanup step
is a bulk `rm` inside the CocoaPods cache, which fails on sandboxed hosts), runs `expo prebuild`, installs
the pods with the CocoaPods cache inside `build/`, and archives the Release configuration into
`build/ios-release/Keystone.xcarchive`. The app is at
`build/ios-release/Keystone.xcarchive/Products/Applications/Keystone.app`.

`app/ios/` is generated and gitignored, so the script regenerates it every time; change `app.json` or the
module, never the generated project.

### Route B — EAS

```bash
npm install -g eas-cli
cd app
eas login
eas device:create                        # registers the iPhone (opens a profile-install page on the phone)
eas build --profile development --platform ios
```

When the build finishes, open the link or QR code on the iPhone to install it, then on the computer:

```bash
npx expo start --dev-client              # the app finds this server, or scan the QR code
```

`eas build --profile production --platform ios` + `eas submit` is the TestFlight/App Store path; it also
needs a real icon and splash (the icon in `assets/` is a placeholder) and a privacy policy if you publish.

## First launch

1. iOS asks to allow **Local Network** access: say **Allow**. If you said no, nothing on your LAN will
   work (connections time out with "no route to host"): turn it on in Settings → Keystone → Local Network.
2. Start the hub's server and token on its console: `webtoken <token>`, then look at the line it prints:
   `certificate SHA-256: AA:BB:…`.
3. In the app: the connect screen lists hubs found by mDNS (tap one to fill the address), or type the
   address (`wss://192.168.0.202:8443/ws`). Enter the token and **the fingerprint exactly as the hub console
   printed it**. (Leave the fingerprint empty only for a hub with a certificate from a real CA: then the
   phone's normal trust is used, and a self-signed hub is simply refused.) Connect.

## Three checks that tell you whether pinning works

Do these before trusting the app (they are also in `app/README.md`):

1. **The right fingerprint connects.** Devices appear and a switch toggles.
2. **A wrong fingerprint is refused.** Change one digit: the app shows the "untrusted" banner, and the hub's
   console shows **no** `client … authenticated` line for it (the token must never have been sent).
3. **A replaced certificate is refused.** Regenerate the hub's identity (`scripts/gen-hub-cert.sh --force`,
   rebuild, flash) and reconnect with the old fingerprint: refused again.

Also worth a minute: turn Wi-Fi off and on on the phone while connected (the app should drop to
"reconnecting" and recover — it detects a silent connection after 35 s), and tap a hub in the discovered
list (it fills the address but never the fingerprint).

## Setting up a hub that is not on your Wi-Fi yet

The connect screen has **Set up a new hub** (docs/app-protocol.md §11). You need a **real phone**: the flow
needs the phone itself to join the hub's own Wi-Fi, and the Simulator uses the Mac's network (a Mac that joins
`Keystone-XXXX` loses its internet while it is there, but it does work as a stand-in).

1. Plug the hub in. With no saved network it opens a Wi-Fi called `Keystone-XXXX` (also after 10 minutes without a
   connection, or when the hub console says `setup on`). Its password is the **setup code**, which the hub console
   prints with `setup` and `scripts/gen-hub-cert.sh` printed when the identity was made.
2. Join that Wi-Fi in the phone's settings (ignore "no internet"). Open the app, tap **Set up a new hub**, type the
   code, choose your home network and its password.
3. The app invents a token for the hub (you never see it), the hub joins your network, and the phone goes back to
   your Wi-Fi. The app then connects to the hub with the certificate fingerprint it learned over the setup link.

A rebuild is needed after the app gained this (and `expo-location`, `expo-crypto`): `npx expo prebuild --platform ios`
then `npx expo run:ios --device` again. The Swift module has a new function, `connectLearning`, next to `connect`.
The first connection over the setup link is the only one that does not check a known fingerprint; if setup says the
hub's network cannot be reached, you are probably still on your home Wi-Fi.

## If it does not compile (or misbehaves)

Everything iOS-specific is in two places:

- `app/modules/keystone-net/ios/KeystoneNetModule.swift` — `PinnedSocket` (the pin check in
  `urlSession(_:didReceive:completionHandler:)`, the WebSocket events), `HubBrowser` (`NetServiceBrowser`
  discovery), and `KeystoneNetModule` (the JavaScript-facing functions and events).
- `app/modules/keystone-net/ios/KeystoneNet.podspec`.

The JavaScript contract is `app/src/native/types.ts`; the glue is `app/src/native/`. Assumptions most
likely to need a fix: exact Swift signatures against your Xcode's SDK (`SecTrustCopyCertificateChain`
needs iOS 15+, which is fine here), Swift 6 diagnostics promoted to errors, and `NetServiceBrowser` /
`NetService` deprecation warnings (warnings only). Send the first compiler error and the file/line.

Runtime hints: in Xcode's console and in the `npx expo start` terminal you will see JavaScript errors; the
native module reports a pin mismatch as an `error` event with `pinMismatch: true`, a bad URL or fingerprint
as an `error` followed by `close`. A pinned `wss://` connection that never opens usually means Local Network
permission is off, the phone is on another network than the hub, or the hub's server is not running
(`webtoken` has not been set).

## Not covered

Push notifications, background connections (the app disconnects when iOS suspends it, by design), App
Store review, Android (the Kotlin module is equally unverified; `eas build --profile development
--platform android` is the equivalent route).
