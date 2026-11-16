#include "agy_events.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/json.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

namespace addon = logonomic::cli_addon;

void require(bool value, const char* message)
{
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    // Oracle: Antigravity's public PreInvocation hook reports modelName and
    // the invocation/trajectory counters. It does not identify the backend.
    auto payload = *addon::parse_json(R"({"conversationId":"root-native","modelName":"native-model",
        "invocationNum":0,"initialNumSteps":12})", {}, nullptr);
    const std::string nonce(32, 'd');
    const auto first = agy::capture_model_observation("PreInvocation", payload, nonce);
    require(first && first->model == "native-model" && first->provider.empty() && first->agent_id.empty() &&
        first->transition_nonce == nonce, "native model and captured activation must survive without guessing the backend");
    payload.set("invocationNum", addon::Json_value(1));
    payload.set("modelName", addon::Json_value("native-changed"));
    const auto next = agy::capture_model_observation("PreInvocation", payload, nonce);
    require(next && next->sequence > first->sequence && next->model == "native-changed",
        "native counter order must survive helper completion reordering");
    payload.set("initialNumSteps", addon::Json_value(13));
    payload.set("invocationNum", addon::Json_value(0));
    const auto later = agy::capture_model_observation("PreInvocation", payload, nonce);
    require(later && later->sequence > next->sequence, "new executions retain native trajectory order");
    require(!agy::capture_model_observation("Stop", payload, nonce) &&
        !agy::capture_model_observation("PreInvocation", payload, ""), "only source-fenced invocation events have model authority");

    auto body = agy::sanitize_hook("PreInvocation", payload);
    addon::encode_model_updates(body, {*later});
    auto native = addon::Json_value::object();
    native.set("name", addon::Json_value("PreInvocation"));
    native.set("body", body);
    addon::Classify_event_request request;
    request.event.name = "NativeHook";
    request.event.body = addon::Json_value::object();
    request.event.body.set("activation_nonce", addon::Json_value(nonce));
    request.event.body.set("event", native);
    request.ledger.current_epoch = 1;
    request.ledger.current_transition = addon::Ledger_transition{addon::Transition_kind::START, "root-native", nonce};
    require(agy::classify_event(request)->model_updates.empty(), "unnegotiated model facts must remain absent");
    request.model_state_version = addon::k_model_state_version;
    const auto classified = agy::classify_event(request);
    require(classified && classified->model_updates.size() == 1 && classified->observations.size() == 1,
        "native model and existing lifecycle must share the accepted carrier");
    request.ledger.current_transition->nonce = std::string(32, 'e');
    require(agy::classify_event(request)->model_updates.empty(), "a delayed prior activation cannot publish a model");
    request.ledger.current_transition->nonce = nonce;
    body.set("model_state", addon::Json_value("malformed"));
    native.set("body", body);
    request.event.body.set("event", native);
    const auto malformed = agy::classify_event(request);
    require(malformed && malformed->model_failure && malformed->observations.size() == 1,
        "malformed optional model data must preserve valid lifecycle observations");
    payload.set("modelName", addon::Json_value("bad\nmodel"));
    require(!agy::capture_model_observation("PreInvocation", payload, nonce), "control-bearing model labels must be omitted");
    std::cout << "Antigravity source-fenced model observation contracts passed\n";
}
