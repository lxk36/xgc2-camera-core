#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

bash -n .xgc2/scripts/*.sh
export PYTHONPYCACHEPREFIX="${PYTHONPYCACHEPREFIX:-/tmp/xgc2-camera-core-pycache}"
python3 -m py_compile \
  .xgc2/scripts/check_manifest_contract.py \
  .xgc2/scripts/xgc2_artifact_manifest.py
python3 .xgc2/scripts/check_manifest_contract.py

generated="$({
  find . \
    -path ./.git -prune -o \
    -path ./.ci -prune -o \
    -path ./build -prune -o \
    -path ./debs -prune -o \
    \( -name CMakeCache.txt -o -name CMakeFiles -o -name Testing \) -print
} || true)"
if [[ -n "${generated}" ]]; then
  echo "Generated build/test artifacts found in source tree:" >&2
  echo "${generated}" >&2
  exit 1
fi

required_files=(
  .clang-format
  .clang-tidy
  .github/workflows/ci.yml
  .github/workflows/release.yml
  .xgc2/product.yml
  .xgc2/scripts/build_deb.sh
  .xgc2/scripts/check_cpp_quality.sh
  .xgc2/scripts/check_manifest_contract.py
  .xgc2/scripts/check_package_compliance.sh
  .xgc2/scripts/smoke_test_installed.sh
  .xgc2/scripts/xgc2_artifact_manifest.py
  CMakeLists.txt
  LICENSE
  README.md
  cmake/xgc2_cameraConfig.cmake.in
  include/xgc2/camera/camera.hpp
  include/xgc2/camera/version.hpp
  pkgconfig/xgc2-camera.pc.in
  src/camera.cpp
  src/internal.hpp
  src/synthetic_camera.cpp
  src/v4l2_camera.cpp
  test/camera_test.cpp
  tools/xgc2-camera-inspect.cpp
)
for file in "${required_files[@]}"; do
  if [[ ! -f "${file}" ]]; then
    echo "Missing required file: ${file}" >&2
    exit 1
  fi
done

if [[ -e package.xml ]]; then
  echo "camera core must not contain a ROS package.xml" >&2
  exit 1
fi
if grep -R -n -E \
  'find_package\(catkin|catkin_package|ros::|#include[[:space:]]*[<\"]ros/' \
  CMakeLists.txt cmake include src tools test 2>/dev/null; then
  echo "camera core must remain independent of ROS and Catkin" >&2
  exit 1
fi
if ! grep -q 'add_library(xgc2::camera ALIAS xgc2_camera)' CMakeLists.txt; then
  echo "missing build-tree xgc2::camera target" >&2
  exit 1
fi
if ! grep -q 'NAMESPACE xgc2::' CMakeLists.txt; then
  echo "missing installed xgc2:: target namespace" >&2
  exit 1
fi

echo "libxgc2-camera-dev package compliance checks passed."
