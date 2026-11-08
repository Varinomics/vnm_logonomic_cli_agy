#include "agy_startup.h"

#include <algorithm>
#include <map>
#include <regex>

namespace agy {

std::optional<Startup_evidence> observe_startup(std::string_view native_log)
{
    if (native_log.size() > 1024 * 1024 || native_log.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    static const std::string prefix = R"(^I[0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{6} +[0-9]+ )";
    static const std::string identity = R"(([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}))";
    static const std::regex stream(prefix + R"(conversation_manager\.go:[0-9]+\] Streaming conversation )" + identity + "$" );
    static const std::regex redraw(prefix + R"(manager\.go:[0-9]+\] Full redraw completed \((?:rerenderAll|RerenderAll)\) for conversation )" +
        identity + R"( \(epoch [0-9]+, items [0-9]+\)(?: in [0-9]+(?:\.[0-9]+)?(?:ns|µs|us|ms|s))?$)");
    Startup_evidence result;
    std::map<std::string, std::string> streams;
    for (std::size_t start = 0; start < native_log.size();) {
        const auto end = native_log.find('\n', start);
        if (end == std::string_view::npos) break; // A partial last record is not evidence.
        auto line = std::string(native_log.substr(start, end - start));
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 16 * 1024) return std::nullopt;
        std::smatch match;
        if (std::regex_match(line, match, stream)) {
            streams[match[1].str()] = line;
            if (streams.size() > 64) return std::nullopt;
        }
        else
        if (std::regex_match(line, match, redraw)) {
            const auto id = match[1].str();
            const auto found = streams.find(id);
            if (found != streams.end() && std::find(result.conversation_ids.begin(), result.conversation_ids.end(), id) ==
                result.conversation_ids.end())
            {
                result.conversation_ids.push_back(id);
                result.native_records += found->second + '\n' + line + '\n';
            }
        }
    }
    return result;
}

std::optional<std::string> selected_conversation(std::string_view native_record)
{
    if (native_record.empty() || native_record.size() > 1024 || native_record.back() != '\n' ||
        native_record.find('\n') != native_record.size() - 1 || native_record.find('\0') != std::string_view::npos)
    {
        return std::nullopt;
    }
    native_record.remove_suffix(1);
    if (!native_record.empty() && native_record.back() == '\r') native_record.remove_suffix(1);
    static const std::regex selection(
        R"(^I[0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{6} +[0-9]+ conversation_manager\.go:[0-9]+\] Streaming conversation ([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})$)");
    const std::string line(native_record);
    std::smatch match;
    return std::regex_match(line, match, selection) ? std::optional(match[1].str()) : std::nullopt;
}

} // namespace agy
