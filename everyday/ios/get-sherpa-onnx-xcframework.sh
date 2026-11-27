#!/usr/bin/env bash
# Downloads sherpa-onnx's prebuilt iOS framework (onnxruntime linked in statically) into
# ios/Frameworks/SherpaOnnxC.xcframework, for the Shruti speech recognizer.
# sherpa-onnx (k2-fsa, Apache-2.0) publishes iOS builds under its "xcframework" release tag, not under v2.0.1 itself.
# 2.0.1 is the version the Shruti iOS sample (udgamlabs/shruti onnx/samples/ios) was written against.
set -euo pipefail
cd "$(dirname "$0")"

NAME="sherpa-onnx-v2.0.1-ios-shared-onnxruntime-static.xcframework.zip"
SHA256="fc4c665e694322ced5393aafafaae6bb3e0a7449d98d5db60bc220ac7ac4cbc9"
URL="https://github.com/k2-fsa/sherpa-onnx/releases/download/xcframework/${NAME}"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
curl -fL --progress-bar -o "$TMP/sherpa.zip" "$URL"
GOT=$(shasum -a 256 "$TMP/sherpa.zip" | cut -d' ' -f1)
if [[ "$GOT" != "$SHA256" ]]; then
  echo "ERROR: checksum mismatch: got $GOT, want $SHA256"
  exit 1
fi
mkdir -p "$TMP/x"
unzip -q "$TMP/sherpa.zip" -d "$TMP/x"
mkdir -p Frameworks
rm -rf Frameworks/SherpaOnnxC.xcframework
mv "$TMP/x/SherpaOnnxC.xcframework" Frameworks/SherpaOnnxC.xcframework
# sherpa-onnx ships no privacy manifest, and its library calls stat/fstat (Apple's "file timestamp" required-reason
# API) on the speech model files in the app. App Store submission needs the reason declared in the framework itself.
for fw in Frameworks/SherpaOnnxC.xcframework/*/SherpaOnnxC.framework; do
  cat > "$fw/PrivacyInfo.xcprivacy" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>NSPrivacyTracking</key>
	<false/>
	<key>NSPrivacyTrackingDomains</key>
	<array/>
	<key>NSPrivacyCollectedDataTypes</key>
	<array/>
	<key>NSPrivacyAccessedAPITypes</key>
	<array>
		<dict>
			<key>NSPrivacyAccessedAPIType</key>
			<string>NSPrivacyAccessedAPICategoryFileTimestamp</string>
			<key>NSPrivacyAccessedAPITypeReasons</key>
			<array>
				<string>C617.1</string>
			</array>
		</dict>
	</array>
</dict>
</plist>
PLIST
done
echo "OK: Frameworks/SherpaOnnxC.xcframework (sherpa-onnx 2.0.1). Now: python3 make_xcodeproj.py"
