#include "agy_startup.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/startup_observer.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <arpa/inet.h>
#include <csignal>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

bool post(const addon::Startup_observer_offer& offer, const std::string& body)
{
    const auto colon = offer.endpoint.find(':', 7);
    const auto slash = offer.endpoint.find('/', colon);
    const auto port = static_cast<unsigned short>(std::stoi(offer.endpoint.substr(colon + 1, slash - colon - 1)));
#ifdef _WIN32
    const HINTERNET session = WinHttpOpen(L"Logonomic AGY startup", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 1000, 1000, 2000, 2000);
    const HINTERNET connection = WinHttpConnect(session, L"127.0.0.1", port, 0);
    const HINTERNET request = connection ? WinHttpOpenRequest(connection, L"POST", L"/events", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
    const auto headers = addon::wide("Content-Type: application/json\r\nAuthorization: " + offer.authorization + "\r\n");
    DWORD status = 0;
    DWORD bytes = sizeof(status);
    const bool accepted = request && WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()),
        const_cast<char*>(body.data()), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
        WinHttpReceiveResponse(request, nullptr) && WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &bytes, nullptr) && status == 200;
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return accepted;
#else
    const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket < 0) return false;
    const timeval timeout{2, 0};
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool accepted = connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    const auto request = "POST /events HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nAuthorization: " +
        offer.authorization + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    for (std::size_t sent = 0; accepted && sent < request.size();) {
        const auto count = send(socket, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (count <= 0) accepted = false;
        else sent += static_cast<std::size_t>(count);
    }
    char response[128]{};
    std::size_t received = 0;
    while (accepted && received < 12) {
        const auto count = recv(socket, response + received, sizeof(response) - received, 0);
        if (count <= 0) accepted = false;
        else received += static_cast<std::size_t>(count);
    }
    accepted = accepted && std::string_view(response, received).starts_with("HTTP/1.1 200");
    close(socket);
    return accepted;
#endif
}

struct Child
{
#ifdef _WIN32
    HANDLE process = nullptr;
#else
    pid_t process = -1;
    int status = 0;
    bool ended = false;
#endif

    ~Child()
    {
#ifdef _WIN32
        if (process) finish(true);
#else
        if (process > 0) finish(true);
#endif
    }

    bool live()
    {
#ifdef _WIN32
        return WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
#else
        if (!ended) ended = waitpid(process, &status, WNOHANG) == process;
        return !ended;
#endif
    }

    int finish(bool terminate)
    {
#ifdef _WIN32
        if (terminate && live()) TerminateProcess(process, 3);
        WaitForSingleObject(process, INFINITE);
        DWORD code = 3;
        GetExitCodeProcess(process, &code);
        CloseHandle(process);
        process = nullptr;
        return static_cast<int>(code);
#else
        if (terminate && live()) kill(process, SIGKILL);
        if (!ended) {
            while (waitpid(process, &status, 0) < 0 && errno == EINTR) {}
        }
        process = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
    }
};

bool start(Child& child, const std::vector<std::string>& arguments)
{
#ifdef _WIN32
    std::wstring command;
    for (const auto& argument : arguments) {
        if (!command.empty()) command += L' ';
        command += addon::quote_argument(addon::wide(argument));
    }
    HANDLE inherited[3]{};
    const DWORD identifiers[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (int i = 0; i < 3; ++i) {
        if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(identifiers[i]), GetCurrentProcess(),
            &inherited[i], 0, TRUE, DUPLICATE_SAME_ACCESS))
        {
            for (int j = 0; j < i; ++j) CloseHandle(inherited[j]);
            return false;
        }
    }
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    const bool initialized = InitializeProcThreadAttributeList(attributes, 1, 0, &bytes) != FALSE;
    PROCESS_INFORMATION process{};
    bool started = false;
    if (initialized && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited, sizeof(inherited), nullptr, nullptr))
    {
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = inherited[0];
        startup.StartupInfo.hStdOutput = inherited[1];
        startup.StartupInfo.hStdError = inherited[2];
        startup.lpAttributeList = attributes;
        const DWORD console = GetConsoleCP() != 0 ? 0 : CREATE_NO_WINDOW;
        started = CreateProcessW(addon::wide(arguments.front()).c_str(), command.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | console, nullptr, nullptr,
            &startup.StartupInfo, &process) != FALSE;
    }
    if (initialized) DeleteProcThreadAttributeList(attributes);
    for (const auto handle : inherited) CloseHandle(handle);
    if (!started) return false;
    CloseHandle(process.hThread);
    child.process = process.hProcess;
    // The native child receives the console event; the bridge waits and cleans up.
    SetConsoleCtrlHandler([](DWORD event) -> BOOL { return event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT; }, TRUE);
    return true;
