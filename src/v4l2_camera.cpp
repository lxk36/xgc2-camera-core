#include "internal.hpp"

#include <linux/videodev2.h>

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace xgc2 {
namespace camera {
namespace {

int retry_ioctl(int fd, unsigned long request, void *argument) noexcept {
  int result = 0;
  do {
    result = ::ioctl(fd, request, argument);
  } while (result < 0 && errno == EINTR);
  return result;
}

std::string system_error_message(const std::string &operation, int error) {
  std::ostringstream stream;
  stream << operation << ": " << std::strerror(error) << " (errno " << error << ")";
  return stream.str();
}

[[noreturn]] void throw_system_error(const std::string &operation,
                                     ErrorCode code = ErrorCode::SystemError) {
  const int error = errno;
  throw CameraError(code, system_error_message(operation, error), error);
}

class UniqueFd {
public:
  UniqueFd() noexcept = default;
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}
  ~UniqueFd() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;

  UniqueFd(UniqueFd &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) {
        ::close(fd_);
      }
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  int get() const noexcept { return fd_; }
  explicit operator bool() const noexcept { return fd_ >= 0; }

private:
  int fd_{-1};
};

class Mapping {
public:
  Mapping() noexcept = default;
  Mapping(void *address, std::size_t length) noexcept
      : address_(address), length_(length) {}
  ~Mapping() { reset(); }

  Mapping(const Mapping &) = delete;
  Mapping &operator=(const Mapping &) = delete;

  Mapping(Mapping &&other) noexcept : address_(other.address_), length_(other.length_) {
    other.address_ = MAP_FAILED;
    other.length_ = 0U;
  }
  Mapping &operator=(Mapping &&other) noexcept {
    if (this != &other) {
      reset();
      address_ = other.address_;
      length_ = other.length_;
      other.address_ = MAP_FAILED;
      other.length_ = 0U;
    }
    return *this;
  }

  void *address() const noexcept { return address_; }
  std::size_t length() const noexcept { return length_; }

private:
  void reset() noexcept {
    if (address_ != MAP_FAILED) {
      ::munmap(address_, length_);
      address_ = MAP_FAILED;
      length_ = 0U;
    }
  }

  void *address_{MAP_FAILED};
  std::size_t length_{0U};
};

struct BufferMemory {
  std::vector<Mapping> planes;
};

UniqueFd open_device(const std::string &path) {
  if (path.empty()) {
    throw CameraError(ErrorCode::InvalidArgument, "V4L2 device path must not be empty");
  }
  const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    throw_system_error("open " + path, ErrorCode::OpenFailed);
  }
  return UniqueFd(fd);
}

std::string fixed_string(const __u8 *value, std::size_t capacity) {
  const auto *characters = reinterpret_cast<const char *>(value);
  std::size_t length = 0U;
  while (length < capacity && characters[length] != '\0') {
    ++length;
  }
  return std::string(characters, length);
}

std::uint32_t effective_device_caps(const v4l2_capability &capability) {
  return (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U
             ? capability.device_caps
             : capability.capabilities;
}

void enumerate_intervals(int fd, std::uint32_t fourcc, std::uint32_t width,
                         std::uint32_t height, std::vector<FrameInterval> *output) {
  for (std::uint32_t index = 0U;; ++index) {
    v4l2_frmivalenum interval;
    std::memset(&interval, 0, sizeof(interval));
    interval.index = index;
    interval.pixel_format = fourcc;
    interval.width = width;
    interval.height = height;
    if (retry_ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) < 0) {
      if (errno == EINVAL) {
        break;
      }
      throw_system_error("VIDIOC_ENUM_FRAMEINTERVALS");
    }
    if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
      output->push_back({interval.discrete.numerator, interval.discrete.denominator});
    } else {
      output->push_back(
          {interval.stepwise.min.numerator, interval.stepwise.min.denominator});
      if (interval.stepwise.max.numerator != interval.stepwise.min.numerator ||
          interval.stepwise.max.denominator != interval.stepwise.min.denominator) {
        output->push_back(
            {interval.stepwise.max.numerator, interval.stepwise.max.denominator});
      }
      break;
    }
  }
}

