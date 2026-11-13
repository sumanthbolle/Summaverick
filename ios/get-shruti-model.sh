#!/usr/bin/env bash
# Puts the five Shruti Telugu speech files (udgamlabs, Apache-2.0) into SummaverickEveryday/Resources/Shruti/.
# Every file's size and SHA-256 is checked; a mismatch stops the script and removes the bad file.
#   ./get-shruti-model.sh                 download from Hugging Face (udgamlabs/Shruti-ONNX, te/sherpa-onnx/)
#   ./get-shruti-model.sh --from <dir>    copy from a local folder that already holds the five files
set -euo pipefail
cd "$(dirname "$0")"

DEST="SummaverickEveryday/Resources/Shruti"
BASE="https://huggingface.co/udgamlabs/Shruti-ONNX/resolve/main/te/sherpa-onnx"
FROM=""
if [[ "${1:-}" == "--from" ]]; then
  FROM="${2:-}"
  [[ -d "$FROM" ]] || { echo "ERROR: --from needs a folder"; exit 1; }
elif [[ $# -ne 0 ]]; then
  echo "usage: $0 [--from <dir>]"; exit 1
fi

# name bytes sha256
FILES=(
  "preprocess.onnx 912310 859b1bac985a6c0a9bff5b0ee520779e5ed14adbdcbe9188c664d3e918fb2203"
  "encode.onnx 19637087 2bd36f1170a42b0a842c81a1e8118811b166488a686223d0a195b57e7fb71642"
  "uncached_decode.onnx 421454 bb50aaeffa579692be00448ec51ba24b73589864fe7e2c42cf3f1c6cf991b510"
  "cached_decode.onnx 32801767 695e071dc484120457cec7cabf6d78addafe7b7d63ba64301af3a278c3744701"
  "tokens.txt 120752 aead59cb98514cdc031c51ca0ed3dd2375e85fe81c05844bdbbf4f4b20505e32"
)

mkdir -p "$DEST"
for entry in "${FILES[@]}"; do
  read -r name bytes sha <<<"$entry"
  out="$DEST/$name"
  if [[ -n "$FROM" ]]; then
    cp "$FROM/$name" "$out"
  else
    curl -fL --progress-bar -o "$out" "$BASE/$name"
  fi
  got_bytes=$(wc -c < "$out" | tr -d ' ')
  got_sha=$(shasum -a 256 "$out" | cut -d' ' -f1)
  if [[ "$got_bytes" != "$bytes" || "$got_sha" != "$sha" ]]; then
    rm -f "$out"
    echo "ERROR: $name: got $got_bytes bytes, sha256 $got_sha; want $bytes bytes, sha256 $sha"
    exit 1
  fi
  echo "ok $name"
done
echo "OK: $DEST. Now: python3 make_xcodeproj.py"
