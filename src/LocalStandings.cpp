#include "LocalStandings.h"
#include "RaceModes.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <string_view>
#include <tuple>
#include <utility>

namespace standings {
namespace {
namespace fs = std::filesystem;

struct Value {
    std::string text;
    bool quoted = false;
};

class JsonObject {
public:
    explicit JsonObject(std::string_view bytes) : rest(bytes) {}

    bool Parse()
    {
        if (rest.substr(0, 3) == "\xEF\xBB\xBF")
            rest.remove_prefix(3);
        if (!Consume('{'))
            return false;
        if (!Consume('}')) {
            do {
                Space();
                std::string key;
                if (!ReadString(key) || !Consume(':'))
                    return false;
                Space();
                Value value;
                value.quoted = !rest.empty() && rest.front() == '"';
                if (value.quoted ? !ReadString(value.text) : !ReadLiteral(value.text))
                    return false;
                if (!fields.emplace(std::move(key), std::move(value)).second)
                    return false;
            } while (Consume(','));
            if (!Consume('}'))
                return false;
        }
        Space();
        return rest.empty();
    }

    std::string Text(const char* field) const
    {
        const auto found = fields.find(field);
        return found != fields.end() && found->second.quoted ? found->second.text : "";
    }

    std::string_view Literal(const char* field) const
    {
        const auto found = fields.find(field);
        if (found == fields.end() || found->second.quoted)
            return {};
        return found->second.text;
    }

private:
    void Space()
    {
        const auto pos = rest.find_first_not_of(" \t\r\n");
        rest.remove_prefix(pos == std::string_view::npos ? rest.size() : pos);
    }

    bool Consume(char character)
    {
        Space();
        if (rest.empty() || rest.front() != character)
            return false;
        rest.remove_prefix(1);
        return true;
    }

    bool ReadLiteral(std::string& value)
    {
        const auto end = rest.find_first_of(" \t\r\n,}");
        if (end == 0 || rest.empty())
            return false;
        value = rest.substr(0, end);
        rest.remove_prefix(value.size());
        if (value == "true" || value == "false" || value == "null")
            return true;
        int number;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
    }

    bool ReadHex(unsigned& number)
    {
        if (rest.size() < 4)
            return false;
        const auto end = rest.data() + 4;
        const auto parsed = std::from_chars(rest.data(), end, number, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != end)
            return false;
        rest.remove_prefix(4);
        return true;
    }

    bool ReadUnicode(std::string& out)
    {
        unsigned code;
        if (!ReadHex(code))
            return false;
        if (code >= 0xD800 && code <= 0xDBFF) {
            unsigned low;
            if (rest.substr(0, 2) != "\\u")
                return false;
            rest.remove_prefix(2);
            if (!ReadHex(low) || low < 0xDC00 || low > 0xDFFF)
                return false;
            code = 0x10000 + ((code - 0xD800) << 10) + low - 0xDC00;
        } else if (code >= 0xDC00 && code <= 0xDFFF) {
            return false;
        }
        if (code < 0x80) {
            out += static_cast<char>(code);
            return true;
        }
        const unsigned continuation = code < 0x800 ? 1 : (code < 0x10000 ? 2 : 3);
        const unsigned prefix = continuation == 1 ? 0xC0 : (continuation == 2 ? 0xE0 : 0xF0);
        out += static_cast<char>(prefix | (code >> (6 * continuation)));
        for (unsigned shift = 6 * continuation; shift != 0;) {
            shift -= 6;
            out += static_cast<char>(0x80 | ((code >> shift) & 0x3F));
        }
        return true;
    }

    bool ReadString(std::string& out)
    {
        if (!Consume('"'))
            return false;
        while (!rest.empty()) {
            const unsigned char ch = rest.front();
            rest.remove_prefix(1);
            if (ch == '"')
                return true;
            if (ch < 32)
                return false;
            if (ch != '\\') {
                out += static_cast<char>(ch);
                continue;
            }
            if (rest.empty())
                return false;
            const char escape = rest.front();
            rest.remove_prefix(1);
            switch (escape) {
            case '"': case '\\': case '/': out += escape; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u':
                if (!ReadUnicode(out))
                    return false;
                break;
            default: return false;
            }
        }
        return false;
    }

