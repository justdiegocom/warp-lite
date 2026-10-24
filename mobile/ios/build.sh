#!/usr/bin/env bash
# Builds the iOS app (Xcode, macOS): the Rust core (mobile/core) for the Simulator or an iPhone, the Xcode project
# (XcodeGen, project.yml) and the app (arm64 only).
#   mobile/ios/build.sh sim                → build/sim/WarpRemote.app (Simulator)
#   mobile/ios/build.sh device <TEAM_ID>   → build/device/WarpRemote.app, development-signed with that team
#   mobile/ios/build.sh ipa                → build/WarpRemote.ipa, unsigned (for a sideloading tool that signs it)
#   mobile/ios/build.sh demo               → the app end to end in the booted Simulator against a real Warp, with real
#     taps (WarpRemoteUITests/DemoTests.swift): TEST_RUNNER_PAIR_TICKET=<code> TEST_RUNNER_CHAT_TAB=<tab>
#     [TEST_RUNNER_SHELL_TAB=<tab> TEST_RUNNER_REPLY=… TEST_RUNNER_SHOTS=<folder>]
# Needs Xcode, xcodegen and rustup's aarch64-apple-ios / aarch64-apple-ios-sim targets.
set -euo pipefail
cd "$(dirname "$0")"
kind=${1:-sim}
case "$kind" in
  sim | demo) target=aarch64-apple-ios-sim plat=iphonesimulator ;;
  device | ipa) target=aarch64-apple-ios plat=iphoneos ;;
  *) echo "usage: $0 sim | device <TEAM_ID> | ipa | demo" >&2; exit 2 ;;
esac
(cd ../core && cargo build --release --locked --target "$target")
mkdir -p "build/rust/$plat"
cp "${CARGO_TARGET_DIR:-../core/target}/$target/release/libwarp_remote_core.a" "build/rust/$plat/"
xcodegen generate --quiet
args=(-project WarpRemote.xcodeproj -scheme WarpRemote -configuration Release -sdk "$plat" -derivedDataPath build/dd ARCHS=arm64)
case "$kind" in
  sim) xcodebuild "${args[@]}" -destination 'generic/platform=iOS Simulator' CODE_SIGN_IDENTITY=- build -quiet ;;
  demo)
    booted=$(xcrun simctl list devices booted | sed -n 's/.*(\([0-9A-F-]\{36\}\)) (Booted).*/\1/p' | head -n 1)
    [[ -n "$booted" ]] || { echo "boot a Simulator first (xcrun simctl boot 'iPhone 17 Pro')" >&2; exit 1; }
    exec xcodebuild "${args[@]}" -destination "id=$booted" CODE_SIGN_IDENTITY=- -only-testing:WarpRemoteUITests/DemoTests test -quiet ;;
  device)
    team=${2:?team id: Xcode → Settings → Accounts}
    xcodebuild "${args[@]}" -destination 'generic/platform=iOS' -allowProvisioningUpdates -allowProvisioningDeviceRegistration \
      DEVELOPMENT_TEAM="$team" build -quiet ;;
  ipa) xcodebuild "${args[@]}" -destination 'generic/platform=iOS' CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO build -quiet ;;
esac
app="build/dd/Build/Products/Release-$plat/WarpRemote.app"
if [[ "$kind" == ipa ]]; then
  rm -rf build/ipa && mkdir -p build/ipa/Payload && cp -R "$app" build/ipa/Payload/
  (cd build/ipa && rm -f ../WarpRemote.ipa && zip -qry ../WarpRemote.ipa Payload)
  echo "build/WarpRemote.ipa"
else
  rm -rf "build/$kind" && mkdir -p "build/$kind" && cp -R "$app" "build/$kind/"
  echo "build/$kind/WarpRemote.app"
fi
