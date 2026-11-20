#include "LeaderboardBrowser.h"
#include "RaceModes.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace standings {
std::vector<const Page*> Browser::Events(const std::vector<Page>& pages) const
{
    std::vector<const Page*> result;
    for (const auto& page : pages) {
        if (page.mode == mode && !page.event.empty()
            && std::none_of(result.begin(), result.end(), [&](const auto* entry) {
                return entry->event == page.event;
            }))
            result.push_back(&page);
    }
    return result;
}

std::vector<const Page*> Browser::Results(const std::vector<Page>& pages) const
{
    std::vector<const Page*> result;
    for (const auto& page : pages) {
        if (page.event == event && page.mode == mode)
            result.push_back(&page);
    }
    return result;
}

Page Browser::View(const std::vector<Page>& pages) const
{
    Page view;
    if (level == Level::Results) {
        const auto results = Results(pages);
        if (results.empty()) {
            view.heading = "No result available";
            return view;
        }
        const auto index = std::min(selected, results.size() - 1);
        view = *results[index];
        if (results.size() > 1)
            view.heading += " [" + std::to_string(index + 1) + "/" + std::to_string(results.size()) + "]";
        return view;
    }

    view.heading = level == Level::Modes ? "Select race type - Enter" : mode + " - Select event";
    std::vector<std::string> labels;
    if (level == Level::Modes) {
        for (const auto& entry : races::Modes)
            labels.emplace_back(entry.name);
    } else {
        for (const auto* entry : Events(pages))
            labels.push_back(entry->heading);
    }
    if (labels.empty()) {
        view.heading = mode + " - No recorded events";
        return view;
    }

    const auto index = std::min(selected, labels.size() - 1);
    const auto first = index / RowsPerPage * RowsPerPage;
    for (size_t row = first; row < labels.size() && row < first + RowsPerPage; ++row) {
        Row choice;
        choice.rank = row == index ? ">" : "";
        choice.profile = labels[row];
        if (level == Level::Events) {
            const auto separator = choice.profile.find(" / ");
            if (separator != std::string::npos) {
                const auto details = choice.profile.substr(separator + 3);
                choice.profile.erase(separator);
                const auto direction = details.rfind(" / ");
                if (direction != std::string::npos) {
                    choice.time = details.substr(direction + 3);
                    const auto laps = details.find(" / ");
                    if (laps != direction)
                        choice.car = details.substr(laps + 3, direction - laps - 3);
                }
            }
        }
        view.rows.push_back(std::move(choice));
    }
    return view;
}

bool Browser::Input(BrowserInput input, const std::vector<Page>& pages)
{
    if (input == BrowserInput::Back) {
        if (level == Level::Modes)
            return false;
        level = level == Level::Results ? Level::Events : Level::Modes;
        selected = 0;
        return true;
    }

    const auto events = Events(pages);
    const auto results = Results(pages);
    const auto count = level == Level::Modes ? std::size(races::Modes)
        : level == Level::Events ? events.size() : results.size();
    if (!count)
        return true;
    selected = std::min(selected, count - 1);
    if (input == BrowserInput::Previous)
        selected = selected == 0 ? count - 1 : selected - 1;
    else if (input == BrowserInput::Next)
        selected = (selected + 1) % count;
    else if (input == BrowserInput::Accept && level == Level::Modes) {
        mode = races::Modes[selected].name;
        level = Level::Events;
        selected = 0;
    } else if (input == BrowserInput::Accept && level == Level::Events) {
        event = events[selected]->event;
        level = Level::Results;
        selected = 0;
    }
    return true;
}

void Browser::Reset()
{
    level = Level::Modes;
    selected = 0;
    mode.clear();
    event.clear();
}
}
