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

// The root conversation manager records the identity it selected, including
// selection followed by a load failure. Child views use a different marker.
// This proves association only; exact startup readiness still uses redraw.
std::optional<std::string> selected_conversation(std::string_view native_record);

} // namespace agy
