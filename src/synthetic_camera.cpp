#include "internal.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <utility>

namespace xgc2 {
namespace camera {
namespace {

struct SyntheticStorage {
  std::vector<std::vector<std::uint8_t>> planes;
};

std::uint32_t stride_for(PixelFormat format, std::uint32_t width) {
  switch (format) {
  case PixelFormat::YUYV:
  case PixelFormat::UYVY:
    return width * 2U;
  case PixelFormat::RGB24:
  case PixelFormat::BGR24:
    return width * 3U;
  case PixelFormat::NV12:
  case PixelFormat::GREY:
    return width;
  case PixelFormat::MJPEG:
  case PixelFormat::H264:
  case PixelFormat::Unknown:
    return 0U;
  }
  return 0U;
}

Capabilities make_capabilities() {
  Capabilities result;
  result.driver = "xgc2-synthetic";
  result.card = "Deterministic CI camera";
  result.bus_info = "synthetic:0";
  result.streaming = true;
  result.single_plane = true;
  result.multi_plane = true;
  const PixelFormat formats[] = {
      PixelFormat::MJPEG, PixelFormat::H264,  PixelFormat::YUYV, PixelFormat::UYVY,
      PixelFormat::RGB24, PixelFormat::BGR24, PixelFormat::NV12, PixelFormat::GREY,
  };
  for (const auto format : formats) {
    FormatCapability capability;
    capability.pixel_format = format;
    capability.capture_mode = CaptureMode::SinglePlane;
    capability.description = std::string("Synthetic ") + to_string(format);
    FrameSize size;
    size.min_width = 1U;
    size.max_width = 8192U;
    size.width_step = 1U;
    size.min_height = 1U;
    size.max_height = 8192U;
    size.height_step = 1U;
    size.discrete = false;
    capability.frame_sizes.push_back(size);
    result.formats.push_back(capability);
    if (format == PixelFormat::NV12) {
      capability.capture_mode = CaptureMode::MultiPlane;
      capability.description = "Synthetic multi-plane nv12";
      result.formats.push_back(capability);
    }
  }
  return result;
}

class SyntheticCamera final : public Camera {
public:
  explicit SyntheticCamera(CaptureConfig config)
      : config_(std::move(config)), capabilities_(make_capabilities()) {
    if (config_.capture_mode == CaptureMode::Auto) {
      config_.capture_mode = CaptureMode::SinglePlane;
    }
    if (config_.capture_mode == CaptureMode::MultiPlane &&
        config_.pixel_format != PixelFormat::NV12) {
      throw CameraError(ErrorCode::Unsupported,
                        "synthetic multi-plane capture is supported for NV12 only");
    }
  }

  const CaptureConfig &config() const noexcept override { return config_; }

  const Capabilities &capabilities() const noexcept override { return capabilities_; }

  void start() override {
    if (running_) {
      throw CameraError(ErrorCode::InvalidState, "synthetic camera is already running");
    }
    sequence_ = 0U;
    next_frame_ = std::chrono::steady_clock::now();
    running_ = true;
  }

  Frame read(std::uint32_t timeout_ms) override {
    if (!running_) {
      throw CameraError(ErrorCode::InvalidState,
                        "synthetic camera must be started before read");
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < next_frame_) {
      const auto deadline =
          now + std::chrono::milliseconds(static_cast<std::int64_t>(timeout_ms));
      if (deadline < next_frame_) {
        std::this_thread::sleep_until(deadline);
        throw CameraError(ErrorCode::Timeout,
                          "timed out waiting for a synthetic capture frame");
      }
      std::this_thread::sleep_until(next_frame_);
    }

    auto storage = std::make_shared<SyntheticStorage>();
    const auto width = config_.width;
    const auto height = config_.height;
    const auto pixels = static_cast<std::size_t>(width) * height;
    if (config_.capture_mode == CaptureMode::MultiPlane) {
      storage->planes.resize(2U);
      storage->planes[0].resize(pixels);
      storage->planes[1].resize((pixels + 1U) / 2U);
    } else {
      storage->planes.resize(1U);
      std::size_t bytes = pixels;
      switch (config_.pixel_format) {
      case PixelFormat::MJPEG:
      case PixelFormat::H264:
        bytes = std::max<std::size_t>(64U, pixels / 8U);
        break;
      case PixelFormat::YUYV:
      case PixelFormat::UYVY:
        bytes = pixels * 2U;
        break;
      case PixelFormat::RGB24:
      case PixelFormat::BGR24:
        bytes = pixels * 3U;
        break;
      case PixelFormat::NV12:
        bytes = pixels + (pixels + 1U) / 2U;
        break;
      case PixelFormat::GREY:
        break;
      case PixelFormat::Unknown:
        throw CameraError(ErrorCode::Unsupported, "unknown synthetic pixel format");
      }
      storage->planes.front().resize(bytes);
    }

    std::uint32_t state =
        config_.synthetic_seed ^ static_cast<std::uint32_t>(sequence_);
    for (auto &bytes : storage->planes) {
      for (auto &byte : bytes) {
        state = state * 1664525U + 1013904223U;
        byte = static_cast<std::uint8_t>(state >> 24U);
      }
    }

    std::vector<Plane> planes;
    for (std::size_t index = 0; index < storage->planes.size(); ++index) {
      const auto &bytes = storage->planes[index];
      Plane plane;
      plane.data = bytes.data();
      plane.bytes_used = bytes.size();
      plane.length = bytes.size();
      plane.stride = stride_for(config_.pixel_format, width);
      planes.push_back(plane);
    }

    const auto period_ns =
        static_cast<std::uint64_t>(std::llround(1000000000.0 / config_.frame_rate));
    const auto timestamp_ns = sequence_ * period_ns;
    Timestamp timestamp;
    timestamp.seconds = static_cast<std::int64_t>(timestamp_ns / 1000000000ULL);
    timestamp.nanoseconds = static_cast<std::uint32_t>(timestamp_ns % 1000000000ULL);
    timestamp.clock = TimestampClock::Monotonic;

    const auto frame_sequence = sequence_++;
    next_frame_ =
        std::chrono::steady_clock::now() + std::chrono::nanoseconds(period_ns);
    return FrameBuilder::build(std::move(planes), width, height, config_.pixel_format,
                               config_.capture_mode, frame_sequence, timestamp,
                               std::static_pointer_cast<void>(storage));
  }

  void stop() noexcept override { running_ = false; }

  bool running() const noexcept override { return running_; }

private:
  CaptureConfig config_;
  Capabilities capabilities_;
  bool running_{false};
  std::uint64_t sequence_{0U};
  std::chrono::steady_clock::time_point next_frame_;
};

} // namespace

std::unique_ptr<Camera> make_synthetic_camera(const CaptureConfig &config) {
  return std::unique_ptr<Camera>(new SyntheticCamera(config));
}

} // namespace camera
} // namespace xgc2
