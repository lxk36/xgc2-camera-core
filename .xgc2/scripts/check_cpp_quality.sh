#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${XGC2_CAMERA_QUALITY_BUILD_DIR:-${repo_root}/.ci/cpp-quality}"
cd "${repo_root}"

sources=(
  include/xgc2/camera/camera.hpp
  include/xgc2/camera/version.hpp
  src/camera.cpp
  src/internal.hpp
  src/synthetic_camera.cpp
  src/v4l2_camera.cpp
  tools/xgc2-camera-inspect.cpp
  test/camera_test.cpp
)

for tool in cmake ctest clang-format cppcheck; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "Missing required C++ quality tool: ${tool}" >&2
    exit 1
  fi
done

clang-format --dry-run --Werror "${sources[@]}"

rm -rf "${build_dir}"
cmake -S "${repo_root}" -B "${build_dir}" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -Werror"
cmake --build "${build_dir}" -- -j"$(nproc)"
(cd "${build_dir}" && ctest --output-on-failure)

cppcheck \
  --enable=warning,performance,portability \
  --error-exitcode=1 \
  --std=c++14 \
  --inline-suppr \
  --suppress=missingIncludeSystem \
  -I include \
  src tools test

echo "XGC2 camera core C++ quality checks passed."