void enumerate_sizes(int fd, std::uint32_t fourcc, std::vector<FrameSize> *output) {
  for (std::uint32_t index = 0U;; ++index) {
    v4l2_frmsizeenum size;
    std::memset(&size, 0, sizeof(size));
    size.index = index;
    size.pixel_format = fourcc;
    if (retry_ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) < 0) {
      if (errno == EINVAL) {
        break;
      }
      throw_system_error("VIDIOC_ENUM_FRAMESIZES");
    }

    FrameSize result;
    if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
      result.min_width = result.max_width = size.discrete.width;
      result.min_height = result.max_height = size.discrete.height;
      result.width_step = result.height_step = 0U;
      result.discrete = true;
    } else {
      result.min_width = size.stepwise.min_width;
      result.max_width = size.stepwise.max_width;
      result.width_step = size.stepwise.step_width;
      result.min_height = size.stepwise.min_height;
      result.max_height = size.stepwise.max_height;
      result.height_step = size.stepwise.step_height;
      result.discrete = false;
    }
    enumerate_intervals(fd, fourcc, result.min_width, result.min_height,
                        &result.frame_intervals);
    output->push_back(std::move(result));
    if (size.type != V4L2_FRMSIZE_TYPE_DISCRETE) {
      break;
    }
  }
}

Capabilities enumerate_fd(int fd) {
  v4l2_capability capability;
  std::memset(&capability, 0, sizeof(capability));
  if (retry_ioctl(fd, VIDIOC_QUERYCAP, &capability) < 0) {
    throw_system_error("VIDIOC_QUERYCAP");
  }

  const auto device_caps = effective_device_caps(capability);
  Capabilities result;
  result.driver = fixed_string(capability.driver, sizeof(capability.driver));
  result.card = fixed_string(capability.card, sizeof(capability.card));
  result.bus_info = fixed_string(capability.bus_info, sizeof(capability.bus_info));
  result.streaming = (device_caps & V4L2_CAP_STREAMING) != 0U;
  result.single_plane = (device_caps & V4L2_CAP_VIDEO_CAPTURE) != 0U;
  result.multi_plane = (device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0U;

  const v4l2_buf_type types[] = {V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                 V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
  for (const auto type : types) {
    const bool supported =
        type == V4L2_BUF_TYPE_VIDEO_CAPTURE ? result.single_plane : result.multi_plane;
    if (!supported) {
      continue;
    }
    for (std::uint32_t index = 0U;; ++index) {
      v4l2_fmtdesc format;
      std::memset(&format, 0, sizeof(format));
      format.index = index;
      format.type = type;
      if (retry_ioctl(fd, VIDIOC_ENUM_FMT, &format) < 0) {
        if (errno == EINVAL) {
          break;
        }
        throw_system_error("VIDIOC_ENUM_FMT");
      }
      FormatCapability entry;
      entry.pixel_format = pixel_format_from_v4l2(format.pixelformat);
      entry.fourcc = format.pixelformat;
      entry.capture_mode = type == V4L2_BUF_TYPE_VIDEO_CAPTURE
                               ? CaptureMode::SinglePlane
                               : CaptureMode::MultiPlane;
      entry.description = fixed_string(format.description, sizeof(format.description));
      enumerate_sizes(fd, format.pixelformat, &entry.frame_sizes);
      result.formats.push_back(std::move(entry));
    }
  }
  return result;
}

struct V4L2State {
  V4L2State(UniqueFd device_fd, v4l2_buf_type capture_type)
      : fd(std::move(device_fd)), type(capture_type) {}

  ~V4L2State() {
    std::lock_guard<std::mutex> lock(mutex);
    if (streaming) {
      auto local_type = type;
      retry_ioctl(fd.get(), VIDIOC_STREAMOFF, &local_type);
      streaming = false;
    }
  }

  bool queue_locked(std::uint32_t index) noexcept {
    if (index >= buffers.size()) {
      errno = EINVAL;
      return false;
    }
    v4l2_buffer buffer;
    std::memset(&buffer, 0, sizeof(buffer));
    buffer.type = type;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    std::vector<v4l2_plane> planes;
    if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
      planes.resize(buffers[index].planes.size());
      std::memset(planes.data(), 0, sizeof(v4l2_plane) * planes.size());
      buffer.m.planes = planes.data();
      buffer.length = static_cast<__u32>(planes.size());
    }
    return retry_ioctl(fd.get(), VIDIOC_QBUF, &buffer) == 0;
  }

  void release(std::uint32_t index) noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    if (outstanding > 0U) {
      --outstanding;
    }
    if (streaming && !queue_locked(index)) {
      broken = true;
    }
  }

  UniqueFd fd;
  v4l2_buf_type type;
  std::vector<BufferMemory> buffers;
  std::mutex mutex;
  std::size_t outstanding{0U};
  bool streaming{false};
  bool broken{false};
};

