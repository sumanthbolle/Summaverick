#!/usr/bin/env python3
"""Generates SummaverickEveryday.xcodeproj without XcodeGen.

Deterministic: object IDs are derived from file paths, so re-running produces an identical project.
Run from the ios/ directory:  python3 make_xcodeproj.py
"""
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
os.chdir(ROOT)

APP = "SummaverickEveryday"
BUNDLE_ID = "com.summaverick.everyday"
SWIFT_DIR = "SummaverickEveryday"
CORE_SRC = "../core/src"
XCFRAMEWORK = "Frameworks/llama.xcframework"


def uid(key: str) -> str:
    return hashlib.md5(key.encode()).hexdigest()[:24].upper()


def q(s: str) -> str:
    """Quote a pbxproj string value when needed."""
    if s and all(c.isalnum() or c in "._/" for c in s) and not s.startswith("/"):
        return s
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


# ---- collect files -------------------------------------------------------
swift_files = []
for dirpath, _, files in os.walk(SWIFT_DIR):
    for f in sorted(files):
        if f.endswith(".swift"):
            swift_files.append(os.path.join(dirpath, f).replace("\\", "/"))
swift_files.sort()

xcstrings_files = []
for dirpath, _, files in os.walk(SWIFT_DIR):
    for f in files:
        if f.endswith(".xcstrings"):
            xcstrings_files.append(os.path.join(dirpath, f).replace("\\", "/"))
xcstrings_files.sort()

core_files = sorted(
    os.path.join(CORE_SRC, f) for f in os.listdir(CORE_SRC) if f.endswith((".cpp", ".h"))
)
if not any(f.endswith("sve_core.cpp") for f in core_files):
    sys.exit("core/src/sve_core.cpp not found; run this script from ios/")
if not os.path.isdir(XCFRAMEWORK):
    print("warning: Frameworks/llama.xcframework not found — run ./get-llama-xcframework.sh first", file=sys.stderr)

bridging = f"{SWIFT_DIR}/Bridging.h"
assets = f"{SWIFT_DIR}/Assets.xcassets"
info_plist = f"{SWIFT_DIR}/Info.plist"

# ---- objects -------------------------------------------------------------
objects = {}


def add(oid, isa, **kv):
    objects[oid] = (isa, kv)
    return oid


def file_ref(path, file_type, name=None):
    oid = uid("ref:" + path)
    kv = {"isa": "PBXFileReference", "lastKnownFileType": file_type, "path": path, "sourceTree": "SOURCE_ROOT"}
    if name:
        kv["name"] = name
    objects[oid] = ("PBXFileReference", kv)
    return oid


def build_file(ref, phase, settings=None):
    oid = uid("build:" + phase + ":" + ref)
    kv = {"isa": "PBXBuildFile", "fileRef": ref}
    if settings:
        kv["settings"] = settings
    objects[oid] = ("PBXBuildFile", kv)
    return oid


swift_refs = [file_ref(p, "sourcecode.swift", name=os.path.basename(p)) for p in swift_files]
core_refs = [
    file_ref(p, "sourcecode.cpp.cpp" if p.endswith(".cpp") else "sourcecode.c.h", name=os.path.basename(p))
    for p in core_files
]
bridging_ref = file_ref(bridging, "sourcecode.c.h", name="Bridging.h")
plist_ref = file_ref(info_plist, "text.plist.xml", name="Info.plist")
assets_ref = file_ref(assets, "folder.assetcatalog", name="Assets.xcassets")
assets_build = build_file(assets_ref, "resources")
xcstrings_refs = [file_ref(p, "text.json.xcstrings", name=os.path.basename(p)) for p in xcstrings_files]
xcstrings_builds = [build_file(r, "resources") for r in xcstrings_refs]
xcf_ref = file_ref(XCFRAMEWORK, "wrapper.xcframework", name="llama.xcframework")
product_ref = uid("ref:product")
objects[product_ref] = ("PBXFileReference", {
    "isa": "PBXFileReference", "explicitFileType": "wrapper.application", "includeInIndex": "0",
    "path": f"{APP}.app", "sourceTree": "BUILT_PRODUCTS_DIR"})

source_builds = [build_file(r, "sources") for r in swift_refs] + \
                [build_file(r, "sources") for r, p in zip(core_refs, core_files) if p.endswith(".cpp")]
link_build = build_file(xcf_ref, "frameworks")
embed_build = build_file(xcf_ref, "embed", settings={"ATTRIBUTES": ["CodeSignOnCopy", "RemoveHeadersOnCopy"]})

