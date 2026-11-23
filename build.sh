#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

TARGET_TRIPLE="x86_64-pc-windows-msvc"
BUILD_DIR="$SCRIPT_DIR/build"
GENERATED_DIR="$BUILD_DIR/generated"
WINSDK_BASE="/opt/winsdk"
VULKAN_DIR="/opt/VulkanSDK/1.4.313.2"
OUTPUT_RELEASE="$SCRIPT_DIR/simp.exe"
VERSION_STATE_FILE="$SCRIPT_DIR/version"
VERSION_HEADER_FILE="$GENERATED_DIR/version.generated.h"
BASE_VERSION_MAJOR=0
BASE_VERSION_MINOR=1
CURRENT_APP_VERSION=""
CURRENT_BUILD_COUNT=""
MAX_JOBS="$(( $(command -v nproc >/dev/null 2>&1 && nproc || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1) ))"
COMPILE_JOBS="${SIMP_COMPILE_JOBS:-$MAX_JOBS}"

if ! [[ "$COMPILE_JOBS" =~ ^[0-9]+$ ]] || [[ "$COMPILE_JOBS" -lt 1 ]]; then
    COMPILE_JOBS=1
fi

die() {
    printf '%s\n' "$*" >&2
    exit 1
}

need_tool() {
    command -v "$1" >/dev/null 2>&1 || die "missing required tool: $1"
}

clean() {
    rm -rf "$BUILD_DIR" "$OUTPUT_RELEASE"
}

detect_mode() {
    case "${1:-release}" in
        "") BUILD_MODE="release" ;;
        release) BUILD_MODE="release" ;;
        debug) BUILD_MODE="debug" ;;
        clean) BUILD_MODE="clean" ;;
        *) die "usage: ./build.sh [release|debug|clean]" ;;
    esac
}

check_environment() {
    for tool in clang++ lld-link x86_64-w64-mingw32-windres glslangValidator python3 ffmpeg ffprobe; do
        need_tool "$tool"
    done

    [[ -d "$WINSDK_BASE/crt/include" ]] || die "missing Windows CRT headers under $WINSDK_BASE"
    [[ -d "$WINSDK_BASE/sdk/include/ucrt" ]] || die "missing Windows SDK headers under $WINSDK_BASE"
    [[ -d "$WINSDK_BASE/crt/lib/x86_64" ]] || die "missing Windows CRT libraries under $WINSDK_BASE"
    [[ -d "$WINSDK_BASE/sdk/lib/ucrt/x86_64" ]] || die "missing Windows UCRT libraries under $WINSDK_BASE"
    [[ -d "$WINSDK_BASE/sdk/lib/um/x86_64" ]] || die "missing Windows UM libraries under $WINSDK_BASE"
    [[ -d "$VULKAN_DIR/Include" ]] || die "missing Vulkan SDK includes under $VULKAN_DIR"
    [[ -f "$VULKAN_DIR/Lib/vulkan-1.lib" ]] || die "missing Vulkan import library under $VULKAN_DIR"

}