class BufferLease {
public:
  BufferLease(std::shared_ptr<V4L2State> state, std::uint32_t index)
      : state_(std::move(state)), index_(index) {}
  ~BufferLease() { state_->release(index_); }

private:
  std::shared_ptr<V4L2State> state_;
  std::uint32_t index_;
};

Mapping map_region(int fd, std::size_t length, std::uint32_t offset) {
  void *address = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                         static_cast<off_t>(offset));
  if (address == MAP_FAILED) {
    throw_system_error("mmap V4L2 buffer");
  }
  return Mapping(address, length);
}

std::uint32_t greatest_common_divisor(std::uint32_t left,
                                      std::uint32_t right) noexcept {
  while (right != 0U) {
    const auto remainder = left % right;
    left = right;
    right = remainder;
  }
  return left;
}

Timestamp timestamp_from(const v4l2_buffer &buffer) noexcept {
  Timestamp timestamp;
  timestamp.seconds = buffer.timestamp.tv_sec;
  timestamp.nanoseconds = static_cast<std::uint32_t>(buffer.timestamp.tv_usec) * 1000U;
  const auto timestamp_type = buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK;
  if (timestamp_type == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
    timestamp.clock = TimestampClock::Monotonic;
  } else {
    timestamp.clock = TimestampClock::Unknown;
  }
  return timestamp;
}

class V4L2Camera final : public Camera {
public:
  explicit V4L2Camera(CaptureConfig requested) : config_(std::move(requested)) {
    auto fd = open_device(config_.device);
    capabilities_ = enumerate_fd(fd.get());
    if (!capabilities_.streaming) {
      throw CameraError(ErrorCode::Unsupported,
                        config_.device + " does not support V4L2 streaming I/O");
    }

    CaptureMode mode = config_.capture_mode;
    if (mode == CaptureMode::Auto) {
      selected_fourcc_ = select_advertised_fourcc(
          capabilities_, CaptureMode::SinglePlane, config_.pixel_format);
      if (selected_fourcc_ != 0U) {
        mode = CaptureMode::SinglePlane;
      } else {
        selected_fourcc_ = select_advertised_fourcc(
            capabilities_, CaptureMode::MultiPlane, config_.pixel_format);
        if (selected_fourcc_ != 0U) {
          mode = CaptureMode::MultiPlane;
        } else {
          throw CameraError(ErrorCode::Unsupported,
                            std::string("device does not advertise pixel format ") +
                                to_string(config_.pixel_format));
        }
      }
    }
    if ((mode == CaptureMode::SinglePlane && !capabilities_.single_plane) ||
        (mode == CaptureMode::MultiPlane && !capabilities_.multi_plane)) {
      throw CameraError(ErrorCode::Unsupported,
                        std::string("device does not support ") + to_string(mode) +
                            " capture");
    }
    if (selected_fourcc_ == 0U) {
      selected_fourcc_ =
          select_advertised_fourcc(capabilities_, mode, config_.pixel_format);
    }
    if (selected_fourcc_ == 0U) {
      throw CameraError(ErrorCode::Unsupported,
                        std::string("device does not advertise ") +
                            to_string(config_.pixel_format) + " for " +
                            to_string(mode));
    }
    config_.capture_mode = mode;
    const auto type = mode == CaptureMode::SinglePlane
                          ? V4L2_BUF_TYPE_VIDEO_CAPTURE
                          : V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    state_ = std::make_shared<V4L2State>(std::move(fd), type);
    configure_format();
    configure_frame_rate();
    allocate_buffers();
  }

  ~V4L2Camera() override { stop_capture(); }

  const CaptureConfig &config() const noexcept override { return config_; }

  const Capabilities &capabilities() const noexcept override { return capabilities_; }