# groups (name-only groups; every file is referenced by its SOURCE_ROOT path, so nothing is in two groups)
def group(key, name, children, path=None):
    oid = uid("group:" + key)
    kv = {"isa": "PBXGroup", "children": children, "sourceTree": "<group>"}
    if name:
        kv["name"] = name
    if path:
        kv["path"] = path
    objects[oid] = ("PBXGroup", kv)
    return oid

# Swift sources grouped by subfolder for readability
by_dir = {}
for ref, p in zip(swift_refs, swift_files):
    d = os.path.dirname(p)[len(SWIFT_DIR):].strip("/") or "."
    by_dir.setdefault(d, []).append(ref)
swift_children = []
for d in sorted(by_dir):
    if d == ".":
        swift_children.extend(by_dir[d])
    else:
        swift_children.append(group("swift/" + d, d, by_dir[d]))
swift_children += [bridging_ref, plist_ref, assets_ref] + xcstrings_refs

app_group = group("app", APP, swift_children)
core_group = group("core", "Core", core_refs)
fw_group = group("frameworks", "Frameworks", [xcf_ref])
products_group = group("products", "Products", [product_ref])
main_group = group("main", None, [app_group, core_group, fw_group, products_group])

# build phases
sources_phase = uid("phase:sources")
objects[sources_phase] = ("PBXSourcesBuildPhase", {
    "isa": "PBXSourcesBuildPhase", "buildActionMask": "2147483647", "files": source_builds,
    "runOnlyForDeploymentPostprocessing": "0"})
frameworks_phase = uid("phase:frameworks")
objects[frameworks_phase] = ("PBXFrameworksBuildPhase", {
    "isa": "PBXFrameworksBuildPhase", "buildActionMask": "2147483647", "files": [link_build],
    "runOnlyForDeploymentPostprocessing": "0"})
resources_phase = uid("phase:resources")
objects[resources_phase] = ("PBXResourcesBuildPhase", {
    "isa": "PBXResourcesBuildPhase", "buildActionMask": "2147483647", "files": [assets_build] + xcstrings_builds,
    "runOnlyForDeploymentPostprocessing": "0"})
embed_phase = uid("phase:embed")
objects[embed_phase] = ("PBXCopyFilesBuildPhase", {
    "isa": "PBXCopyFilesBuildPhase", "buildActionMask": "2147483647", "dstPath": "",
    "dstSubfolderSpec": "10", "files": [embed_build], "name": "Embed Frameworks",
    "runOnlyForDeploymentPostprocessing": "0"})

# build settings
project_base = {
    "ALWAYS_SEARCH_USER_PATHS": "NO",
    "CLANG_ANALYZER_NONNULL": "YES",
    "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
    "CLANG_CXX_LIBRARY": "libc++",
    "CLANG_ENABLE_MODULES": "YES",
    "CLANG_ENABLE_OBJC_ARC": "YES",
    "CLANG_WARN_DOCUMENTATION_COMMENTS": "NO",
    "CLANG_WARN_QUOTED_INCLUDE_IN_FRAMEWORK_HEADER": "NO",
    "COPY_PHASE_STRIP": "NO",
    "ENABLE_STRICT_OBJC_MSGSEND": "YES",
    "ENABLE_USER_SCRIPT_SANDBOXING": "NO",
    "GCC_C_LANGUAGE_STANDARD": "gnu17",
    "GCC_NO_COMMON_BLOCKS": "YES",
    "IPHONEOS_DEPLOYMENT_TARGET": "17.0",
    "MTL_FAST_MATH": "YES",
    "SDKROOT": "iphoneos",
    "SWIFT_VERSION": "5.0",
    "TARGETED_DEVICE_FAMILY": "1,2",
    "CURRENT_PROJECT_VERSION": "1",
    "MARKETING_VERSION": "0.1.0",
}
project_debug = dict(project_base, **{
    "DEBUG_INFORMATION_FORMAT": "dwarf", "ENABLE_TESTABILITY": "YES", "GCC_OPTIMIZATION_LEVEL": "0",
    "GCC_PREPROCESSOR_DEFINITIONS": ["DEBUG=1", "$(inherited)"], "ONLY_ACTIVE_ARCH": "YES",
    "SWIFT_ACTIVE_COMPILATION_CONDITIONS": "DEBUG", "SWIFT_OPTIMIZATION_LEVEL": "-Onone",
    "MTL_ENABLE_DEBUG_INFO": "INCLUDE_SOURCE"})
project_release = dict(project_base, **{
    "DEBUG_INFORMATION_FORMAT": "dwarf-with-dsym", "ENABLE_NS_ASSERTIONS": "NO",
    "SWIFT_COMPILATION_MODE": "wholemodule", "SWIFT_OPTIMIZATION_LEVEL": "-O",
    "VALIDATE_PRODUCT": "YES", "MTL_ENABLE_DEBUG_INFO": "NO"})

