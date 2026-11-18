#include <array>
#include <g90/log.hpp>

namespace g90::log {
namespace {

constexpr std::size_t kModuleCount = 2;

std::array<Level, kModuleCount>& levels() {
  static std::array<Level, kModuleCount> g{Level::Warn, Level::Warn};
  return g;
}

std::size_t index(Module m) noexcept {
  switch (m) {
  case Module::Codec:
    return 0;
  case Module::Wire:
    return 1;
  }
  return 0;
}

} // namespace

void set_level(Module module, Level level) {
  levels()[index(module)] = level;
}

Level get_level(Module module) noexcept {
  return levels()[index(module)];
}

} // namespace g90::log
