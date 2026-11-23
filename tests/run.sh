#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
TASK_TEST_DIR=$(mktemp -d /tmp/simp-tests.XXXXXX)
trap 'rm -rf "$TASK_TEST_DIR"' EXIT
clang++ -std=c++20 -Wall -Wextra -Werror -g -fsanitize=address,undefined -Iinclude \
    tests/core_tests.cpp src/simpformat.cpp src/image_history.cpp src/cels.cpp src/cpu_work.cpp -pthread -o "$TASK_TEST_DIR/core-tests"
ASAN_OPTIONS=detect_leaks=0 "$TASK_TEST_DIR/core-tests" "$TASK_TEST_DIR/data"
clang++ -std=c++20 -Wall -Wextra -Werror -g -fsanitize=address,undefined -Iinclude \
    tests/ui_tests.cpp src/editor_ui.cpp -o "$TASK_TEST_DIR/ui-tests"
ASAN_OPTIONS=detect_leaks=0 "$TASK_TEST_DIR/ui-tests"
clang++ -std=c++20 -Wall -Wextra -Werror -g -pthread -fsanitize=address,undefined -Iinclude \
    tests/cpu_tests.cpp src/cpu_pixels.cpp src/cpu_work.cpp src/color_inventory.cpp -o "$TASK_TEST_DIR/cpu-tests"
ASAN_OPTIONS=detect_leaks=0 "$TASK_TEST_DIR/cpu-tests"
clang++ -std=c++20 -Wall -Wextra -Werror -g -fsanitize=address,undefined -Iinclude \
    tests/paint_tests.cpp src/cpu_pixels.cpp src/image_history.cpp src/cels.cpp src/cpu_work.cpp -pthread -o "$TASK_TEST_DIR/paint-tests"
ASAN_OPTIONS=detect_leaks=0 "$TASK_TEST_DIR/paint-tests"
clang++ -std=c++20 -Wall -Wextra -Werror -g -pthread -fsanitize=address,undefined -Iinclude \
    tests/cels_tests.cpp src/simpformat.cpp src/image_history.cpp src/cels.cpp src/cpu_work.cpp src/color_inventory.cpp -o "$TASK_TEST_DIR/cels-tests"
ASAN_OPTIONS=detect_leaks=0 "$TASK_TEST_DIR/cels-tests" "$TASK_TEST_DIR/cels-data"
clang++ -std=c++20 -Wall -Wextra -Werror -O2 -g -pthread -fsanitize=address,undefined -Iinclude \
    tests/splash_tests.cpp src/splash_shadow.cpp src/cpu_work.cpp -o "$TASK_TEST_DIR/splash-tests"
ASAN_OPTIONS=detect_leaks=0 "$TASK_TEST_DIR/splash-tests"
for stage in vert frag; do
    glslangValidator -V --target-env vulkan1.1 "shaders/ui.$stage" -o "$TASK_TEST_DIR/ui.$stage.spv"
    spirv-val --target-env vulkan1.1 "$TASK_TEST_DIR/ui.$stage.spv"
done
for stage in vert frag; do
    glslangValidator -V --target-env vulkan1.1 "shaders/canvas.$stage" -o "$TASK_TEST_DIR/canvas.$stage.spv"
    spirv-val --target-env vulkan1.1 "$TASK_TEST_DIR/canvas.$stage.spv"
done

glslangValidator -V --target-env vulkan1.1 shaders/paint.comp -o "$TASK_TEST_DIR/paint.comp.spv"
spirv-val --target-env vulkan1.1 "$TASK_TEST_DIR/paint.comp.spv"

# Explicit opt-in: a headless ICD can validate shader math without a Win32 surface.
if [[ -n "${SIMP_TEST_VULKAN_ICD:-}" ]]; then
    clang++ -std=c++20 -Wall -Wextra -Werror -Wno-missing-field-initializers \
        tests/compute_tests.cpp -l:libvulkan.so.1 -o "$TASK_TEST_DIR/compute-tests"
    result=0
    VK_ICD_FILENAMES="$SIMP_TEST_VULKAN_ICD" "$TASK_TEST_DIR/compute-tests" "$TASK_TEST_DIR/paint.comp.spv" || result=$?
    if [[ "$result" != 0 && "$result" != 77 ]]; then exit "$result"; fi
    clang++ -std=c++20 -Wall -Wextra -Werror -Wno-missing-field-initializers -O2 -pthread -Iinclude \
        tests/cel_render_tests.cpp src/cels.cpp src/cpu_work.cpp -l:libvulkan.so.1 -o "$TASK_TEST_DIR/cel-render-tests"
    result=0
    VK_ICD_FILENAMES="$SIMP_TEST_VULKAN_ICD" "$TASK_TEST_DIR/cel-render-tests" "$TASK_TEST_DIR/canvas.vert.spv" "$TASK_TEST_DIR/canvas.frag.spv" || result=$?
    if [[ "$result" != 0 && "$result" != 77 ]]; then exit "$result"; fi
fi
