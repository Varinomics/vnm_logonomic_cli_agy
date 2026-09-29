#include "agy_startup.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/startup_observer.h>

#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#include <winhttp.h>
#else
#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
// The native logger writes into a kernel-bounded pipe. No transcript path is
// created, and custody termination closes the resources without C++ unwinding.
struct Log_pipe
{
    std::string path;
#ifdef _WIN32
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    OVERLAPPED connection{};
    bool connected = false;
#else
    int reader = -1;
    int writer = -1;
#endif

    ~Log_pipe()
    {
#ifdef _WIN32
        if (pipe != INVALID_HANDLE_VALUE) { CancelIoEx(pipe, nullptr); CloseHandle(pipe); }
        if (event) CloseHandle(event);
#else
        if (reader >= 0) close(reader);
        if (writer >= 0) close(writer);
#endif
    }

    bool initialize()
    {
#ifdef _WIN32
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        DWORD bytes = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
        std::vector<unsigned char> storage(bytes);
        const bool valid = GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes) != FALSE;
        CloseHandle(token);
        LPWSTR sid = nullptr;
        if (!valid || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid)) return false;
        const auto sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
        LocalFree(sid);
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return false;
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
        path = "\\\\.\\pipe\\logonomic-agy-" + addon::random_nonce_hex();
        pipe = CreateNamedPipeW(addon::wide(path).c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &security);
        LocalFree(descriptor);
        if (pipe == INVALID_HANDLE_VALUE) return false;
        event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) return false;
        connection.hEvent = event;
        connected = ConnectNamedPipe(pipe, &connection) != FALSE;
        const auto error = connected ? ERROR_SUCCESS : GetLastError();
        if (error == ERROR_PIPE_CONNECTED) connected = true;
        return connected || error == ERROR_IO_PENDING;
#else
        int descriptors[2]{};
        if (pipe2(descriptors, O_CLOEXEC) != 0) return false;
        reader = descriptors[0];
        writer = descriptors[1];
        if (fcntl(reader, F_SETFL, O_NONBLOCK) < 0 || fcntl(writer, F_SETFD, 0) < 0) return false;
        path = "/proc/self/fd/" + std::to_string(writer);
        return true;
#endif
    }

    void started()
    {
#ifndef _WIN32
        close(writer);
        writer = -1;
#endif
    }

    std::optional<std::string> read_available()
    {
        char buffer[65536];
#ifdef _WIN32
        if (!connected) {
            if (WaitForSingleObject(event, 0) != WAIT_OBJECT_0) return std::string{};
            DWORD bytes = 0;
            if (!GetOverlappedResult(pipe, &connection, &bytes, FALSE)) return std::nullopt;
            connected = true;
        }
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return std::nullopt;
        if (!available) return std::string{};
        ResetEvent(event);
        OVERLAPPED reading{};
        reading.hEvent = event;
        DWORD bytes = 0;
        if (!ReadFile(pipe, buffer, std::min<DWORD>(available, sizeof(buffer)), &bytes, &reading)) {
            if (GetLastError() != ERROR_IO_PENDING) return std::nullopt;
            if (WaitForSingleObject(event, 2000) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &reading);
                GetOverlappedResult(pipe, &reading, &bytes, TRUE);
                return std::nullopt;
            }
            if (!GetOverlappedResult(pipe, &reading, &bytes, FALSE)) return std::nullopt;
        }
        return std::string(buffer, bytes);
#else
        const auto bytes = read(reader, buffer, sizeof(buffer));
        if (bytes < 0 && (errno == EAGAIN || errno == EINTR)) return std::string{};
        if (bytes <= 0) return std::nullopt;
        return std::string(buffer, static_cast<std::size_t>(bytes));
#endif
    }
};

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
    if (!offered || arguments.empty()) return 2;
    const auto value = addon::parse_json(offered, {}, nullptr);
    addon::Contract_failure failure;
    const auto offer = value ? addon::decode_startup_observer_offer(*value, &failure) : std::nullopt;
    if (!offer || offer->confirmation != "observed-identity") return 2;
    Log_pipe log;
    if (!log.initialize()) return 3;
    arguments.insert(arguments.end(), {"--log-file", log.path});
    Child child;
    if (!start(child, arguments)) return 3;
    log.started();
    bool confirmed = false;
    std::string startup;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (child.live()) {
        const auto bytes = log.read_available();
        if (!confirmed) {
            if (!bytes || std::chrono::steady_clock::now() >= deadline) break;
            if (startup.size() + bytes->size() > 1024 * 1024) break;
            startup += *bytes;
            const auto evidence = agy::observe_startup(startup);
            if (!evidence) break;
            if (!evidence->conversation_ids.empty()) {
                auto body = addon::Json_value::object();
                body.set("native_records", addon::Json_value(evidence->native_records));
                confirmed = post(*offer, addon::serialize_json(body));
                if (!confirmed) break;
                startup.clear();
                startup.shrink_to_fit();
            }
        }
        // After confirmation, drain and discard. Neither live output nor a
        // force-stopped session leaves an on-disk native transcript.
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
