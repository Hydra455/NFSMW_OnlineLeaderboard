#pragma once

#include "LocalStandings.h"

#include <memory>

namespace online {
using Pages = std::shared_ptr<const std::vector<standings::Page>>;

struct Snapshot {
    Pages pages;
    bool pending = false;
};

std::string Profile();
bool Request();
Snapshot Get();
}

namespace diagnostics {
std::string Status();
}
