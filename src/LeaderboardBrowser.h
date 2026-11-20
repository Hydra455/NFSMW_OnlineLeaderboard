#pragma once

#include "LocalStandings.h"

namespace standings {
enum class BrowserInput { Previous, Next, Accept, Back };

class Browser {
public:
    Page View(const std::vector<Page>& pages) const;
    bool Input(BrowserInput input, const std::vector<Page>& pages);
    void Reset();

private:
    enum class Level { Modes, Events, Results };
    std::vector<const Page*> Events(const std::vector<Page>& pages) const;
    std::vector<const Page*> Results(const std::vector<Page>& pages) const;
    Level level = Level::Modes;
    size_t selected = 0;
    std::string mode;
    std::string event;
};
}
