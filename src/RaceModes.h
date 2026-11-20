#pragma once

#include <string_view>

namespace races {
struct Mode {
    const char* name;
    bool hasLaps;
};

inline constexpr Mode Modes[] = {
    {"Sprint", false},
    {"Circuit", true},
    {"Drag", false},
    {"Lap Knockout", true},
    {"Tollbooth", false},
    {"Speedtrap", false},
};

inline const Mode* Find(std::string_view name)
{
    for (const auto& mode : Modes) {
        if (name == mode.name)
            return &mode;
    }
    return nullptr;
}
}
