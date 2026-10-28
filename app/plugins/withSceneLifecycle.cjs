// Xcode 27 / iOS 27 refuse to launch an app that does not adopt the UIKit scene lifecycle:
// "Application failed to launch: UIScene lifecycle is required for apps built with this SDK".
// The Expo template's AppDelegate is window-based (ExpoAppDelegate conforms to UIApplicationDelegate
// only), so `expo prebuild` writes one that iOS then kills on launch.
//
// This plugin restores the scene-aware AppDelegate after every prebuild. The Swift source lives
// next to this file; `ios/` is generated and gitignored, so this is the only durable place for it.
const fs = require('fs');
const path = require('path');
const { withAppDelegate } = require('@expo/config-plugins');

const TEMPLATE = path.join(__dirname, 'AppDelegate.swift.tmpl');

/** @type {import('@expo/config-plugins').ConfigPlugin} */
function withSceneLifecycle(config) {
  return withAppDelegate(config, (cfg) => {
    const source = fs.readFileSync(TEMPLATE, 'utf8');
    const pathToDelegate = cfg.modResults.path;
    // Only touch the Swift delegate; the Objective-C one has a different shape.
    if (!pathToDelegate.endsWith('.swift')) {
      throw new Error(`withSceneLifecycle: expected a Swift AppDelegate, got ${pathToDelegate}`);
    }
    if (cfg.modResults.contents !== source) {
      fs.writeFileSync(pathToDelegate, source);
      cfg.modResults.contents = source;
    }
    return cfg;
  });
}

module.exports = withSceneLifecycle;