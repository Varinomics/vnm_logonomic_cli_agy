#include <logonomic_cli_addon/json.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

int main(int argc, char** argv)
{
    namespace addon = logonomic::cli_addon;
    const auto* capture = std::getenv("AGY_TEST_CAPTURE");
    if (!capture || argc < 2) {
        return 2;
    }
    const std::string_view operation(argv[1]);
    if (operation == "provider-event" && argc == 4 && std::string_view(argv[2]) == "acknowledge") {
        std::ofstream(capture, std::ios::app) << "{\"operation\":\"acknowledge\"}\n";
        return 0;
    }
    const std::string bytes{std::istreambuf_iterator<char>(std::cin), {}};
    addon::Json_parse_error failure;
    const auto request = addon::parse_json(bytes, {}, &failure);
    if (!request || !request->is_object()) {
        return 2;
    }
    auto record = addon::Json_value::object();
    record.set("operation", addon::Json_value(operation));
    record.set("request", *request);
    std::ofstream(capture, std::ios::app) << addon::serialize_json(record) << '\n';
    if (operation == "request-title-instruction") {
        const std::string mode = std::getenv("AGY_TEST_CONTEXT") ? std::getenv("AGY_TEST_CONTEXT") : "title";
        if (mode == "refused" || mode == "stale_generation" || mode == "closed_ledger" || mode == "unsupported_capability") {
            auto refused = addon::Json_value::object();
            refused.set("accepted", addon::Json_value(false));
            refused.set("reason", addon::Json_value(mode));
            std::cout << addon::serialize_json(refused) << '\n';
            return 0;
        }
        auto response = addon::Json_value::object();
        response.set("accepted", addon::Json_value(true));
        response.set("instruction_required", addon::Json_value(mode == "title"));
        response.set("instruction", addon::Json_value(mode == "empty" ? "" : mode == "title"
            ? "Set the session title with the supplied host command." : "Host delegation and notice text.\nPreserve this exact context."));
        std::cout << addon::serialize_json(response) << '\n';
        return 0;
    }
    if (operation != "provider-event" || argc != 4 || std::string_view(argv[2]) != "deliver") {
        return 2;
    }
    if (const auto* mode = std::getenv("AGY_TEST_RETRY"); mode && std::string_view(mode) == "1") {
        std::ifstream input(capture);
        int lines = 0;
        for (std::string line; std::getline(input, line);) {
            ++lines;
        }
        if (lines == 1) {
            return 4;
        }
    }
    std::cout << "{\"accepted\":true,\"epoch\":1}\n";
    return 0;
}
