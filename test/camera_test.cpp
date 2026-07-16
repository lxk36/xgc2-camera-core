#include <xgc2/camera/camera.hpp>

#include "internal.hpp"

#include <linux/videodev2.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace camera = xgc2::camera;

namespace {

void require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename Function>
void require_camera_error(camera::ErrorCode code, Function function,
                          const std::string &message) {
  try {
    function();
  } catch (const camera::CameraError &error) {
    require(error.code() == code, message + " (wrong error code)");
    return;
  }
  throw std::runtime_error(message + " (no exception)");
}

camera::CaptureConfig base_config() {
  camera::CaptureConfig config;
  config.backend = camera::BackendKind::Synthetic;
  config.width = 8U;
  config.height = 4U;
  config.frame_rate = 1000.0;
  config.pixel_format = camera::PixelFormat::GREY;
  config.capture_mode = camera::CaptureMode::SinglePlane;
  config.buffer_count = 2U;
  return config;
}

void test_format_helpers() {
  require(camera::pixel_format_from_string("MJPG") == camera::PixelFormat::MJPEG,
          "MJPG parser failed");
  require(camera::pixel_format_from_string("mono8") == camera::PixelFormat::GREY,
          "mono8 parser failed");
  require(std::string(camera::to_string(camera::PixelFormat::NV12)) == "nv12",
          "NV12 formatter failed");
  require(camera::backend_kind_from_string("synthetic") ==
              camera::BackendKind::Synthetic,
          "backend parser failed");
  require(camera::capture_mode_from_string("multi") == camera::CaptureMode::MultiPlane,
          "capture mode parser failed");

  require(camera::pixel_format_from_v4l2(V4L2_PIX_FMT_JPEG) ==
              camera::PixelFormat::MJPEG,
          "JPEG semantic mapping failed");
  require(camera::pixel_format_from_v4l2(V4L2_PIX_FMT_MJPEG) ==
              camera::PixelFormat::MJPEG,
          "MJPEG semantic mapping failed");

  camera::Capabilities capabilities;
  camera::FormatCapability jpeg;
  jpeg.pixel_format = camera::PixelFormat::MJPEG;
  jpeg.fourcc = V4L2_PIX_FMT_JPEG;
  jpeg.capture_mode = camera::CaptureMode::SinglePlane;
  capabilities.formats.push_back(jpeg);
  require(
      camera::select_advertised_fourcc(capabilities, camera::CaptureMode::SinglePlane,
                                       camera::PixelFormat::MJPEG) == V4L2_PIX_FMT_JPEG,
      "negotiation must preserve the exact advertised JPEG fourcc");
  require(camera::select_advertised_fourcc(capabilities,
                                           camera::CaptureMode::MultiPlane,
                                           camera::PixelFormat::MJPEG) == 0U,
          "negotiation must match the advertised capture mode");
}

void test_single_plane_capture() {
  auto device = camera::make_camera(base_config());
  require(device->capabilities().streaming,
          "synthetic capabilities must advertise streaming");
  require_camera_error(
      camera::ErrorCode::InvalidState, [&device]() { device->read(1U); },
      "read before start must fail");
  device->start();
  const auto first = device->read(10U);
  require(static_cast<bool>(first), "first frame is empty");
  require(first.width() == 8U && first.height() == 4U, "frame geometry mismatch");
  require(first.planes().size() == 1U && first.size() == 32U,
          "GREY plane size mismatch");
  require(first.stride() == 8U, "GREY stride mismatch");
  require(first.sequence() == 0U && first.timestamp_ns() == 0,
          "first frame metadata mismatch");
  const auto second = device->read(10U);
  require(second.sequence() == 1U && second.timestamp_ns() == 1000000,
          "second frame timing mismatch");
  bool payload_differs = false;
  for (std::size_t index = 0U; index < first.size(); ++index) {
    payload_differs = payload_differs || first.data()[index] != second.data()[index];
  }
  require(payload_differs, "synthetic sequence should alter payload");
  device->stop();
  require(!device->running(), "stop did not update running state");
}

void test_multi_plane_nv12() {
  auto config = base_config();
  config.pixel_format = camera::PixelFormat::NV12;
  config.capture_mode = camera::CaptureMode::MultiPlane;
  auto device = camera::make_camera(config);
  device->start();
  const auto frame = device->read(10U);
  require(frame.capture_mode() == camera::CaptureMode::MultiPlane,
          "multi-plane mode was not retained");
  require(frame.planes().size() == 2U, "NV12 must expose two planes");
  require(frame.planes()[0].bytes_used == 32U, "NV12 luma size mismatch");
  require(frame.planes()[1].bytes_used == 16U, "NV12 chroma size mismatch");
}

void test_timeout_and_validation() {
  auto config = base_config();
  config.frame_rate = 10.0;
  auto device = camera::make_camera(config);
  device->start();
  (void)device->read(1U);
  require_camera_error(
      camera::ErrorCode::Timeout, [&device]() { device->read(1U); },
      "synthetic throttle must honor timeout");

  config.width = 0U;
  require_camera_error(
      camera::ErrorCode::InvalidArgument,
      [&config]() { (void)camera::make_camera(config); },
      "zero width must be rejected");
}

} // namespace

int main() {
  try {
    test_format_helpers();
    test_single_plane_capture();
    test_multi_plane_nv12();
    test_timeout_and_validation();
    std::cout << "xgc2_camera tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return 1;
  }
}
