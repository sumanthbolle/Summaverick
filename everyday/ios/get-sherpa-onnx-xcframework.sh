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
echo "OK: Frameworks/SherpaOnnxC.xcframework (sherpa-onnx 2.0.1). Now: python3 make_xcodeproj.py"
