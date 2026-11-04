// Linked only into the Linux test fixture. Record native call metadata, never
// logger bytes, so the regression measures the wait contract without CPU limits.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pid_t original_child = -1;

static void trace(const char *name, long long detail, long long result, int first, int second)
{
    const int saved_errno = errno;
    static int descriptor = -2;
    if (descriptor == -2) {
        const char *path = getenv("AGY_TEST_NATIVE_TRACE");
        descriptor = path ? open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600) : -1;
    }
    if (descriptor >= 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        char line[192];
        const int bytes = snprintf(line, sizeof(line), "%s %lld %lld %lld %d %d\n", name,
            (long long)now.tv_sec * 1000000000 + now.tv_nsec, detail, result, first, second);
        if (bytes > 0 && bytes < (int)sizeof(line) && write(descriptor, line, (size_t)bytes) != bytes) {
            close(descriptor);
            descriptor = -1;
        }
    }
    errno = saved_errno;
}

extern int __real_nanosleep(const struct timespec *, struct timespec *);
int __wrap_nanosleep(const struct timespec *requested, struct timespec *remaining)
{
    trace("nanosleep", requested->tv_sec * 1000000000LL + requested->tv_nsec, 0, -1, -1);
    return __real_nanosleep(requested, remaining);
}

extern pid_t __real_waitpid(pid_t, int *, int);
pid_t __wrap_waitpid(pid_t child, int *status, int options)
{
    if (child > 0 && (options & WNOHANG)) {
        original_child = child;
    }
    const pid_t result = __real_waitpid(child, status, options);
    trace("waitpid", options, result, child, errno);
    return result;
}

extern ssize_t __real_read(int, void *, size_t);
ssize_t __wrap_read(int descriptor, void *buffer, size_t size)
{
    const ssize_t result = __real_read(descriptor, buffer, size);
    trace("read", (long long)size, result, descriptor, errno);
    return result;
}

extern int __real_poll(struct pollfd *, nfds_t, int);
int __wrap_poll(struct pollfd *descriptors, nfds_t count, int timeout)
{
    const int first = count > 0 ? descriptors[0].fd : -1;
    const int second = count > 1 ? descriptors[1].fd : -1;
    trace("poll", timeout, count, first, second);

    // Pause once at the actual wait boundary. The Python test lets the original
    // child exit before releasing this gate; its SIGCHLD must survive the gap.
    static int gated = 0;
    const char *gate = getenv("AGY_TEST_WAIT_GATE_FD");
    nfds_t active = 0;
    for (nfds_t index = 0; index < count; ++index) {
        if (descriptors[index].fd >= 0) ++active;
    }
    if (timeout < 0 && active == 1 && gate && !gated) {
        gated = 1;
        const int descriptor = atoi(gate);
        trace("gate", 0, 0, descriptor, -1);
        char release;
        while (__real_read(descriptor, &release, 1) < 0 && errno == EINTR) {}
        close(descriptor);
        // WNOWAIT proves exit here while leaving the production waitpid as the
        // authoritative reaper. No /proc mount or pidfd support is assumed.
        siginfo_t status = {0};
        int ready;
        do {
            ready = waitid(P_PID, original_child, &status, WEXITED | WNOWAIT);
        } while (ready < 0 && errno == EINTR);
        trace("exit_before_poll", original_child, ready, status.si_pid, status.si_status);
    }
    const int result = __real_poll(descriptors, count, timeout);
    trace("poll_result", result, count, first, second);
    return result;
}
