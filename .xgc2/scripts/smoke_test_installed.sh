#!/usr/bin/env bash

set -euo pipefail

dpkg -s libxgc2-camera-dev >/dev/null

multiarch="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"
test -f /usr/include/xgc2/camera/camera.hpp
test -f /usr/include/xgc2/camera/version.hpp
test -e "/usr/lib/${multiarch}/libxgc2_camera.so"
test -f "/usr/lib/${multiarch}/cmake/xgc2_camera/xgc2_cameraConfig.cmake"
test -f "/usr/lib/${multiarch}/pkgconfig/xgc2-camera.pc"
test -x /usr/bin/xgc2-camera-inspect

ldd "/usr/lib/${multiarch}/libxgc2_camera.so" | \
  tee /tmp/xgc2-camera-ldd.txt
if grep -q "not found" /tmp/xgc2-camera-ldd.txt; then
  exit 1
fi

test "$(pkg-config --modversion xgc2-camera)" = "0.1.0"
xgc2-camera-inspect --synthetic --format nv12 --mode multi \
  --width 16 --height 8 --fps 100 --capture 2 --timeout-ms 100 \
  | tee /tmp/xgc2-camera-inspect.txt
grep -q 'planes=2' /tmp/xgc2-camera-inspect.txt
grep -q 'sequence=1' /tmp/xgc2-camera-inspect.txt

probe_dir="${XGC2_CAMERA_SMOKE_DIR:-$(mktemp -d -t xgc2-camera-smoke-XXXXXX)}"
mkdir -p "${probe_dir}/build"
cat >"${probe_dir}/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.10)
project(xgc2_camera_link_probe LANGUAGES CXX)

find_package(xgc2_camera REQUIRED CONFIG)
add_executable(link_probe link_probe.cpp)
target_compile_features(link_probe PRIVATE cxx_std_14)
target_link_libraries(link_probe PRIVATE xgc2::camera)
CMAKE
cat >"${probe_dir}/link_probe.cpp" <<'CPP'
#include <xgc2/camera/camera.hpp>

int main()
{
  xgc2::camera::CaptureConfig config;
  config.backend = xgc2::camera::BackendKind::Synthetic;
  config.pixel_format = xgc2::camera::PixelFormat::GREY;
  config.capture_mode = xgc2::camera::CaptureMode::SinglePlane;
  config.width = 8;
  config.height = 4;
  config.frame_rate = 1000.0;
  config.buffer_count = 2;
  auto camera = xgc2::camera::make_camera(config);
  camera->start();
  const auto frame = camera->read(100);
  return frame.size() == 32 && frame.sequence() == 0 ? 0 : 1;
}
CPP

cmake -S "${probe_dir}" -B "${probe_dir}/build"
cmake --build "${probe_dir}/build" -- -j2
"${probe_dir}/build/link_probe"

echo "libxgc2-camera-dev installed smoke test passed."
