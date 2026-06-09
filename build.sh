#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

PROJECT_NAME="SIMP"
TARGET_TRIPLE="x86_64-pc-windows-msvc"
BUILD_DIR="$SCRIPT_DIR/build"
GENERATED_DIR="$BUILD_DIR/generated"
WINSDK_BASE="/opt/winsdk"
VULKAN_DIR="/opt/VulkanSDK/1.4.313.2"
OUTPUT_RELEASE="$SCRIPT_DIR/simp.exe"
OUTPUT_DEBUG="$SCRIPT_DIR/simp-debug.exe"
VERSION_STATE_FILE="$SCRIPT_DIR/version"
VERSION_HEADER_FILE="$GENERATED_DIR/version.generated.h"
BASE_VERSION_MAJOR=0
BASE_VERSION_MINOR=1
CURRENT_APP_VERSION=""
CURRENT_BUILD_DATE=""
CURRENT_BUILD_COUNT=""
MAX_JOBS="$(( $(command -v nproc >/dev/null 2>&1 && nproc || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1) ))"
COMPILE_JOBS="${SIMP_COMPILE_JOBS:-$MAX_JOBS}"

if ! [[ "$COMPILE_JOBS" =~ ^[0-9]+$ ]] || [[ "$COMPILE_JOBS" -lt 1 ]]; then
    COMPILE_JOBS=1
fi

if [[ -t 1 ]] && command -v tput >/dev/null 2>&1 && [[ $(tput colors 2>/dev/null || echo 0) -ge 8 ]]; then
    COLOR_RESET="\033[0m"
    COLOR_BOLD="\033[1m"
    COLOR_DIM="\033[2m"
    COLOR_RED="\033[31m"
    COLOR_GREEN="\033[32m"
    COLOR_YELLOW="\033[33m"
    COLOR_BLUE="\033[34m"
    COLOR_MAGENTA="\033[35m"
    COLOR_CYAN="\033[36m"
else
    COLOR_RESET=""
    COLOR_BOLD=""
    COLOR_DIM=""
    COLOR_RED=""
    COLOR_GREEN=""
    COLOR_YELLOW=""
    COLOR_BLUE=""
    COLOR_MAGENTA=""
    COLOR_CYAN=""
fi

if [[ "${LANG:-}" =~ UTF-8 ]] || [[ "${LC_ALL:-}" =~ UTF-8 ]]; then
    EMOJI_SUCCESS="✅"
    EMOJI_CACHED="⚡"
    EMOJI_FAILED="❌"
    EMOJI_WARNING="⚠️"
    EMOJI_ROCKET="🚀"
    EMOJI_WRENCH="🔧"
    EMOJI_PACKAGE="📦"
    EMOJI_FIRE="🔥"
else
    EMOJI_SUCCESS="[OK]"
    EMOJI_CACHED="[==]"
    EMOJI_FAILED="[!!]"
    EMOJI_WARNING="[!]"
    EMOJI_ROCKET=">>>"
    EMOJI_WRENCH="[*]"
    EMOJI_PACKAGE="[+]"
    EMOJI_FIRE="[X]"
fi

banner() {
    local width=78
    local line
    line="$(printf '%*s' "$width" | tr ' ' '=')"
    printf "\n${COLOR_BOLD}${COLOR_CYAN}%s\n" "$line"
    printf "  %s\n" "$1"
    printf "%s${COLOR_RESET}\n\n" "$line"
}

info() {
    printf "${COLOR_DIM}%s${COLOR_RESET}\n" "$1"
}

step() {
    printf "${COLOR_BLUE}%s${COLOR_RESET}\n" "$1"
}

success() {
    printf "${COLOR_GREEN}%s %s${COLOR_RESET}\n" "$EMOJI_SUCCESS" "$1"
}

warn() {
    printf "${COLOR_YELLOW}%s %s${COLOR_RESET}\n" "$EMOJI_WARNING" "$1"
}

fail() {
    printf "${COLOR_RED}%s %s${COLOR_RESET}\n" "$EMOJI_FAILED" "$1" >&2
}

