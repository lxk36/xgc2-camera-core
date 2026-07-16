#include <xgc2/camera/camera.hpp>

#include "internal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <utility>

namespace xgc2 {
namespace camera {

CameraError::CameraError(ErrorCode code, const std::string &message, int system_errno)
    : std::runtime_error(message), code_(code), system_errno_(system_errno) {}

ErrorCode CameraError::code() const noexcept { return code_; }

int CameraError::system_errno() const noexcept { return system_errno_; }

std::int64_t Timestamp::to_nanoseconds() const noexcept {
  constexpr std::int64_t kNanosecondsPerSecond = 1000000000LL;
  if (seconds > std::numeric_limits<std::int64_t>::max() / kNanosecondsPerSecond) {
    return std::numeric_limits<std::int64_t>::max();
  }
  if (seconds < std::numeric_limits<std::int64_t>::min() / kNanosecondsPerSecond) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return seconds * kNanosecondsPerSecond + nanoseconds;
}

double FrameInterval::frames_per_second() const noexcept {
  if (numerator == 0U) {
    return 0.0;
  }
  return static_cast<double>(denominator) / static_cast<double>(numerator);
}

Frame::Frame() noexcept = default;

Frame::Frame(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

const std::uint8_t *Frame::data() const noexcept {
  return impl_ && !impl_->planes.empty() ? impl_->planes.front().data : nullptr;
}

std::size_t Frame::size() const noexcept {
  return impl_ && !impl_->planes.empty() ? impl_->planes.front().bytes_used : 0U;
}

const std::vector<Plane> &Frame::planes() const noexcept {
  static const std::vector<Plane> empty;
  return impl_ ? impl_->planes : empty;
}

std::uint32_t Frame::width() const noexcept { return impl_ ? impl_->width : 0U; }

std::uint32_t Frame::height() const noexcept { return impl_ ? impl_->height : 0U; }

std::uint32_t Frame::stride() const noexcept {
  return impl_ && !impl_->planes.empty() ? impl_->planes.front().stride : 0U;
}

PixelFormat Frame::pixel_format() const noexcept {
  return impl_ ? impl_->pixel_format : PixelFormat::Unknown;
}

CaptureMode Frame::capture_mode() const noexcept {
  return impl_ ? impl_->capture_mode : CaptureMode::Auto;
}

std::uint64_t Frame::sequence() const noexcept { return impl_ ? impl_->sequence : 0U; }

const Timestamp &Frame::timestamp() const noexcept {
  static const Timestamp empty;
  return impl_ ? impl_->timestamp : empty;
}

std::int64_t Frame::timestamp_ns() const noexcept {
  return timestamp().to_nanoseconds();
}

Frame::operator bool() const noexcept { return impl_ != nullptr; }

Frame FrameBuilder::build(std::vector<Plane> planes, std::uint32_t width,
                          std::uint32_t height, PixelFormat pixel_format,
                          CaptureMode capture_mode, std::uint64_t sequence,
                          const Timestamp &timestamp, std::shared_ptr<void> lease) {
  auto impl = std::make_shared<Frame::Impl>();
  impl->planes = std::move(planes);
  impl->width = width;
  impl->height = height;
  impl->pixel_format = pixel_format;
  impl->capture_mode = capture_mode;
  impl->sequence = sequence;
  impl->timestamp = timestamp;
  impl->lease = std::move(lease);
  return Frame(std::move(impl));
}

std::unique_ptr<Camera> make_camera(const CaptureConfig &config) {
  if (config.width == 0U || config.height == 0U) {
    throw CameraError(ErrorCode::InvalidArgument,
                      "camera width and height must be non-zero");
  }
  if (!std::isfinite(config.frame_rate) || config.frame_rate <= 0.0) {
    throw CameraError(ErrorCode::InvalidArgument,
                      "camera frame_rate must be finite and positive");
  }
  constexpr double kRateScale = 1000.0;
  if (config.frame_rate * kRateScale < 1.0 ||
      config.frame_rate * kRateScale >
          static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
    throw CameraError(ErrorCode::InvalidArgument,
                      "camera frame_rate cannot be represented by V4L2");
  }
  if (config.pixel_format == PixelFormat::Unknown) {
    throw CameraError(ErrorCode::InvalidArgument,
                      "camera pixel format must not be unknown");
  }
  if (config.buffer_count < 2U) {
    throw CameraError(ErrorCode::InvalidArgument,
                      "camera buffer_count must be at least two");
  }

  switch (config.backend) {
  case BackendKind::V4L2:
    return make_v4l2_camera(config);
  case BackendKind::Synthetic:
    return make_synthetic_camera(config);
  }
  throw CameraError(ErrorCode::InvalidArgument, "unknown camera backend");
}

const char *to_string(BackendKind value) noexcept {
  switch (value) {
  case BackendKind::V4L2:
    return "v4l2";
  case BackendKind::Synthetic:
    return "synthetic";
  }
  return "unknown";
}

const char *to_string(CaptureMode value) noexcept {
  switch (value) {
  case CaptureMode::Auto:
    return "auto";
  case CaptureMode::SinglePlane:
    return "single-plane";
  case CaptureMode::MultiPlane:
    return "multi-plane";
  }
  return "unknown";
}

const char *to_string(PixelFormat value) noexcept {
  switch (value) {
  case PixelFormat::Unknown:
    return "unknown";
  case PixelFormat::MJPEG:
    return "mjpeg";
  case PixelFormat::H264:
    return "h264";
  case PixelFormat::YUYV:
    return "yuyv";
  case PixelFormat::UYVY:
    return "uyvy";
  case PixelFormat::RGB24:
    return "rgb24";
  case PixelFormat::BGR24:
    return "bgr24";
  case PixelFormat::NV12:
    return "nv12";
  case PixelFormat::GREY:
    return "grey";
  }
  return "unknown";
}

const char *to_string(TimestampClock value) noexcept {
  switch (value) {
  case TimestampClock::Unknown:
    return "unknown";
  case TimestampClock::Realtime:
    return "realtime";
  case TimestampClock::Monotonic:
    return "monotonic";
  }
  return "unknown";
}

PixelFormat pixel_format_from_string(const std::string &value) {
  std::string normalized(value);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  if (normalized == "mjpeg" || normalized == "mjpg") {
    return PixelFormat::MJPEG;
  }
  if (normalized == "h264") {
    return PixelFormat::H264;
  }
  if (normalized == "yuyv" || normalized == "yuy2") {
    return PixelFormat::YUYV;
  }
  if (normalized == "uyvy") {
    return PixelFormat::UYVY;
  }
  if (normalized == "rgb24" || normalized == "rgb") {
    return PixelFormat::RGB24;
  }
  if (normalized == "bgr24" || normalized == "bgr") {
    return PixelFormat::BGR24;
  }
  if (normalized == "nv12") {
    return PixelFormat::NV12;
  }
  if (normalized == "grey" || normalized == "gray" || normalized == "mono8") {
    return PixelFormat::GREY;
  }
  return PixelFormat::Unknown;
}

BackendKind backend_kind_from_string(const std::string &value) {
  std::string normalized(value);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  if (normalized == "v4l2") {
    return BackendKind::V4L2;
  }
  if (normalized == "synthetic") {
    return BackendKind::Synthetic;
  }
  throw CameraError(ErrorCode::InvalidArgument, "unknown camera backend: " + value);
}

CaptureMode capture_mode_from_string(const std::string &value) {
  std::string normalized(value);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  if (normalized == "auto") {
    return CaptureMode::Auto;
  }
  if (normalized == "single" || normalized == "single-plane" ||
      normalized == "single_plane") {
    return CaptureMode::SinglePlane;
  }
  if (normalized == "multi" || normalized == "multi-plane" ||
      normalized == "multi_plane") {
    return CaptureMode::MultiPlane;
  }
  throw CameraError(ErrorCode::InvalidArgument,
                    "unknown camera capture mode: " + value);
}

} // namespace camera
} // namespace xgc2
