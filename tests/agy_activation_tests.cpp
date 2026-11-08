#include "agy_activation.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>
#include <windows.h>
#include <sddl.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>

namespace {

std::mutex s_read_mutex;
std::condition_variable s_read_ready;
bool s_read_entered = false;
bool s_release_read = false;

BOOL WINAPI capture_read_file(
    HANDLE file,
    LPVOID buffer,
    DWORD bytes,
    LPDWORD count,
    LPOVERLAPPED operation)
{
    const auto result = ReadFile(file, buffer, bytes, count, operation);
    std::unique_lock lock(s_read_mutex);
    s_read_entered = true;
    s_read_ready.notify_all();
    s_read_ready.wait(lock, [] { return s_release_read; });
    return result;
}

} // namespace

// Only this executable pauses the actual source read with its handle open.
// The packaged reader has no test hooks or synchronization dependencies.
#define ReadFile capture_read_file
#include "../src/agy_activation.cpp"
#undef ReadFile

int main()
{
    namespace addon = logonomic::cli_addon;
    addon::Helper_environment environment;
    environment.generation_nonce = std::string(32, 'c');
    environment.session_capability = std::string(64, 'a');
    agy::Activation_file source(environment);
    const agy::Activation original{"conversation-a", std::string(32, 'd'), true};
    const agy::Activation selected{"conversation-b", std::string(32, 'e'), false};
    if (!source.initialize() || !source.select(original)) {
        std::fprintf(stderr, "FAIL: initialize the disposable activation source\n");
        return 1;
    }

    std::optional<agy::Activation> captured;
    std::thread reader([&] { captured = agy::capture_activation(source.path(), environment); });
    bool reached = false;
    {
        std::unique_lock lock(s_read_mutex);
        reached = s_read_ready.wait_for(lock, std::chrono::seconds(5), [] { return s_read_entered; });
    }
    SetLastError(ERROR_SUCCESS);
    const bool replaced = reached && source.select(selected);
    const auto replacement_error = GetLastError();
    SetLastError(ERROR_SUCCESS);
    const bool committed = replaced && source.commit(selected.transition_nonce);
    const auto commit_error = GetLastError();
    {
        const std::lock_guard lock(s_read_mutex);
        s_release_read = true;
    }
    s_read_ready.notify_all();
    reader.join();

    const auto current = agy::capture_activation(source.path(), environment);
    if (!reached || !replaced || !committed || !captured || !current ||
        captured->provider_session_id != original.provider_session_id ||
        captured->transition_nonce != original.transition_nonce || !captured->committed ||
        current->provider_session_id != selected.provider_session_id ||
        current->transition_nonce != selected.transition_nonce || !current->committed)
    {
        std::fprintf(stderr, "FAIL: activation replacement and commit must coexist with an in-flight source reader\n"
            "read=%d replaced=%d replacement_error=%lu committed=%d commit_error=%lu captured=%d current=%d\n",
            reached, replaced, replacement_error, committed, commit_error, captured.has_value(), current.has_value());
        if (captured) {
            std::fprintf(stderr, "captured_id=%s captured_nonce=%s captured_committed=%d\n",
                captured->provider_session_id.c_str(), captured->transition_nonce.c_str(), captured->committed);
        }
        if (current) {
            std::fprintf(stderr, "current_id=%s current_nonce=%s current_committed=%d\n",
                current->provider_session_id.c_str(), current->transition_nonce.c_str(), current->committed);
        }
        return 1;
    }
    std::puts("PASS: replacement and commit preserve an in-flight source snapshot");
    return 0;
}
