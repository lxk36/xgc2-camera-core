#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace xgc2 {
namespace camera {

enum class BackendKind {
  V4L2,
  Synthetic,
};

enum class CaptureMode {
  Auto,
  SinglePlane,
  MultiPlane,
};

enum class PixelFormat {
  Unknown,
  MJPEG,
  H264,
  YUYV,
  UYVY,
  RGB24,
  BGR24,
  NV12,
  GREY,
};

enum class TimestampClock {
  Unknown,
  Realtime,
  Monotonic,
};

enum class ErrorCode {
  InvalidArgument,
  OpenFailed,
  SystemError,
  Unsupported,
  InvalidState,
  Timeout,
};

class CameraError : public std::runtime_error {
public:
  CameraError(ErrorCode code, const std::string &message, int system_errno = 0);

  ErrorCode code() const noexcept;
  int system_errno() const noexcept;

private:
  ErrorCode code_;
  int system_errno_;
};

struct Timestamp {
  std::int64_t seconds{0};
  std::uint32_t nanoseconds{0};
  TimestampClock clock{TimestampClock::Unknown};

  std::int64_t to_nanoseconds() const noexcept;
};

struct Plane {
  const std::uint8_t *data{nullptr};
  std::size_t bytes_used{0};
  std::size_t length{0};
  std::uint32_t stride{0};
};

struct FrameInterval {
  std::uint32_t numerator{0};
  std::uint32_t denominator{0};
  double frames_per_second() const noexcept;
};

struct FrameSize {
  std::uint32_t min_width{0};
  std::uint32_t max_width{0};
  std::uint32_t width_step{0};
  std::uint32_t min_height{0};
  std::uint32_t max_height{0};
  std::uint32_t height_step{0};
  bool discrete{true};
  std::vector<FrameInterval> frame_intervals;
};

struct FormatCapability {
  PixelFormat pixel_format{PixelFormat::Unknown};
  std::uint32_t fourcc{0};
  CaptureMode capture_mode{CaptureMode::Auto};
  std::string description;
  std::vector<FrameSize> frame_sizes;
};

struct Capabilities {
  std::string driver;
  std::string card;
  std::string bus_info;
  bool streaming{false};
  bool single_plane{false};
  bool multi_plane{false};
  std::vector<FormatCapability> formats;
};

struct CaptureConfig {
  BackendKind backend{BackendKind::V4L2};
  std::string device{"/dev/video0"};
  std::uint32_t width{640};
  std::uint32_t height{480};
  double frame_rate{30.0};
  PixelFormat pixel_format{PixelFormat::MJPEG};
  CaptureMode capture_mode{CaptureMode::Auto};
  std::uint32_t buffer_count{4};
  std::uint32_t synthetic_seed{1};
};

class Frame {
public:
  Frame() noexcept;

  const std::uint8_t *data() const noexcept;
  std::size_t size() const noexcept;
  const std::vector<Plane> &planes() const noexcept;
  std::uint32_t width() const noexcept;
  std::uint32_t height() const noexcept;
  std::uint32_t stride() const noexcept;
  PixelFormat pixel_format() const noexcept;
  CaptureMode capture_mode() const noexcept;
  std::uint64_t sequence() const noexcept;
  const Timestamp &timestamp() const noexcept;
  std::int64_t timestamp_ns() const noexcept;
  explicit operator bool() const noexcept;

private:
  struct Impl;
  explicit Frame(std::shared_ptr<Impl> impl) noexcept;
  std::shared_ptr<Impl> impl_;

  friend class FrameBuilder;
};

class Camera {
public:
  virtual ~Camera() = default;

  virtual const CaptureConfig &config() const noexcept = 0;
  virtual const Capabilities &capabilities() const noexcept = 0;
  virtual void start() = 0;
  virtual Frame read(std::uint32_t timeout_ms) = 0;
  virtual void stop() noexcept = 0;
  virtual bool running() const noexcept = 0;
};

std::unique_ptr<Camera> make_camera(const CaptureConfig &config);
Capabilities enumerate_v4l2(const std::string &device);

const char *to_string(BackendKind value) noexcept;
const char *to_string(CaptureMode value) noexcept;
const char *to_string(PixelFormat value) noexcept;
const char *to_string(TimestampClock value) noexcept;
BackendKind backend_kind_from_string(const std::string &value);
CaptureMode capture_mode_from_string(const std::string &value);
PixelFormat pixel_format_from_string(const std::string &value);

} // namespace camera
} // namespace xgc2
