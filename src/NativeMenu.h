#pragma once

#include <filesystem>

namespace native_menu {
bool Install(const std::filesystem::path& archive, const std::filesystem::path& queue);
}
