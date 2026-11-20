#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace standings {
inline constexpr size_t RowsPerPage = 10;

struct Row {
    std::string rank;
    std::string profile;
    std::string car;
    std::string time;
    std::string fullProfile;
    bool mine = false;
    int value = 0;
    int leader = 0;
};

struct Page {
    std::string heading;
    std::vector<Row> rows;
    std::string event;
    std::string mode;
    std::string personal;
};

std::vector<Page> LoadPages(const std::filesystem::path& archive,
                            const std::filesystem::path& queue, std::string_view profile = {});
std::vector<Page> ParseSnapshot(std::string_view payload);
std::string PersonalSummary(const Row& row, bool speed);
void MarkPersonal(std::vector<Page>& pages, std::string_view profile);
}