  void start() override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->streaming) {
      throw CameraError(ErrorCode::InvalidState, "V4L2 camera is already running");
    }
    if (state_->broken) {
      throw CameraError(ErrorCode::InvalidState,
                        "V4L2 camera entered an unrecoverable queue state");
    }
    if (state_->outstanding != 0U) {
      throw CameraError(
          ErrorCode::InvalidState,
          "cannot restart V4L2 camera while captured frames are still leased");
    }
    for (std::uint32_t index = 0U; index < state_->buffers.size(); ++index) {
      if (!state_->queue_locked(index)) {
        state_->broken = true;
        throw_system_error("VIDIOC_QBUF");
      }
    }
    auto type = state_->type;
    if (retry_ioctl(state_->fd.get(), VIDIOC_STREAMON, &type) < 0) {
      state_->broken = true;
      throw_system_error("VIDIOC_STREAMON");
    }
    state_->streaming = true;
  }

  Frame read(std::uint32_t timeout_ms) override {
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      if (!state_->streaming) {
        throw CameraError(ErrorCode::InvalidState,
                          "V4L2 camera must be started before read");
      }
      if (state_->broken) {
        throw CameraError(ErrorCode::SystemError,
                          "V4L2 buffer queue failed while releasing a frame");
      }
    }

    pollfd descriptor;
    std::memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fd = state_->fd.get();
    descriptor.events = POLLIN | POLLPRI;
    const int poll_timeout = timeout_ms > static_cast<std::uint32_t>(INT_MAX)
                                 ? INT_MAX
                                 : static_cast<int>(timeout_ms);
    int poll_result = 0;
    do {
      poll_result = ::poll(&descriptor, 1U, poll_timeout);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result == 0) {
      throw CameraError(ErrorCode::Timeout,
                        "timed out waiting for a V4L2 capture frame");
    }
    if (poll_result < 0) {
      throw_system_error("poll V4L2 device");
    }

    v4l2_buffer buffer;
    std::memset(&buffer, 0, sizeof(buffer));
    buffer.type = state_->type;
    buffer.memory = V4L2_MEMORY_MMAP;
    std::vector<v4l2_plane> dequeued_planes;
    if (state_->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
      dequeued_planes.resize(VIDEO_MAX_PLANES);
      std::memset(dequeued_planes.data(), 0,
                  sizeof(v4l2_plane) * dequeued_planes.size());
      buffer.m.planes = dequeued_planes.data();
      buffer.length = static_cast<__u32>(dequeued_planes.size());
    }

    std::shared_ptr<void> lease;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      if (!state_->streaming) {
        throw CameraError(ErrorCode::InvalidState,
                          "V4L2 camera stopped while waiting for a frame");
      }
      if (retry_ioctl(state_->fd.get(), VIDIOC_DQBUF, &buffer) < 0) {
        if (errno == EAGAIN) {
          throw CameraError(ErrorCode::Timeout, "V4L2 frame was not ready after poll");
        }
        throw_system_error("VIDIOC_DQBUF");
      }
      if (buffer.index >= state_->buffers.size()) {
        state_->broken = true;
        throw CameraError(ErrorCode::SystemError,
                          "V4L2 returned an out-of-range buffer index");
      }
      ++state_->outstanding;
      try {
        lease = std::static_pointer_cast<void>(
            std::make_shared<BufferLease>(state_, buffer.index));
      } catch (...) {
        --state_->outstanding;
        if (!state_->queue_locked(buffer.index)) {
          state_->broken = true;
        }
        throw;
      }
    }

    std::vector<Plane> planes;
    const auto &memory = state_->buffers[buffer.index];
    if (state_->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
      Plane plane;
      plane.data = static_cast<const std::uint8_t *>(memory.planes.front().address());
      plane.bytes_used =
          std::min<std::size_t>(buffer.bytesused, memory.planes.front().length());
      plane.length = memory.planes.front().length();
      plane.stride = strides_.front();
      planes.push_back(plane);
    } else {
      const auto plane_count = memory.planes.size();
      if (plane_count == 0U || plane_count > dequeued_planes.size()) {
        throw CameraError(ErrorCode::SystemError,
                          "V4L2 returned an invalid plane count");
      }
      planes.reserve(plane_count);
      for (std::size_t index = 0U; index < plane_count; ++index) {
        const auto &dequeued = dequeued_planes[index];
        const auto mapping_length = memory.planes[index].length();
        const auto offset = std::min<std::size_t>(dequeued.data_offset, mapping_length);
        Plane plane;
        plane.data =
            static_cast<const std::uint8_t *>(memory.planes[index].address()) + offset;
        plane.bytes_used = dequeued.bytesused > offset
                               ? std::min<std::size_t>(dequeued.bytesused - offset,
                                                       mapping_length - offset)
                               : 0U;
        plane.length = mapping_length - offset;
        plane.stride = index < strides_.size() ? strides_[index] : 0U;
        planes.push_back(plane);
      }
    }

    return FrameBuilder::build(std::move(planes), config_.width, config_.height,
                               config_.pixel_format, config_.capture_mode,
                               buffer.sequence, timestamp_from(buffer),
                               std::move(lease));
  }

  void stop() noexcept override { stop_capture(); }

  bool running() const noexcept override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->streaming;
  }