target_settings = {
    "ASSETCATALOG_COMPILER_APPICON_NAME": "AppIcon",
    "ASSETCATALOG_COMPILER_GLOBAL_ACCENT_COLOR_NAME": "AccentColor",
    "CODE_SIGN_STYLE": "Automatic",
    "DEVELOPMENT_TEAM": "3JVSNB8RYM",
    "GENERATE_INFOPLIST_FILE": "NO",
    "INFOPLIST_FILE": info_plist,
    "CODE_SIGN_ENTITLEMENTS": f"{SWIFT_DIR}/SummaverickEveryday.entitlements",
    "HEADER_SEARCH_PATHS": ["$(inherited)", "$(SRCROOT)/../core/include", "$(SRCROOT)/../core/src"],
    "LD_RUNPATH_SEARCH_PATHS": ["$(inherited)", "@executable_path/Frameworks"],
    "OTHER_LDFLAGS": ["$(inherited)", "-lsqlite3"],
    "PRODUCT_BUNDLE_IDENTIFIER": BUNDLE_ID,
    "PRODUCT_NAME": "$(TARGET_NAME)",
    "SWIFT_EMIT_LOC_STRINGS": "YES",
    "SWIFT_OBJC_BRIDGING_HEADER": bridging,
    "SWIFT_OBJC_INTEROP_MODE": "objc",
    "ENABLE_PREVIEWS": "YES",
}


def config(key, name, settings):
    oid = uid("config:" + key)
    objects[oid] = ("XCBuildConfiguration", {"isa": "XCBuildConfiguration", "buildSettings": settings, "name": name})
    return oid


def config_list(key, configs):
    oid = uid("configlist:" + key)
    objects[oid] = ("XCConfigurationList", {
        "isa": "XCConfigurationList", "buildConfigurations": configs,
        "defaultConfigurationIsVisible": "0", "defaultConfigurationName": "Release"})
    return oid


proj_cl = config_list("project", [config("project/Debug", "Debug", project_debug),
                                  config("project/Release", "Release", project_release)])
target_cl = config_list("target", [config("target/Debug", "Debug", target_settings),
                                   config("target/Release", "Release", target_settings)])

target = uid("target:app")
objects[target] = ("PBXNativeTarget", {
    "isa": "PBXNativeTarget", "buildConfigurationList": target_cl,
    "buildPhases": [sources_phase, frameworks_phase, resources_phase, embed_phase],
    "buildRules": [], "dependencies": [], "name": APP, "productName": APP,
    "productReference": product_ref, "productType": "com.apple.product-type.application"})

project = uid("project")
objects[project] = ("PBXProject", {
    "isa": "PBXProject",
    "attributes": {"BuildIndependentTargetsInParallel": "1", "LastSwiftUpdateCheck": "1600",
                   "LastUpgradeCheck": "1600",
                   "TargetAttributes": {target: {"CreatedOnToolsVersion": "16.0"}}},
    "buildConfigurationList": proj_cl, "compatibilityVersion": "Xcode 14.0",
    "developmentRegion": "en", "hasScannedForEncodings": "0", "knownRegions": ["en", "Base"],
    "mainGroup": main_group, "productRefGroup": products_group, "projectDirPath": "",
    "projectRoot": "", "targets": [target]})

# ---- serialise -----------------------------------------------------------
def fmt(v, indent):
    pad = "\t" * indent
    if isinstance(v, dict):
        out = "{\n"
        for k in sorted(v, key=lambda k: (k != "isa", k)):
            out += f"{pad}\t{q(k)} = {fmt(v[k], indent + 1)};\n"
        return out + pad + "}"
    if isinstance(v, list):
        out = "(\n"
        for item in v:
            out += f"{pad}\t{fmt(item, indent + 1)},\n"
        return out + pad + ")"
    return q(str(v))


lines = ["// !$*UTF8*$!", "{", "\tarchiveVersion = 1;", "\tclasses = {", "\t};", "\tobjectVersion = 56;", "\tobjects = {"]
for isa in sorted({isa for isa, _ in objects.values()}):
    lines.append(f"\n/* Begin {isa} section */")
    for oid in sorted(o for o, (i, _) in objects.items() if i == isa):
        lines.append(f"\t\t{oid} = {fmt(objects[oid][1], 2)};")
    lines.append(f"/* End {isa} section */")
lines += ["\t};", f"\trootObject = {project};", "}", ""]

os.makedirs(f"{APP}.xcodeproj", exist_ok=True)
with open(f"{APP}.xcodeproj/project.pbxproj", "w") as f:
    f.write("\n".join(lines))
print(f"Created {APP}.xcodeproj ({len(swift_files)} Swift files, {sum(p.endswith('.cpp') for p in core_files)} C++ files)")
