#!/usr/bin/env bash
# Release build of the Keystone app.
#
#   app/scripts/ios-release.sh              # device (arm64) — needs your signing
#   app/scripts/ios-release.sh simulator    # simulator — no signing, runs in Simulator
#
# What it does:
#   1. patches React Native's prebuilt-dependencies podspec (its cleanup step does a
#      bulk rm inside the CocoaPods cache, which fails on sandboxed/CI hosts),
#   2. regenerates the native project (expo prebuild) — app/ios/ is generated, not committed,
#   3. installs the pods (CocoaPods cache and Expo home are kept inside build/),
#   4. builds the Release configuration and archives it.
#
# Signing: the project is set to automatic signing with team 5H749MZFPN and the bundle id
# com.kliuchnikovv.keystone. Xcode renews the provisioning profile when you are signed in
# to your Apple ID (Xcode ▸ Settings ▸ Accounts). The profile on this machine expired on
# 2026-10-24, so sign in before building for a device.

set -euo pipefail

# Optional: wipe the global Clang module cache first. Use when a build fails with
# "pcm is not a valid module file: file too small to contain precompiled file magic".
if [ "${CLEAN:-0}" = "1" ]; then
  rm -rf "$HOME/Library/Developer/Xcode/DerivedData/ModuleCache.noindex"
fi

DESTINATION_KIND="${1:-device}"
APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_DIR="$(cd "$APP_DIR/.." && pwd)"
WORK_DIR="$REPO_DIR/build/ios-release"
PODSPEC="$APP_DIR/node_modules/react-native/third-party-podspecs/ReactNativeDependencies.podspec"

mkdir -p "$WORK_DIR" "$WORK_DIR/cp-home" "$WORK_DIR/expo-home"

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }

step "1/4 React Native podspec"
if [ -f "$PODSPEC" ]; then
  python3 - "$PODSPEC" <<'PY'
import sys
from pathlib import Path
p = Path(sys.argv[1])
s = p.read_text()
old = """    find "$XCFRAMEWORK_PATH/.." -type f -exec rm {} +
    find "$CURRENT_PATH" -type d -empty -delete"""
new = """    # Cleanup skipped on purpose: sandboxed hosts refuse deletions inside the
    # CocoaPods cache, and the extra copies are harmless.
    # find "$XCFRAMEWORK_PATH/.." -type f -exec rm {} +
    # find "$CURRENT_PATH" -type d -empty -delete"""
if old in s:
    p.write_text(s.replace(old, new))
    print("patched")
else:
    print("already patched")
PY
else
  echo "podspec not found (run npm install first)"
fi

step "2/4 expo prebuild"
(cd "$APP_DIR" && EXPO_HOME="$WORK_DIR/expo-home" npx expo prebuild --platform ios --no-install)

step "3/4 pod install"
(cd "$APP_DIR/ios" && CP_HOME_DIR="$WORK_DIR/cp-home" pod install --no-repo-update)

step "4/4 xcodebuild ($DESTINATION_KIND)"
if [ "$DESTINATION_KIND" = "simulator" ]; then
  DESTINATION='generic/platform=iOS Simulator'
  ARCHIVE="$WORK_DIR/Keystone-simulator.xcarchive"
else
  DESTINATION='generic/platform=iOS'
  ARCHIVE="$WORK_DIR/Keystone.xcarchive"
fi

(cd "$APP_DIR/ios" && xcodebuild \
  -workspace Keystone.xcworkspace \
  -scheme Keystone \
  -configuration Release \
  -destination "$DESTINATION" \
  -derivedDataPath "$WORK_DIR/DerivedData" \
  -archivePath "$ARCHIVE" \
  -allowProvisioningUpdates \
  archive)

echo
echo "Archive: $ARCHIVE"
echo "App:     $ARCHIVE/Products/Applications/Keystone.app"
