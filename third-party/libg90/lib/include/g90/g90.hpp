#pragma once

/// @file g90.hpp
/// @brief Umbrella include for the libg90 public API.
///
/// Pulls in codec types (head/body frames), CRC, defaults, logging, version
/// metadata, and the DE-9 serial transport rim. Prefer including specific
/// headers when you depend on only one module.

#include <g90/body.hpp>
#include <g90/crc.hpp>
#include <g90/de9.hpp>
#include <g90/defaults.hpp>
#include <g90/error.hpp>
#include <g90/head.hpp>
#include <g90/log.hpp>
#include <g90/types.hpp>
#include <g90/version.hpp>
