#pragma once

#include <cstdint>

namespace g90::log {

/// @brief Log channel selector for libg90 diagnostics.
enum class Module {
  Codec, ///< Head/body encode-decode and CRC.
  Wire,  ///< DE-9 serial transport.
};

/// @brief Verbosity level for a `Module`.
enum class Level {
  Off,   ///< No output.
  Error, ///< Failures only.
  Warn,  ///< Recoverable issues.
  Info,  ///< High-level progress.
  Debug, ///< Detailed codec decisions.
  Trace, ///< Per-byte wire trace (serial I/O).
};

/// @brief Set the minimum log level for a module.
/// @param module Target channel (`Codec` or `Wire`).
/// @param level Messages below this level are suppressed.
void set_level(Module module, Level level);

/// @brief Query the current log level for a module.
/// @param module Target channel.
/// @return Active minimum level.
[[nodiscard]] Level get_level(Module module) noexcept;

} // namespace g90::log
