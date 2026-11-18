#pragma once

#include <string_view>

namespace g90 {

/// @brief Semantic version string for this libg90 build.
/// @return Dotted version (`"0.1.0"`).
[[nodiscard]] constexpr std::string_view version_string() noexcept {
  return "0.1.0";
}

} // namespace g90