private:
  void stop_capture() noexcept {
    if (!state_) {
      return;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->streaming) {
      return;
    }
    auto type = state_->type;
    retry_ioctl(state_->fd.get(), VIDIOC_STREAMOFF, &type);
    state_->streaming = false;
  }
  void configure_format() {
    const auto fourcc = selected_fourcc_;
    if (fourcc == 0U) {
      throw CameraError(ErrorCode::Unsupported,
                        "pixel format has no V4L2 representation");
    }

    v4l2_format format;
    std::memset(&format, 0, sizeof(format));
    format.type = state_->type;
    if (state_->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
      format.fmt.pix.width = config_.width;
      format.fmt.pix.height = config_.height;
      format.fmt.pix.pixelformat = fourcc;
      format.fmt.pix.field = V4L2_FIELD_ANY;
    } else {
      format.fmt.pix_mp.width = config_.width;
      format.fmt.pix_mp.height = config_.height;
      format.fmt.pix_mp.pixelformat = fourcc;
      format.fmt.pix_mp.field = V4L2_FIELD_ANY;
    }
    if (retry_ioctl(state_->fd.get(), VIDIOC_S_FMT, &format) < 0) {
      throw_system_error("VIDIOC_S_FMT");
    }

    const auto actual_fourcc = state_->type == V4L2_BUF_TYPE_VIDEO_CAPTURE
                                   ? format.fmt.pix.pixelformat
                                   : format.fmt.pix_mp.pixelformat;
    if (pixel_format_from_v4l2(actual_fourcc) != config_.pixel_format) {
      throw CameraError(ErrorCode::Unsupported,
                        "V4L2 driver substituted a different pixel format");
    }
    strides_.clear();
    if (state_->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
      config_.width = format.fmt.pix.width;
      config_.height = format.fmt.pix.height;
      strides_.push_back(format.fmt.pix.bytesperline);
    } else {
      config_.width = format.fmt.pix_mp.width;
      config_.height = format.fmt.pix_mp.height;
      if (format.fmt.pix_mp.num_planes == 0U ||
          format.fmt.pix_mp.num_planes > VIDEO_MAX_PLANES) {
        throw CameraError(ErrorCode::SystemError,
                          "V4L2 driver returned an invalid format plane count");
      }
      for (std::uint32_t index = 0U; index < format.fmt.pix_mp.num_planes; ++index) {
        strides_.push_back(format.fmt.pix_mp.plane_fmt[index].bytesperline);
      }
    }
  }

  void configure_frame_rate() {
    constexpr std::uint32_t kRateScale = 1000U;
    auto denominator = static_cast<std::uint32_t>(
        std::llround(config_.frame_rate * static_cast<double>(kRateScale)));
    auto numerator = kRateScale;
    const auto divisor = greatest_common_divisor(numerator, denominator);
    numerator /= divisor;
    denominator /= divisor;

    v4l2_streamparm parameters;
    std::memset(&parameters, 0, sizeof(parameters));
    parameters.type = state_->type;
    parameters.parm.capture.timeperframe.numerator = numerator;
    parameters.parm.capture.timeperframe.denominator = denominator;
    if (retry_ioctl(state_->fd.get(), VIDIOC_S_PARM, &parameters) == 0) {
      const auto &actual = parameters.parm.capture.timeperframe;
      if (actual.numerator != 0U && actual.denominator != 0U) {
        config_.frame_rate = static_cast<double>(actual.denominator) /
                             static_cast<double>(actual.numerator);
      }
    } else if (errno != EINVAL && errno != ENOTTY) {
      throw_system_error("VIDIOC_S_PARM");
    }
  }

  void allocate_buffers() {
    v4l2_requestbuffers request;
    std::memset(&request, 0, sizeof(request));
    request.count = config_.buffer_count;
    request.type = state_->type;
    request.memory = V4L2_MEMORY_MMAP;
    if (retry_ioctl(state_->fd.get(), VIDIOC_REQBUFS, &request) < 0) {
      throw_system_error("VIDIOC_REQBUFS");
    }
    if (request.count < 2U) {
      throw CameraError(ErrorCode::Unsupported,
                        "V4L2 driver allocated fewer than two buffers");
    }
    config_.buffer_count = request.count;
    state_->buffers.reserve(request.count);
    for (std::uint32_t index = 0U; index < request.count; ++index) {
      v4l2_buffer buffer;
      std::memset(&buffer, 0, sizeof(buffer));
      buffer.type = state_->type;
      buffer.memory = V4L2_MEMORY_MMAP;
      buffer.index = index;
      std::vector<v4l2_plane> planes;
      if (state_->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        planes.resize(VIDEO_MAX_PLANES);
        std::memset(planes.data(), 0, sizeof(v4l2_plane) * planes.size());
        buffer.m.planes = planes.data();
        buffer.length = static_cast<__u32>(planes.size());
      }
      if (retry_ioctl(state_->fd.get(), VIDIOC_QUERYBUF, &buffer) < 0) {
        throw_system_error("VIDIOC_QUERYBUF");
      }

      BufferMemory memory;
      if (state_->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        memory.planes.push_back(
            map_region(state_->fd.get(), buffer.length, buffer.m.offset));
      } else {
        if (buffer.length == 0U || buffer.length > VIDEO_MAX_PLANES) {
          throw CameraError(ErrorCode::SystemError,
                            "V4L2 query returned an invalid plane count");
        }
        for (std::uint32_t plane = 0U; plane < buffer.length; ++plane) {
          memory.planes.push_back(map_region(state_->fd.get(), planes[plane].length,
                                             planes[plane].m.mem_offset));
        }
      }
      state_->buffers.push_back(std::move(memory));
    }
  }

  CaptureConfig config_;
  Capabilities capabilities_;
  std::shared_ptr<V4L2State> state_;
  std::vector<std::uint32_t> strides_;
  std::uint32_t selected_fourcc_{0U};
};

} // namespace