die() {
    fail "$*"
    exit 1
}

need_tool() {
    command -v "$1" >/dev/null 2>&1 || die "missing required tool: $1"
}

clean() {
    banner "${EMOJI_FIRE} Cleaning Build Artifacts"
    rm -rf "$BUILD_DIR" "$OUTPUT_RELEASE" "$OUTPUT_DEBUG"
    success "Removed build outputs"
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
    banner "${EMOJI_WRENCH} Checking Toolchain"

    need_tool clang++
    need_tool lld-link
    need_tool x86_64-w64-mingw32-windres

    success "Found clang++"
    success "Found lld-link"
    success "Found x86_64-w64-mingw32-windres"

    [[ -d "$WINSDK_BASE/crt/include" ]] || die "missing Windows CRT headers under $WINSDK_BASE"
    [[ -d "$WINSDK_BASE/sdk/include/ucrt" ]] || die "missing Windows SDK headers under $WINSDK_BASE"
    [[ -d "$WINSDK_BASE/crt/lib/x86_64" ]] || die "missing Windows CRT libraries under $WINSDK_BASE"
    [[ -d "$WINSDK_BASE/sdk/lib/ucrt/x86_64" ]] || die "missing Windows UCRT libraries under $WINSDK_BASE"
    [[ -d "$WINSDK_BASE/sdk/lib/um/x86_64" ]] || die "missing Windows UM libraries under $WINSDK_BASE"
    [[ -d "$VULKAN_DIR/Include" ]] || die "missing Vulkan SDK includes under $VULKAN_DIR"
    [[ -f "$VULKAN_DIR/Lib/vulkan-1.lib" ]] || die "missing Vulkan import library under $VULKAN_DIR"

    info "CRT includes : $WINSDK_BASE/crt/include"
    info "SDK includes : $WINSDK_BASE/sdk/include"
    info "SDK libs     : $WINSDK_BASE/sdk/lib"
    info "Vulkan SDK   : $VULKAN_DIR"
}

write_version_header() {
    mkdir -p "$GENERATED_DIR"

    cat > "$VERSION_HEADER_FILE" <<EOF
#ifndef SIMP_VERSION_GENERATED_H
#define SIMP_VERSION_GENERATED_H

#define APP_VERSION_STRING "${CURRENT_APP_VERSION}"
#define APP_VERSION_WSTRING L"${CURRENT_APP_VERSION}"
#define APP_VERSION_MAJOR ${BASE_VERSION_MAJOR}
#define APP_VERSION_MINOR ${BASE_VERSION_MINOR}
#define APP_VERSION_DATE ${CURRENT_BUILD_DATE}
#define APP_VERSION_BUILD ${CURRENT_BUILD_COUNT}

#endif
EOF
}

prepare_version_metadata() {
    banner "${EMOJI_PACKAGE} Preparing Version"

    local today previous_version previous_date previous_build
    today="$(date +%Y%m%d)"
    previous_version=""
    previous_date=""
    previous_build=0

    if [[ -f "$VERSION_STATE_FILE" ]]; then
        previous_version="$(tr -d '\r\n' < "$VERSION_STATE_FILE")"
        if [[ "$previous_version" =~ ^${BASE_VERSION_MAJOR}\.${BASE_VERSION_MINOR}\.([0-9]{8})\.([0-9]+)$ ]]; then
            previous_date="${BASH_REMATCH[1]}"
            previous_build="${BASH_REMATCH[2]}"
        else
            warn "Ignoring unreadable version state: $previous_version"
            previous_version=""
            previous_date=""
            previous_build=0
        fi
    fi

    if [[ "$previous_date" == "$today" ]]; then
        CURRENT_BUILD_COUNT=$((previous_build + 1))
    else
        CURRENT_BUILD_COUNT=1
    fi

    CURRENT_BUILD_DATE="$today"
    CURRENT_APP_VERSION="${BASE_VERSION_MAJOR}.${BASE_VERSION_MINOR}.${CURRENT_BUILD_DATE}.${CURRENT_BUILD_COUNT}"

    if [[ -n "$previous_version" ]]; then
        info "Previous version: $previous_version"
    else
        info "Previous version: none"
    fi
    success "Pending version: $CURRENT_APP_VERSION"

    write_version_header
    info "Generated header: $VERSION_HEADER_FILE"
}

