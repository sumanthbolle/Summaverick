#!/usr/bin/env bash
# Downloads the prebuilt llama.xcframework from a llama.cpp GitHub release into ios/Frameworks/.
#   ./get-llama-xcframework.sh          # latest release
#   ./get-llama-xcframework.sh b12119   # a specific build tag
#
# Manual alternative: open https://github.com/ggml-org/llama.cpp/releases, pick the newest build whose
# tag looks like "b12119" (not v0.9.0), download "llama-b12119-xcframework.zip", unzip it, and move the llama.xcframework
# folder to ios/Frameworks/llama.xcframework.
set -euo pipefail
cd "$(dirname "$0")"

TAG="${1:-}"
if [[ -z "$TAG" ]]; then
  # llama.cpp publishes the xcframework on its per-commit builds (tags like b12119), not on the
  # "stable" vX.Y.Z releases that GitHub marks as "latest". So we look for the newest b-tag via git.
  echo "Looking up the newest llama.cpp build tag…"
  TAG=$(git ls-remote --tags https://github.com/ggml-org/llama.cpp.git 2>/dev/null \
        | grep -v '\^{}' | sed 's#.*refs/tags/##' | grep -E '^b[0-9]+$' | sort -V | tail -1 || true)
fi
if [[ -z "$TAG" ]]; then
  echo "ERROR: could not determine the newest build tag. Pass one explicitly, e.g.:"
  echo "       ./get-llama-xcframework.sh b12119   (see https://github.com/ggml-org/llama.cpp/releases)"
  exit 1
fi

URL="https://github.com/ggml-org/llama.cpp/releases/download/${TAG}/llama-${TAG}-xcframework.zip"
echo "Release: $TAG"
echo "Downloading $URL"
mkdir -p Frameworks
TMP=$(mktemp -d)
if ! curl -fL --progress-bar -o "$TMP/llama.zip" "$URL"; then
  echo "Asset not found for $TAG (that build may still be running). Trying earlier builds…"
  OK=""
  N=${TAG#b}
  for i in 1 2 3 4 5 6 7 8; do
    T="b$((N - i))"
    U="https://github.com/ggml-org/llama.cpp/releases/download/${T}/llama-${T}-xcframework.zip"
    if curl -fL --progress-bar -o "$TMP/llama.zip" "$U" 2>/dev/null; then TAG="$T"; OK=1; echo "Got $T"; break; fi
  done
  if [[ -z "$OK" ]]; then
    echo "ERROR: download failed. Download it manually (see top of this script)."
    exit 1
  fi
fi
rm -rf Frameworks/llama.xcframework
unzip -q "$TMP/llama.zip" -d "$TMP"
FOUND=$(find "$TMP" -type d -name 'llama.xcframework' | head -1)
if [[ -z "$FOUND" ]]; then
  echo "ERROR: the zip did not contain llama.xcframework"
  exit 1
fi
mv "$FOUND" Frameworks/llama.xcframework
rm -rf "$TMP"
echo "$TAG" > Frameworks/llama.version
echo "OK: Frameworks/llama.xcframework ($TAG) is in place. Now: open SummaverickEveryday.xcodeproj"
