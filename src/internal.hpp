#pragma once

#include <xgc2/camera/camera.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace xgc2 {
namespace camera {

struct Frame::Impl {
  std::vector<Plane> planes;
  std::uint32_t width{0};
  std::uint32_t height{0};
  PixelFormat pixel_format{PixelFormat::Unknown};
  CaptureMode capture_mode{CaptureMode::Auto};
  std::uint64_t sequence{0};
  Timestamp timestamp;
  std::shared_ptr<void> lease;
};

class FrameBuilder {
public:
  static Frame build(std::vector<Plane> planes, std::uint32_t width,
                     std::uint32_t height, PixelFormat pixel_format,
                     CaptureMode capture_mode, std::uint64_t sequence,
                     const Timestamp &timestamp, std::shared_ptr<void> lease);
};

std::unique_ptr<Camera> make_synthetic_camera(const CaptureConfig &config);
std::unique_ptr<Camera> make_v4l2_camera(const CaptureConfig &config);

std::uint32_t pixel_format_to_v4l2(PixelFormat format, CaptureMode mode) noexcept;
PixelFormat pixel_format_from_v4l2(std::uint32_t fourcc) noexcept;
std::uint32_t select_advertised_fourcc(const Capabilities &capabilities,
                                       CaptureMode mode, PixelFormat format) noexcept;

} // namespace camera
} // namespace xgc2
