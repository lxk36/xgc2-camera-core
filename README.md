# XGC2 Camera Core

`xgc2-camera-core` is the ROS-independent Linux camera capture product used by
XGC2 camera adapters. It is a small C++14 shared library built directly on the
V4L2 streaming API. The core has no ROS, Catkin, OpenCV, FFmpeg, Gazebo, or
device-specific vehicle dependency.

The product owns generic camera capture only. Platform-specific camera stacks,
vehicle integrations, ROS topic publication, decoding, calibration, and
simulation remain separate adapters or products.

## Features

- V4L2 capability, pixel format, frame size, and frame interval enumeration.
- MMAP streaming for `VIDEO_CAPTURE` and `VIDEO_CAPTURE_MPLANE` devices.
- MJPEG, H264, YUYV, UYVY, RGB24, BGR24, NV12/NV12M, and GREY formats.
- Driver capture timestamps from `v4l2_buffer`; the library never replaces a
  capture timestamp with the time at which `read()` happened to return.
- RAII ownership of file descriptors, mappings, stream state, and dequeued
  buffers.
- A deterministic, rate-limited synthetic backend for CI and adapter tests.
- Installed CMake target `xgc2::camera` and `xgc2-camera-inspect` utility.

The library transports native bytes. It does not claim that synthetic MJPEG or
H264 payloads are valid compressed bitstreams, and it does not decode or
convert pixel formats.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
(cd build && ctest --output-on-failure)
```

Install to a staging prefix:

```bash
cmake --install build --prefix /tmp/xgc2-camera-install
```

The Debian packaging script builds `libxgc2-camera-dev`:

```bash
PACKAGE_DISTRIBUTION=focal ./.xgc2/scripts/build_deb.sh
```

## Consumer example

```cpp
#include <xgc2/camera/camera.hpp>

xgc2::camera::CaptureConfig config;
config.device = "/dev/xgc2-camera/arena";
config.width = 1920;
config.height = 1080;
config.frame_rate = 30.0;
config.pixel_format = xgc2::camera::PixelFormat::MJPEG;

auto camera = xgc2::camera::make_camera(config);
camera->start();
auto frame = camera->read(2000);
// Native capture bytes remain valid while frame (or a copy) is alive.
camera->stop();
```

Consumer CMake:

```cmake
find_package(xgc2_camera REQUIRED CONFIG)
target_link_libraries(my_capture PRIVATE xgc2::camera)
```

## Buffer lifetime

`Frame` is a cheap, copyable lease. For V4L2, the dequeued kernel buffer is
returned with `VIDIOC_QBUF` only after the last copy of that frame is destroyed.
Holding every frame can therefore exhaust the configured buffer pool and make
the next `read()` time out. `stop()` is safe with outstanding frames; restart is
rejected until those leases have been released.

For multi-plane capture, use `Frame::planes()`. `Frame::data()`, `size()`, and
`stride()` are convenience accessors for plane zero.

## Timestamp contract

`Timestamp` preserves the seconds and microseconds supplied by the V4L2
driver, converted exactly to nanoseconds. `TimestampClock::Monotonic` is
reported only when the kernel sets `V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC`.
Otherwise the clock is `Unknown`; an adapter that needs wall-clock timestamps
must perform an explicit, measured clock-domain conversion.

The synthetic backend starts at timestamp zero and advances by the configured
frame period using the monotonic clock classification. It also paces `read()`;
when the next frame cannot arrive within `timeout_ms`, `CameraError` with
`ErrorCode::Timeout` is raised.

## Inspection

Enumerate a real device:

```bash
xgc2-camera-inspect --device /dev/video0
```

Exercise the hardware-free multi-plane path:

```bash
xgc2-camera-inspect --synthetic --format nv12 --mode multi \
  --width 640 --height 480 --fps 30 --capture 3
```

The executable returns non-zero on open, negotiation, streaming, or timeout
errors, making it suitable for installation smoke tests.

## Threading

One thread should own `start()`, `read()`, and `stop()` for a `Camera` instance.
Frames may be released on other threads. The V4L2 buffer lease return path is
serialized internally.

## Supported release matrix

The product CI builds and installs native Debian packages on Ubuntu Focal,
Jammy, and Noble for amd64 and arm64. No cross-compiled architecture is labeled
as a native artifact.