    std::string_view rest;
    std::map<std::string, Value> fields;
};

using EventKey = std::tuple<std::string, std::string, int, bool>;

struct Run {
    EventKey event;
    std::string title;
    std::string profile;
    std::string car;
    int timeMs;
    int place;
    int speed;
    bool mine;
    int leaderTime;
    int leaderSpeed;
};

int Number(std::string_view value)
{
    if (value.empty())
        return -1;
    int number;
    const auto end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, number);
    return parsed.ec == std::errc{} && parsed.ptr == end ? number : -1;
}

std::string SafeLabel(std::string label)
{
    std::replace_if(label.begin(), label.end(), [](unsigned char c) {
        return c < 32 || c == 127;
    }, ' ');
    if (label.find_first_not_of(' ') == std::string::npos)
        return {};
    return label;
}

std::optional<Run> ParseRun(std::string_view data)
{
    JsonObject json(data);
    if (!json.Parse())
        return {};
    auto hash = json.Text("event_hash");
    auto profile = SafeLabel(json.Text("profile"));
    auto mode = json.Text("mode");
    auto title = SafeLabel(json.Text("event_name"));
    if (title.empty())
        title = SafeLabel(json.Text("event_id"));
    if (title.empty())
        title = hash;
    const auto raceMode = races::Find(mode);
    const auto laps = Number(json.Literal("laps"));
    const auto reverse = json.Literal("reversed");
    const auto time = Number(json.Literal("time_ms"));
    const auto speed = Number(json.Literal("speed_milli_kmh"));
    if (hash.size() != 8 || hash.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos
        || hash == "00000000" || profile.empty() || !raceMode
        || laps < 1 || laps > 99 || (!raceMode->hasLaps && laps != 1)
        || (reverse != "true" && reverse != "false") || time < 1000 || time > 21600000 || speed > 1000000000)
        return {};
    for (auto& ch : hash) {
        if (ch >= 'A' && ch <= 'F')
            ch = static_cast<char>(ch + ('a' - 'A'));
    }
    return Run{{std::move(hash), std::move(mode), laps, reverse == "true"},
               std::move(title), std::move(profile), SafeLabel(json.Text("car")), time, Number(json.Literal("place")), speed,
               json.Literal("mine") == "true", Number(json.Literal("leader_time_ms")),
               Number(json.Literal("leader_speed_milli_kmh"))};
}

std::optional<Run> ReadRun(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    char buffer[4097]{};
    file.read(buffer, sizeof(buffer));
    if (!file.eof() || file.gcount() <= 0 || file.gcount() > 4096)
        return {};
    return ParseRun(std::string_view(buffer, static_cast<size_t>(file.gcount())));
}

struct BestRun {
    int time;
    std::string car;
    int speed;
};

struct Board {
    std::string title;
    std::map<std::string, BestRun> best;
};

void ReadDirectory(const fs::path& root, std::map<EventKey, Board>& boards)
{
    std::error_code error;
    const fs::directory_iterator end;
    for (fs::directory_iterator it(root, error); !error && it != end; it.increment(error)) {
        if (it->path().extension() != ".json")
            continue;
        std::error_code entryError;
        if (!it->is_regular_file(entryError))
            continue;
        const auto run = ReadRun(it->path());
        if (!run || (std::get<1>(run->event) == "Speedtrap" && run->speed < 0))
            continue;
        auto& board = boards[run->event];
        if (board.title.empty() || run->title < board.title)
            board.title = run->title;
        const auto found = board.best.try_emplace(run->profile, BestRun{run->timeMs, run->car, run->speed}).first;
        auto& best = found->second;
        const bool speed = std::get<1>(run->event) == "Speedtrap";
        const auto candidate = speed ? -run->speed : run->timeMs;
        const auto current = speed ? -best.speed : best.time;
        if (candidate < current || (candidate == current && !run->car.empty()
            && best.car.empty())) {
            best = {run->timeMs, run->car, run->speed};
        }
    }
}

std::string Shorten(const std::string& label, size_t bytes)
{
    if (label.size() <= bytes)
        return label;
    while (bytes && (static_cast<unsigned char>(label[bytes]) & 0xC0) == 0x80)
        --bytes;
    return label.substr(0, bytes) + "...";
}

std::string FormatTime(int milliseconds)
{
    char text[24]{};
    std::snprintf(text, sizeof(text), "%d:%02d.%03d",
                  milliseconds / 60000, milliseconds / 1000 % 60, milliseconds % 1000);
    return text;
}
std::string CourseHeading(const EventKey& event, const std::string& title)
{
    const auto& [hash, mode, laps, reverse] = event;
    std::string heading = Shorten(title, 36) + " / " + mode;
    if (races::Find(mode)->hasLaps)
        heading += " / " + std::to_string(laps) + (laps == 1 ? " lap" : " laps");
    return heading + (reverse ? " / Reverse" : " / Forward");
}

std::string FormatSpeed(int value)
{
    char text[32]{};
    std::snprintf(text, sizeof(text), "%d.%03d km/h", value / 1000, value % 1000);
    return text;
}

std::string Key(const EventKey& event)
{
    const auto& [hash, mode, laps, reverse] = event;
    return hash + "." + mode + "." + std::to_string(laps) + "." + (reverse ? "1" : "0");
}

Row FormatRow(int rank, const std::string& profile, const std::string& car, int value,
              int leader, bool speed, bool mine)
{
    return {std::to_string(rank), Shorten(profile, 26),
            car.empty() ? "-" : Shorten(car, 32), speed ? FormatSpeed(value) : FormatTime(value),
            profile, mine, value, leader};
}

}