prepare_version_metadata() {
    local today previous_version
    today="$(date +%Y%m%d)"
    CURRENT_BUILD_COUNT=1
    if [[ -f "$VERSION_STATE_FILE" ]]; then
        previous_version="$(tr -d '\r\n' < "$VERSION_STATE_FILE")"
        if [[ "$previous_version" =~ ^${BASE_VERSION_MAJOR}\.${BASE_VERSION_MINOR}\.([0-9]{8})\.([0-9]+)$ ]]; then
            if [[ "${BASH_REMATCH[1]}" == "$today" ]]; then
                CURRENT_BUILD_COUNT=$((10#${BASH_REMATCH[2]} + 1))
            fi
        else
            printf 'Ignoring unreadable version state: %s\n' "$previous_version" >&2
        fi
    fi
    CURRENT_APP_VERSION="${BASE_VERSION_MAJOR}.${BASE_VERSION_MINOR}.$today.$CURRENT_BUILD_COUNT"
    python3 "$SCRIPT_DIR/tools/generate_version.py" "$VERSION_HEADER_FILE" \
        "$BASE_VERSION_MAJOR" "$BASE_VERSION_MINOR" "$today" "$CURRENT_BUILD_COUNT" "$BUILD_MODE"
}

compile_resources() {

    x86_64-w64-mingw32-windres \
        -O coff \
        -I "$SCRIPT_DIR" \
        -I "$WINSDK_BASE/crt/include" \
        -I "$WINSDK_BASE/sdk/include/ucrt" \
        -I "$WINSDK_BASE/sdk/include/um" \
        -I "$WINSDK_BASE/sdk/include/shared" \
        -i "$SCRIPT_DIR/resource.rc" \
        -o "$BUILD_DIR/resource.res"

}

compile_shaders() {
    local shader
    for shader in ui.vert ui.frag paint.comp canvas.vert canvas.frag; do
        glslangValidator -V --target-env vulkan1.1 "$SCRIPT_DIR/shaders/$shader" -o "$GENERATED_DIR/$shader.spv"
    done
    python3 "$SCRIPT_DIR/tools/embed_shaders.py" "$GENERATED_DIR"
}

compile_sources() {
    local -n object_files_ref=$1
    local clang_intrinsics_dir
    clang_intrinsics_dir="$(clang++ -print-resource-dir)/include"

    local build_flags=(
        "--target=$TARGET_TRIPLE"
        "-fuse-ld=lld-link"
        "-std=c++20"
        "-fexceptions"
        "-fcxx-exceptions"
        "-fms-compatibility"
        "-fms-compatibility-version=19.37"
        "-D_MT"
        "-DUNICODE"
        "-D_UNICODE"
        "-DNOMINMAX"
        "-isystem" "/opt/msvc/include"
        "-isystem" "$WINSDK_BASE/crt/include"
        "-isystem" "$WINSDK_BASE/sdk/include/ucrt"
        "-isystem" "$WINSDK_BASE/sdk/include/um"
        "-isystem" "$WINSDK_BASE/sdk/include/shared"
        "-I$GENERATED_DIR"
        "-I$SCRIPT_DIR/include"
        "-I$VULKAN_DIR/Include"
        "-Wall"
        "-Wextra"
        "-Wpedantic"
        "-Wno-c++98-compat"
        "-Wno-c++98-compat-pedantic"
        "-Wno-language-extension-token"
        "-Wno-missing-field-initializers"
        "-Wno-unknown-attributes"
        "-Wno-unused-parameter"
        "-Wno-unused-command-line-argument"
        "-Wno-zero-as-null-pointer-constant"
    )

    if [[ "$BUILD_MODE" == "debug" ]]; then
        build_flags+=("-O0" "-gcodeview" "-DSIMP_DEBUG")
    else
        build_flags+=("-O2" "-DNDEBUG")
    fi

    local sources=("$SCRIPT_DIR"/src/*.cpp)
    [[ -f "${sources[0]}" ]] || die "no source files found under src"

    local start index source base object pid failed
    local -a pids source_flags logs
    local compile_log_dir
    compile_log_dir="$(mktemp -d "$BUILD_DIR/compile_logs.XXXXXX")"
    for ((start = 0; start < ${#sources[@]}; start += COMPILE_JOBS)); do
        pids=()
        logs=()
        for source in "${sources[@]:start:COMPILE_JOBS}"; do
            base=${source##*/}
            base=${base%.cpp}
            object="$BUILD_DIR/$base.obj"
            source_flags=()
            if [[ "$base" == stb_image_impl || "$base" == cpu_pixels ]]; then
                # Use Clang's definitions for the SIMD intrinsics.
                source_flags=(-nostdinc "-I$clang_intrinsics_dir" -Wno-c99-extensions -Wno-microsoft-redeclare-static)
            fi
            object_files_ref+=("$object")
            logs+=("$compile_log_dir/$base.log")
            clang++ "${build_flags[@]}" "${source_flags[@]}" -c "$source" -o "$object" >"${logs[-1]}" 2>&1 &
            pids+=("$!")
        done
        failed=0
        for index in "${!pids[@]}"; do
            pid=${pids[index]}
            if ! wait "$pid"; then
                printf 'Compilation failed for %s\n' "${sources[start + index]##*/}" >&2
                failed=1
            fi
            if [[ -s "${logs[index]}" ]]; then
                cat "${logs[index]}"
            fi
            rm -f "${logs[index]}"
        done
        if (( failed != 0 )); then
            rmdir "$compile_log_dir"
            return 1
        fi
    done
    rmdir "$compile_log_dir"
}

link_executable() {
    local output_file="$1"
    shift

    local link_flags=(
        "--target=$TARGET_TRIPLE"
        "-fuse-ld=lld-link"
        "-Xlinker" "/machine:x64"
        "-Xlinker" "/subsystem:windows"
        "-Xlinker" "/incremental:no"
        "-Xlinker" "/implib:$BUILD_DIR/simp-$BUILD_MODE.lib"
        "-Xlinker" "/threads:$MAX_JOBS"
        "-Xlinker" "/libpath:$WINSDK_BASE/crt/lib/x86_64"
        "-Xlinker" "/libpath:$WINSDK_BASE/sdk/lib/ucrt/x86_64"
        "-Xlinker" "/libpath:$WINSDK_BASE/sdk/lib/um/x86_64"
        "-Xlinker" "/libpath:$VULKAN_DIR/Lib"
    )

    if [[ "$BUILD_MODE" == "release" ]]; then
        link_flags+=("-Xlinker" "/opt:ref" "-Xlinker" "/opt:icf")
    fi

    clang++ \
        "${link_flags[@]}" \
        "$@" \
        "$BUILD_DIR/resource.res" \
        -o "$output_file" \
        "$WINSDK_BASE/crt/lib/x86_64/libcmt.lib" \
        "$WINSDK_BASE/crt/lib/x86_64/libvcruntime.lib" \
        "$WINSDK_BASE/crt/lib/x86_64/libcpmt.lib" \
        "$WINSDK_BASE/crt/lib/x86_64/oldnames.lib" \
        "$WINSDK_BASE/sdk/lib/ucrt/x86_64/libucrt.lib" \
        "$WINSDK_BASE/sdk/lib/ucrt/x86_64/ucrt.lib" \
        "$WINSDK_BASE/sdk/lib/um/x86_64/user32.lib" \
        "$WINSDK_BASE/sdk/lib/um/x86_64/comdlg32.lib" \
        "$WINSDK_BASE/sdk/lib/um/x86_64/comctl32.lib" \
        "$WINSDK_BASE/sdk/lib/um/x86_64/shell32.lib" \
        "$WINSDK_BASE/sdk/lib/um/x86_64/gdi32.lib" \
        "$WINSDK_BASE/sdk/lib/um/x86_64/msimg32.lib" \
        "$WINSDK_BASE/sdk/lib/um/x86_64/advapi32.lib" \
        "$WINSDK_BASE/sdk/lib/um/x86_64/ole32.lib" \
        "$WINSDK_BASE/sdk/lib/um/x86_64/shlwapi.lib" \
        "$WINSDK_BASE/sdk/lib/um/x86_64/mfuuid.lib" \
        "$WINSDK_BASE/sdk/lib/um/x86_64/windowscodecs.lib" \
        "$WINSDK_BASE/sdk/lib/um/x86_64/dwmapi.lib" \
        "$WINSDK_BASE/sdk/lib/um/x86_64/uxtheme.lib" \
        "$VULKAN_DIR/Lib/vulkan-1.lib"

    python3 "$SCRIPT_DIR/tools/verify_build.py" "$output_file" "$CURRENT_APP_VERSION" "$BUILD_MODE"
}

main() {
    detect_mode "${1:-release}"
    if [[ "$BUILD_MODE" == clean ]]; then
        clean
        return
    fi

    check_environment
    mkdir -p "$GENERATED_DIR"
    prepare_version_metadata
    python3 "$SCRIPT_DIR/tools/encode_splash.py" "$GENERATED_DIR"
    compile_shaders
    compile_resources
    local object_files=()
    printf 'Compiling SIMP (%s, %s jobs)\n' "$BUILD_MODE" "$COMPILE_JOBS"
    compile_sources object_files
    link_executable "$OUTPUT_RELEASE" "${object_files[@]}"
    printf '%s\n' "$CURRENT_APP_VERSION" > "$VERSION_STATE_FILE"
    printf 'Built %s (%s)\n' "$OUTPUT_RELEASE" "$CURRENT_APP_VERSION"
}

main "$@"
