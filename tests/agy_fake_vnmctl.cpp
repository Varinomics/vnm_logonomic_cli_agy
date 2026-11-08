#include <logonomic_cli_addon/json.h>

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>

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
    const auto* ledger = std::getenv("AGY_TEST_CONTEXT_LEDGER");
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
        if (ledger) {
            const auto* ready = std::getenv("AGY_TEST_CONTEXT_READY");
            const auto* release = std::getenv("AGY_TEST_CONTEXT_RELEASE");
            if (!ready || !release) return 2;
            std::ofstream(ready) << "request waiting\n";
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!std::filesystem::exists(release)) {
                if (std::chrono::steady_clock::now() >= deadline) return 1;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::ifstream source(ledger);
            const std::string current{std::istreambuf_iterator<char>(source), {}};
            const auto activation = addon::parse_json(current, {}, nullptr);
            if (!activation) return 2;
            // Independent public-contract oracle: one-shot consumption may
            // occur only for the exact activation carried by delivery@2.
            const auto* delivery = request->find("delivery");
            const auto* version = delivery ? delivery->find("version") : nullptr;
            const auto* expected = delivery ? delivery->find("activation") : nullptr;
            if (!version || !version->is_integer() || version->as_integer() != 2 || !expected || *expected != *activation) {
                auto refused = addon::Json_value::object();
                refused.set("operation", addon::Json_value("context-refused"));
                if (expected) refused.set("activation", *expected);
                std::ofstream(capture, std::ios::app) << addon::serialize_json(refused) << '\n';
                std::cout << "{\"accepted\":false,\"reason\":\"provider_activation_stale\"}\n";
                return 0;
            }
            auto consumed = addon::Json_value::object();
            consumed.set("operation", addon::Json_value("context-consumed"));
            consumed.set("activation", *activation);
            std::ofstream(capture, std::ios::app) << addon::serialize_json(consumed) << '\n';
            response.set("instruction", addon::Json_value(current));
        }
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
    if (ledger) {
        const auto* event = request->find("event");
        const auto* name = event ? event->find("name") : nullptr;
        if (name && name->is_string() && name->as_string() == "NativeConversation") {
            auto current = addon::Json_value::object();
            current.set("provider_session_id", *event->find("body")->find("conversationId"));
            current.set("transition_nonce", *request->find("transition_nonce"));
            std::ofstream(ledger) << addon::serialize_json(current);
        }
    }
    std::cout << "{\"accepted\":true,\"epoch\":1}\n";
    return 0;
}
