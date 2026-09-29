#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agy {

struct Startup_evidence
{
    std::vector<std::string> conversation_ids;
    std::string             native_records;
};

// Only a completed native redraw following that conversation's stream start
// proves loaded identity. Request/selector echoes never qualify.
std::optional<Startup_evidence> observe_startup(std::string_view native_log);

} // namespace agy