#else
    child.process = fork();
    if (child.process < 0) return false;
    if (child.process == 0) {
        std::vector<char*> argv;
        for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        execv(argv.front(), argv.data());
        _exit(3);
    }
    signal(SIGINT, SIG_IGN);
    return true;
#endif
}

int run(std::vector<std::string> arguments)
{
    const char* offered = std::getenv("AGY_LOGONOMIC_STARTUP_OBSERVER");
    const char* root = std::getenv("AGY_LOGONOMIC_STARTUP_ROOT");
    if (!offered || !root || arguments.empty()) return 2;
    const auto value = addon::parse_json(offered, {}, nullptr);
    addon::Contract_failure failure;
    const auto offer = value ? addon::decode_startup_observer_offer(*value, &failure) : std::nullopt;
    if (!offer || offer->confirmation != "observed-identity") return 2;
    const auto parent = fs::u8path(root);
    if (!fs::is_directory(fs::symlink_status(parent))) return 2;
#ifdef _WIN32
    if ((GetFileAttributesW(parent.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return 2;
#endif
    const auto directory = parent / ("agy-startup-" + addon::random_nonce_hex());
    if (!fs::create_directory(directory)) return 3;
#ifndef _WIN32
    fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace);
#endif
    struct Cleanup {
        fs::path directory;
        ~Cleanup() { std::error_code error; fs::remove(directory / "native.log", error); fs::remove(directory, error); }
    } cleanup{directory};
    const auto log = directory / "native.log";
    const auto utf8 = log.u8string();
    arguments.insert(arguments.end(), {"--log-file", std::string(utf8.begin(), utf8.end())});
    Child child;
    if (!start(child, arguments)) return 3;
    bool confirmed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (child.live() && std::chrono::steady_clock::now() < deadline) {
        std::error_code error;
        const auto status = fs::symlink_status(log, error);
        if (!error && fs::is_regular_file(status)) {
#ifdef _WIN32
            if ((GetFileAttributesW(log.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0) break;
#endif
            const auto size = fs::file_size(log, error);
            if (error || size > 1024 * 1024) break;
            std::ifstream stream(log, std::ios::binary);
            std::string bytes(static_cast<std::size_t>(size), '\0');
            stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            bytes.resize(static_cast<std::size_t>(stream.gcount()));
            const auto evidence = agy::observe_startup(bytes);
            if (!evidence) break;
            if (!evidence->conversation_ids.empty()) {
                auto body = addon::Json_value::object();
                body.set("native_records", addon::Json_value(evidence->native_records));
                confirmed = post(*offer, addon::serialize_json(body));
                break;
            }
        }
        else if (!error && fs::exists(status)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    const int code = child.finish(!confirmed);
    return confirmed ? code : 3;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i) {
        const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (count <= 0) return 2;
        std::string argument(static_cast<std::size_t>(count), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, argument.data(), count, nullptr, nullptr);
        argument.pop_back();
        arguments.push_back(std::move(argument));
    }
    try { return run(std::move(arguments)); } catch (...) { return 3; }
}
#else
int main(int argc, char** argv)
{
    try { return run(std::vector<std::string>(argv + 1, argv + argc)); } catch (...) { return 3; }
}
#endif