std::uint32_t pixel_format_to_v4l2(PixelFormat format, CaptureMode mode) noexcept {
  switch (format) {
  case PixelFormat::MJPEG:
    return V4L2_PIX_FMT_MJPEG;
  case PixelFormat::H264:
    return V4L2_PIX_FMT_H264;
  case PixelFormat::YUYV:
    return V4L2_PIX_FMT_YUYV;
  case PixelFormat::UYVY:
    return V4L2_PIX_FMT_UYVY;
  case PixelFormat::RGB24:
    return V4L2_PIX_FMT_RGB24;
  case PixelFormat::BGR24:
    return V4L2_PIX_FMT_BGR24;
  case PixelFormat::NV12:
    return mode == CaptureMode::MultiPlane ? V4L2_PIX_FMT_NV12M : V4L2_PIX_FMT_NV12;
  case PixelFormat::GREY:
    return V4L2_PIX_FMT_GREY;
  case PixelFormat::Unknown:
    return 0U;
  }
  return 0U;
}

PixelFormat pixel_format_from_v4l2(std::uint32_t fourcc) noexcept {
  switch (fourcc) {
  case V4L2_PIX_FMT_MJPEG:
  case V4L2_PIX_FMT_JPEG:
    return PixelFormat::MJPEG;
  case V4L2_PIX_FMT_H264:
    return PixelFormat::H264;
  case V4L2_PIX_FMT_YUYV:
    return PixelFormat::YUYV;
  case V4L2_PIX_FMT_UYVY:
    return PixelFormat::UYVY;
  case V4L2_PIX_FMT_RGB24:
    return PixelFormat::RGB24;
  case V4L2_PIX_FMT_BGR24:
    return PixelFormat::BGR24;
  case V4L2_PIX_FMT_NV12:
  case V4L2_PIX_FMT_NV12M:
    return PixelFormat::NV12;
  case V4L2_PIX_FMT_GREY:
    return PixelFormat::GREY;
  default:
    return PixelFormat::Unknown;
  }
}

std::uint32_t select_advertised_fourcc(const Capabilities &capabilities,
                                       CaptureMode mode, PixelFormat format) noexcept {
  for (const auto &candidate : capabilities.formats) {
    if (candidate.capture_mode == mode && candidate.pixel_format == format) {
      // Preserve the exact code advertised by the driver. JPEG and MJPEG, for
      // example, share the same public semantic format but are not
      // interchangeable during VIDIOC_S_FMT negotiation.
      return candidate.fourcc;
    }
  }
  return 0U;
}

std::unique_ptr<Camera> make_v4l2_camera(const CaptureConfig &config) {
  return std::unique_ptr<Camera>(new V4L2Camera(config));
}

Capabilities enumerate_v4l2(const std::string &device) {
  auto fd = open_device(device);
  return enumerate_fd(fd.get());
}

} // namespace camera
} // namespace xgc2