std::vector<Page> LoadPages(const fs::path& archive, const fs::path& queue, std::string_view profile)
{
    std::map<EventKey, Board> boards;
    ReadDirectory(queue, boards);
    ReadDirectory(archive, boards);
    if (boards.empty())
        return {{"No race results yet", {}, {}, {}, {}}};

    using BoardEntry = std::map<EventKey, Board>::value_type;
    std::vector<const BoardEntry*> ordered;
    for (const auto& event : boards)
        ordered.push_back(&event);
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        return std::tie(left->second.title, left->first) < std::tie(right->second.title, right->first);
    });

    std::vector<Page> pages;
    for (const auto* event : ordered) {
        const auto& board = event->second;
        const auto heading = CourseHeading(event->first, board.title);
        std::vector<std::pair<int, std::string>> ranked;
        const bool speed = std::get<1>(event->first) == "Speedtrap";
        for (const auto& [name, best] : board.best)
            ranked.emplace_back(speed ? -best.speed : best.time, name);
        std::sort(ranked.begin(), ranked.end());
        size_t rank = 0;
        for (size_t row = 0; row < ranked.size(); ++row) {
            if (row % RowsPerPage == 0)
                pages.push_back({heading, {}, Key(event->first), std::get<1>(event->first), {}});
            if (row == 0 || ranked[row].first != ranked[row - 1].first)
                rank = row + 1;
            const auto& [time, profile] = ranked[row];
            const auto& car = board.best.at(profile).car;
            const int value = speed ? -time : time;
            const int leader = speed ? -ranked.front().first : ranked.front().first;
            pages.back().rows.push_back(FormatRow(static_cast<int>(rank), profile, car, value, leader, speed, false));
        }
    }
    MarkPersonal(pages, profile);
    return pages;
}

std::string PersonalSummary(const Row& row, bool speed)
{
    const auto gap = speed ? row.leader - row.value : row.value - row.leader;
    return "You #" + row.rank + "  " + row.time
        + (gap <= 0 ? "  Leader" : "  Gap " + (speed ? FormatSpeed(gap) : FormatTime(gap)));
}

void MarkPersonal(std::vector<Page>& pages, std::string_view profile)
{
    std::map<std::string, std::string> summaries;
    for (auto& page : pages) {
        for (auto& row : page.rows) {
            if (!profile.empty())
                row.mine = row.fullProfile == profile;
            if (row.mine)
                summaries[page.event] = PersonalSummary(row, page.mode == "Speedtrap");
        }
    }
    for (auto& page : pages) {
        const auto summary = summaries.find(page.event);
        page.personal = summary == summaries.end() ? "You have no recorded result here" : summary->second;
    }
}

std::vector<Page> ParseSnapshot(std::string_view payload)
{
    constexpr std::string_view version = "MWLeaderboard/2\n";
    constexpr std::string_view legacy = "MWLeaderboard/1\n";
    if (payload.substr(0, version.size()) != version && payload.substr(0, legacy.size()) != legacy)
        return {};
    payload.remove_prefix(version.size());

    std::map<EventKey, std::vector<Run>> courses;
    size_t count = 0;
    while (!payload.empty()) {
        const auto newline = payload.find('\n');
        if (newline == std::string_view::npos || newline > 4096 || ++count > 5000)
            return {};
        auto run = ParseRun(payload.substr(0, newline));
        if (!run || run->place < 1 || run->place > 1000000)
            return {};
        if (std::get<1>(run->event) != "Speedtrap" || run->speed >= 0)
            courses[run->event].push_back(std::move(*run));
        payload.remove_prefix(newline + 1);
    }

    for (auto& [event, runs] : courses) {
        std::sort(runs.begin(), runs.end(), [](const auto& left, const auto& right) {
            return std::tie(left.place, left.profile) < std::tie(right.place, right.profile);
        });
    }
    if (courses.empty())
        return {{"No shared race results yet", {}, {}, {}, {}}};

    std::vector<Page> pages;
    for (const auto& [event, runs] : courses) {
        const auto heading = CourseHeading(event, runs.front().title);
        for (size_t row = 0; row < runs.size(); ++row) {
            if (row % RowsPerPage == 0)
                pages.push_back({heading, {}, Key(event), std::get<1>(event), {}});
            const auto& run = runs[row];
            const bool speed = std::get<1>(event) == "Speedtrap";
            const int leader = speed ? run.leaderSpeed : run.leaderTime;
            const auto& first = runs.front();
            pages.back().rows.push_back(FormatRow(run.place, run.profile, run.car,
                speed ? run.speed : run.timeMs, leader >= 0 ? leader : (speed ? first.speed : first.timeMs),
                speed, run.mine));
        }
    }
    MarkPersonal(pages, {});
    return pages;
}
}