finalize_version_metadata() {
    printf '%s\n' "$CURRENT_APP_VERSION" > "$VERSION_STATE_FILE"
    success "Recorded version: $CURRENT_APP_VERSION"
    info "Version file     : $VERSION_STATE_FILE"
}

generate_splash_resource_include() {
    banner "${EMOJI_PACKAGE} Embedding Splash Frames"

    local output_file="$GENERATED_DIR/splash_frames.rcinc"
    local frame_id=1000

    mkdir -p "$GENERATED_DIR"

    mapfile -t frames < <(find "$SCRIPT_DIR/graphics/splash_screen" -maxdepth 1 -type f -name 'frame_*.png' | sort)
    [[ ${#frames[@]} -gt 0 ]] || die "no splash PNG frames found under graphics/splash_screen"

    : > "$output_file"
    for frame in "${frames[@]}"; do
        local relative_path="${frame#"$SCRIPT_DIR/"}"
        printf '%d RCDATA "%s"\n' "$frame_id" "$relative_path" >> "$output_file"
        frame_id=$((frame_id + 1))
    done

    success "Embedded ${#frames[@]} splash frames"
    info "Resource include : $output_file"
}

compile_resources() {
    banner "${EMOJI_WRENCH} Compiling Resources"

    x86_64-w64-mingw32-windres \
        -O coff \
        -I "$SCRIPT_DIR" \
        -I "$WINSDK_BASE/crt/include" \
        -I "$WINSDK_BASE/sdk/include/ucrt" \
        -I "$WINSDK_BASE/sdk/include/um" \
        -I "$WINSDK_BASE/sdk/include/shared" \
        -i "$SCRIPT_DIR/resource.rc" \
        -o "$BUILD_DIR/resource.res"

    success "Compiled resource.rc"
}

compile_sources() {
    local -n object_files_ref=$1
    banner "${EMOJI_ROCKET} Parallel Compilation (${COMPILE_JOBS} jobs)"

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

    mapfile -t sources < <(find "$SCRIPT_DIR/src" -maxdepth 1 -type f -name '*.cpp' | sort)
    [[ ${#sources[@]} -gt 0 ]] || die "no source files found under src"

    info "Source files : ${#sources[@]}"
    info "Compile jobs : $COMPILE_JOBS"

    object_files_ref=()
    local compile_log_dir="$BUILD_DIR/compile_logs"
    rm -rf "$compile_log_dir"
    mkdir -p "$compile_log_dir"

    local batch_start=0
    while [[ $batch_start -lt ${#sources[@]} ]]; do
        local batch_pids=()
        local batch_sources=("${sources[@]:batch_start:COMPILE_JOBS}")

        for source_file in "${batch_sources[@]}"; do
            local base_name
            local object_file
            local log_file
            local status_file

            base_name="$(basename "${source_file%.cpp}")"
            object_file="$BUILD_DIR/${base_name}.obj"
            log_file="$compile_log_dir/${base_name}.log"
            status_file="$compile_log_dir/${base_name}.status"
            object_files_ref+=("$object_file")

            step "Queued ${base_name}.cpp"
            (
                if clang++ "${build_flags[@]}" -c "$source_file" -o "$object_file" >"$log_file" 2>&1; then
                    printf 'success\n' > "$status_file"
                else
                    printf 'failed\n' > "$status_file"
                fi
            ) &
            batch_pids+=("$!")
        done

        for pid in "${batch_pids[@]}"; do
            wait "$pid" || true
        done

        local batch_failed=0
        for source_file in "${batch_sources[@]}"; do
            local base_name
            local log_file
            local status_file

            base_name="$(basename "${source_file%.cpp}")"
            log_file="$compile_log_dir/${base_name}.log"
            status_file="$compile_log_dir/${base_name}.status"

            if [[ -f "$status_file" ]] && [[ "$(<"$status_file")" == "success" ]]; then
                success "Built ${base_name}.obj"
                if [[ -s "$log_file" ]]; then
                    printf "${COLOR_YELLOW}"
                    cat "$log_file"
                    printf "${COLOR_RESET}"
                fi
            else
                batch_failed=1
                fail "Failed ${base_name}.cpp"
                if [[ -s "$log_file" ]]; then
                    printf "${COLOR_RED}"
                    cat "$log_file"
                    printf "${COLOR_RESET}"
                fi
            fi

            rm -f "$log_file" "$status_file"
        done

        if [[ $batch_failed -ne 0 ]]; then
            rm -rf "$compile_log_dir"
            return 1
        fi

        batch_start=$((batch_start + COMPILE_JOBS))
    done

    rm -rf "$compile_log_dir"
    success "Compiled ${#sources[@]} source files"
}

link_executable() {
    local output_file="$1"
    shift
    local object_files=("$@")

    banner "${EMOJI_ROCKET} Linking Executable"
    info "Output  : $output_file"
    info "Version : $CURRENT_APP_VERSION"

    local link_flags=(
        "--target=$TARGET_TRIPLE"
        "-fuse-ld=lld-link"
        "-Xlinker" "/machine:x64"
        "-Xlinker" "/subsystem:windows"
        "-Xlinker" "/incremental:no"
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
        "${object_files[@]}" \
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
        "$WINSDK_BASE/sdk/lib/um/x86_64/shell32.lib" \
        "$WINSDK_BASE/sdk/lib/um/x86_64/gdi32.lib" \
        "$WINSDK_BASE/sdk/lib/um/x86_64/advapi32.lib" \
        "$WINSDK_BASE/sdk/lib/um/x86_64/ole32.lib" \
        "$WINSDK_BASE/sdk/lib/um/x86_64/windowscodecs.lib" \
        "$WINSDK_BASE/sdk/lib/um/x86_64/dwmapi.lib" \
        "$WINSDK_BASE/sdk/lib/um/x86_64/uxtheme.lib" \
        "$VULKAN_DIR/Lib/vulkan-1.lib"

    success "Linked $(basename "$output_file")"
}

print_summary() {
    local output_file="$1"
    local output_size="unknown"

    if [[ -f "$output_file" ]]; then
        output_size="$(du -h "$output_file" | awk '{print $1}')"
    fi

    banner "${EMOJI_SUCCESS} Build Complete"
    printf "${COLOR_BOLD}${COLOR_GREEN}Project:${COLOR_RESET} %s\n" "$PROJECT_NAME"
    printf "${COLOR_BOLD}${COLOR_GREEN}Mode:${COLOR_RESET}    %s\n" "$BUILD_MODE"
    printf "${COLOR_BOLD}${COLOR_GREEN}Version:${COLOR_RESET} %s\n" "$CURRENT_APP_VERSION"
    printf "${COLOR_BOLD}${COLOR_GREEN}Output:${COLOR_RESET}  %s\n" "$output_file"
    printf "${COLOR_BOLD}${COLOR_GREEN}Size:${COLOR_RESET}    %s\n" "$output_size"
}

main() {
    detect_mode "${1:-release}"

    if [[ "$BUILD_MODE" == "clean" ]]; then
        clean
        exit 0
    fi

    check_environment
    mkdir -p "$BUILD_DIR"

    prepare_version_metadata
    generate_splash_resource_include
    compile_resources

    local object_files=()
    compile_sources object_files

    if [[ "$BUILD_MODE" == "debug" ]]; then
        link_executable "$OUTPUT_DEBUG" "${object_files[@]}"
        finalize_version_metadata
        print_summary "$OUTPUT_DEBUG"
    else
        link_executable "$OUTPUT_RELEASE" "${object_files[@]}"
        finalize_version_metadata
        print_summary "$OUTPUT_RELEASE"
    fi
}

main "$@"
