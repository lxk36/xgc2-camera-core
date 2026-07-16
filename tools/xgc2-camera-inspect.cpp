#include <xgc2/camera/camera.hpp>
#include <xgc2/camera/version.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace camera = xgc2::camera;

namespace {

struct Options {
  camera::CaptureConfig config;
  std::uint32_t capture_count{0U};
  std::uint32_t timeout_ms{2000U};
};

void usage(std::ostream &output) {
  output << "xgc2-camera-inspect " << XGC2_CAMERA_VERSION_STRING << "\n"
         << "Usage: xgc2-camera-inspect [options]\n"
         << "  --device PATH            V4L2 device (default /dev/video0)\n"
         << "  --synthetic              use deterministic CI backend\n"
         << "  --format FORMAT          mjpeg,h264,yuyv,uyvy,rgb24,bgr24,nv12,grey\n"
         << "  --mode MODE              auto,single,multi\n"
         << "  --width PIXELS           requested width\n"
         << "  --height PIXELS          requested height\n"
         << "  --fps RATE               requested frame rate\n"
         << "  --buffers COUNT          mmap buffer count\n"
         << "  --capture COUNT          capture and summarize COUNT frames\n"
         << "  --timeout-ms MS          per-frame timeout\n"
         << "  --help                    show this help\n";
}

std::uint32_t parse_uint(const std::string &value, const char *option) {
  std::size_t parsed = 0U;
  const auto result = std::stoull(value, &parsed, 10);
  if (parsed != value.size() || result > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument(std::string("invalid value for ") + option);
  }
  return static_cast<std::uint32_t>(result);
}

Options parse_options(int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--help" || argument == "-h") {
      usage(std::cout);
      std::exit(0);
    }
    if (argument == "--synthetic") {
      options.config.backend = camera::BackendKind::Synthetic;
      continue;
    }
    if (index + 1 >= argc) {
      throw std::invalid_argument("missing value for " + argument);
    }
    const std::string value(argv[++index]);
    if (argument == "--device") {
      options.config.device = value;
    } else if (argument == "--format") {
      options.config.pixel_format = camera::pixel_format_from_string(value);
      if (options.config.pixel_format == camera::PixelFormat::Unknown) {
        throw std::invalid_argument("unknown pixel format: " + value);
      }
    } else if (argument == "--mode") {
      options.config.capture_mode = camera::capture_mode_from_string(value);
    } else if (argument == "--width") {
      options.config.width = parse_uint(value, "--width");
    } else if (argument == "--height") {
      options.config.height = parse_uint(value, "--height");
    } else if (argument == "--fps") {
      options.config.frame_rate = std::stod(value);
    } else if (argument == "--buffers") {
      options.config.buffer_count = parse_uint(value, "--buffers");
    } else if (argument == "--capture") {
      options.capture_count = parse_uint(value, "--capture");
    } else if (argument == "--timeout-ms") {
      options.timeout_ms = parse_uint(value, "--timeout-ms");
    } else {
      throw std::invalid_argument("unknown option: " + argument);
    }
  }
  return options;
}

std::string fourcc_string(std::uint32_t fourcc) {
  if (fourcc == 0U) {
    return "----";
  }
  std::string result(4U, ' ');
  for (std::uint32_t index = 0U; index < 4U; ++index) {
    result[index] = static_cast<char>((fourcc >> (index * 8U)) & 0xffU);
  }
  return result;
}

void print_capabilities(const camera::Capabilities &capabilities) {
  std::cout << "driver=" << capabilities.driver << " card=" << capabilities.card
            << " bus=" << capabilities.bus_info << '\n'
            << "streaming=" << (capabilities.streaming ? "yes" : "no")
            << " single_plane=" << (capabilities.single_plane ? "yes" : "no")
            << " multi_plane=" << (capabilities.multi_plane ? "yes" : "no") << '\n';
  for (const auto &format : capabilities.formats) {
    std::cout << "format=" << camera::to_string(format.pixel_format)
              << " fourcc=" << fourcc_string(format.fourcc)
              << " mode=" << camera::to_string(format.capture_mode) << " description=\""
              << format.description << "\"\n";
    for (const auto &size : format.frame_sizes) {
      if (size.discrete) {
        std::cout << "  size=" << size.min_width << 'x' << size.min_height;
      } else {
        std::cout << "  size=" << size.min_width << 'x' << size.min_height << ".."
                  << size.max_width << 'x' << size.max_height
                  << " step=" << size.width_step << 'x' << size.height_step;
      }
      if (!size.frame_intervals.empty()) {
        std::cout << " fps=";
        for (std::size_t index = 0U; index < size.frame_intervals.size(); ++index) {
          if (index != 0U) {
            std::cout << ',';
          }
          std::cout << std::fixed << std::setprecision(3)
                    << size.frame_intervals[index].frames_per_second();
        }
      }
      std::cout << '\n';
    }
  }
}

std::uint32_t frame_checksum(const camera::Frame &frame) {
  std::uint32_t checksum = 2166136261U;
  for (const auto &plane : frame.planes()) {
    for (std::size_t index = 0U; index < plane.bytes_used; ++index) {
      checksum ^= plane.data[index];
      checksum *= 16777619U;
    }
  }
  return checksum;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto options = parse_options(argc, argv);
    if (options.config.backend == camera::BackendKind::V4L2 &&
        options.capture_count == 0U) {
      print_capabilities(camera::enumerate_v4l2(options.config.device));
      return 0;
    }

    auto device = camera::make_camera(options.config);
    print_capabilities(device->capabilities());
    if (options.capture_count == 0U) {
      return 0;
    }
    device->start();
    for (std::uint32_t count = 0U; count < options.capture_count; ++count) {
      const auto frame = device->read(options.timeout_ms);
      std::size_t total_bytes = 0U;
      for (const auto &plane : frame.planes()) {
        total_bytes += plane.bytes_used;
      }
      std::cout << "frame sequence=" << frame.sequence()
                << " timestamp_ns=" << frame.timestamp_ns()
                << " clock=" << camera::to_string(frame.timestamp().clock)
                << " size=" << frame.width() << 'x' << frame.height()
                << " format=" << camera::to_string(frame.pixel_format())
                << " planes=" << frame.planes().size() << " bytes=" << total_bytes
                << " checksum=0x" << std::hex << frame_checksum(frame) << std::dec
                << '\n';
    }
    device->stop();
    return 0;
  } catch (const camera::CameraError &error) {
    std::cerr << "camera error: " << error.what() << '\n';
    return 2;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    usage(std::cerr);
    return 2;
  }
}
