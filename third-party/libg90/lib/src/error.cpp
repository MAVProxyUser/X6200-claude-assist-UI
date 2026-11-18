#include <g90/error.hpp>

namespace g90 {

std::string_view to_string(Error e) noexcept {
  switch (e) {
  case Error::InvalidLength:
    return "InvalidLength";
  case Error::BadSync:
    return "BadSync";
  case Error::BadCrc:
    return "BadCrc";
  case Error::InvalidMode:
    return "InvalidMode";
  case Error::InvalidFilter:
    return "InvalidFilter";
  case Error::InvalidFftScale:
    return "InvalidFftScale";
  case Error::InvalidProfile:
    return "InvalidProfile";
  case Error::Timeout:
    return "Timeout";
  case Error::ShortRead:
    return "ShortRead";
  case Error::IoError:
    return "IoError";
  case Error::NotImplemented:
    return "NotImplemented";
  default:
    return "Unknown";
  }
}

} // namespace g90
