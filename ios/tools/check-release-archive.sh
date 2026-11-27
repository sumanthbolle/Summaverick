#!/usr/bin/env bash
# Checks a signed Release build before it goes to TestFlight / App Store review:
#   - privacy manifests in the app, the widget extension and SherpaOnnxC.framework (a framework fetched before
#     get-sherpa-onnx-xcframework.sh added its manifest has none: run that script again),
#   - each manifest is a valid plist that collects and tracks nothing,
#   - the app and the widget are both signed with the App Group the record control reads.
# Usage: tools/check-release-archive.sh <Summaverick.xcarchive | SummaverickEveryday.app | .ipa>
set -uo pipefail
GROUP="group.com.summaverick.everyday"
in="${1:?usage: $0 <xcarchive|app|ipa>}"
tmp=""
case "$in" in
  *.xcarchive) app=$(ls -d "$in"/Products/Applications/*.app 2>/dev/null | head -1) ;;
  *.ipa) tmp=$(mktemp -d); unzip -q "$in" -d "$tmp"; app=$(ls -d "$tmp"/Payload/*.app | head -1) ;;
  *.app) app="$in" ;;
  *) echo "not an .xcarchive, .app or .ipa: $in"; exit 2 ;;
esac
trap '[[ -n "$tmp" ]] && rm -rf "$tmp"' EXIT
[[ -d "${app:-}" ]] || { echo "FAIL no app found in $in"; exit 1; }
widget="$app/PlugIns/SummaverickWidgets.appex"
fails=0
ok()   { echo "ok    $1"; }
fail() { echo "FAIL  $1"; fails=$((fails + 1)); }

for b in "$app" "$widget" "$app/Frameworks/SherpaOnnxC.framework"; do
  name=${b#"$app"}; name=${name:-/ (app)}
  m="$b/PrivacyInfo.xcprivacy"
  if [[ ! -f "$m" ]]; then fail "privacy manifest missing: $name"; continue; fi
  if ! plutil -lint -s "$m"; then fail "privacy manifest not a valid plist: $name"; continue; fi
  tracking=$(plutil -extract NSPrivacyTracking raw "$m" 2>/dev/null)
  collected=$(plutil -extract NSPrivacyCollectedDataTypes raw "$m" 2>/dev/null)
  if [[ "$tracking" == "false" && "$collected" == "0" ]]; then ok "privacy manifest: $name"
  else fail "privacy manifest says tracking=$tracking, collected types=$collected: $name"; fi
done

for b in "$app" "$widget"; do
  name=$(basename "$b")
  ent=$(codesign -d --entitlements - --xml "$b" 2>/dev/null)
  if [[ -z "$ent" ]]; then fail "not signed (or no entitlements): $name"; continue; fi
  if grep -q "$GROUP" <<<"$ent"; then ok "signed with $GROUP: $name"
  else fail "signed without $GROUP (record control cannot show the state): $name"; fi
  if grep -q '<key>get-task-allow</key><true/>' <<<"$ent"; then fail "development signing (get-task-allow): $name"; fi
done

[[ -f "$app/Shruti/encode.onnx" || -n $(find "$app" -path '*Shruti*' -name '*.onnx' -print -quit) ]] \
  && ok "Telugu speech model bundled" || fail "Telugu speech model missing (run ./get-shruti-model.sh)"

if (( fails )); then echo "$fails problem(s)"; exit 1; fi
echo "all checks passed"
