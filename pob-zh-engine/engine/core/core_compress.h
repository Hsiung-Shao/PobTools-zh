#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <gsl/span>
#include <zstd.h>

std::optional<std::vector<char>> CompressZstandard(gsl::span<const std::byte> src, std::optional<int> level = {});

std::optional<std::vector<char>> DecompressZstandard(gsl::span<const std::byte> src);
