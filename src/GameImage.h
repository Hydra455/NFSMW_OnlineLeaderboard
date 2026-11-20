#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace game_image {
bool Readable(std::uintptr_t address, std::size_t size);
bool MatchesCode(std::uintptr_t address, std::string_view expected);
}
