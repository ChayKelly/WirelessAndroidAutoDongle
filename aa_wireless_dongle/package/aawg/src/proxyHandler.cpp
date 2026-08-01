#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <thread>
#include <optional>
#include <atomic>
#include <string>
#include <system_error>

#include "common.h"
#include "usb.h"
#include "bluetoothHandler.h"
#include "proxyHandler.h"

void empty_signal_handler(int signal) {
    // Empty. We don't want to do anything but interrupt the thread.
}

// Name why a read/write ended so each teardown records which failure path fired.
// These are transport-level symptoms, not a root-cause verdict: a timeout means
// data stopped moving, which could be radio congestion, a weak link, or the phone
// simply sending nothing. Read them next to the kernel wifi log (klogd) and the
// heartbeat telemetry rather than treating the label itself as the diagnosis.
static const char *teardownReason(int err) {
    switch (err) {
        case EAGAIN:
#if EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
            // SO_RCVTIMEO/SO_SNDTIMEO expired: no data moved for 30s.
            return "socket timeout after 30s (no data moved)";
        case ETIMEDOUT:
            // TCP keepalive or TCP_USER_TIMEOUT gave up: the kernel declared the
            // connection dead (unacked data or failed keepalive probes).
            return "connection timed out (keepalive/user-timeout)";
        default:
            return strerror(err);
    }
}

ssize_t AAWProxy::readFully(int fd, unsigned char *buffer, size_t nbyte) {
    size_t remaining_bytes = nbyte;
    while (remaining_bytes > 0) {
        ssize_t len = read(fd, buffer, remaining_bytes);

        if (len <= 0) {
            // Error, cannot read more.
            return len;
        }

        buffer += len;
        remaining_bytes -= len;
    }

    return nbyte;
}

ssize_t AAWProxy::writeFully(int fd, unsigned char *buffer, size_t nbyte) {
    size_t remaining_bytes = nbyte;
    while (remaining_bytes > 0) {
        ssize_t len = write(fd, buffer, remaining_bytes);

        if (len <= 0) {
            // Error, cannot write more. Partial writes are handled by retrying above.
            return len;
        }

        buffer += len;
        remaining_bytes -= len;
    }

    return nbyte;
}

ssize_t AAWProxy::readMessage(int fd, unsigned char *buffer, size_t buffer_len) {
    size_t header_length = 4;
    if (ssize_t len = readFully(fd, buffer, header_length); len <= 0) {
        return len;
    }

    size_t message_length = (buffer[2] << 8) + buffer[3];

    constexpr char FRAME_TYPE_FIRST = 1 << 0;
    constexpr char FRAME_TYPE_LAST = 1 << 1;
    constexpr char FRAME_TYPE_MASK = FRAME_TYPE_FIRST | FRAME_TYPE_LAST;
    if ((buffer[1] & FRAME_TYPE_MASK) == FRAME_TYPE_FIRST) { // This means the header is 8 bytes long, we need to read four more bytes.
        message_length += 4;
    }

    if ((header_length + message_length) > buffer_len) {
        // Not enough space in the buffer. This is unexpected.
        errno = EMSGSIZE;
        return -1;
    }

    if (ssize_t len = readFully(fd, buffer + header_length, message_length); len <= 0) {
        return len;
    }

    return header_length + message_length;
}

void AAWProxy::forward(ProxyDirection direction, std::atomic<bool>& should_exit) {
    size_t buffer_len = 16384;
    unsigned char buffer[buffer_len];

    bool read_message;
    int read_fd, write_fd;
    std::string read_name, write_name;
    switch (direction) {
        case ProxyDirection::TCP_to_USB:
            read_message = true;

            read_fd = m_tcp_fd;
            read_name = "TCP";

            write_fd = m_usb_fd;
            write_name = "USB";
            break;
        case ProxyDirection::USB_to_TCP:
            read_message = false;

            read_fd = m_usb_fd;
            read_name = "USB";

            write_fd = m_tcp_fd;
            write_name = "TCP";
            break;
    }

    while (!should_exit) {
        // Read
        ssize_t len = read_message ? readMessage(read_fd, buffer, buffer_len) : read(read_fd, buffer, buffer_len);
        // Capture errno now, before any logging call (vsyslog) can overwrite it,
        // so the teardown reason describes the failed read and not the logger.
        int read_errno = errno;

        if (len <= 0) {
            // Start logging read/write details if there is an error.
            m_log_communication = true;
        }
        if (m_log_communication) {
            Logger::instance()->info("%d bytes read from %s\n", len, read_name.c_str());
        }

        if (len < 0) {
            // On teardown we SIGUSR1 the blocked threads to unstick them, which
            // surfaces as EINTR. That is a coordinated stop, not a failure, so
            // do not log it as one and drown out the real teardown reason.
            if (read_errno == EINTR && should_exit) {
                break;
            }
            // The USB link state at the instant the transfer failed, which is the
            // one moment worth measuring it: "not-attached" means VBUS went away,
            // anything else means it held and the data path failed instead.
            Logger::instance()->info("Teardown: read from %s failed: %s [%s]\n", read_name.c_str(), teardownReason(read_errno), UsbManager::udcStatus().c_str());
            break;
        }
        else if (len == 0) {
            Logger::instance()->info("Teardown: %s closed the connection (EOF) [%s]\n", read_name.c_str(), UsbManager::udcStatus().c_str());
            break;
        }
        else if (should_exit) {
            break;
        }

        // Write
        ssize_t wlen = writeFully(write_fd, buffer, len);
        int write_errno = errno;

        if (wlen <= 0) {
            // Start logging read/write details if there is an error.
            m_log_communication = true;
        }
        if (m_log_communication) {
            Logger::instance()->info("%d bytes written to %s\n", wlen, write_name.c_str());
        }

        if (wlen < 0) {
            if (write_errno == EINTR && should_exit) {
                break;
            }
            Logger::instance()->info("Teardown: write to %s failed: %s [%s]\n", write_name.c_str(), teardownReason(write_errno), UsbManager::udcStatus().c_str());
            break;
        }
        else if (should_exit) {
            break;
        }

        // Count only bytes that actually reached the far side. writeFully returns
        // the full length or a negative, so wlen here is always the whole buffer.
        if (direction == ProxyDirection::TCP_to_USB) {
            m_tcp_to_usb_bytes += static_cast<uint64_t>(wlen);
        } else {
            m_usb_to_tcp_bytes += static_cast<uint64_t>(wlen);
        }
    }

    stopForwarding(should_exit);
}

// A session that stops carrying data without dropping is invisible to everything
// else here. The 30s SO_RCVTIMEO fires only on *zero* bytes, so a trickle keeps it
// armed indefinitely: on Drive 8 a session sat at 1.4 kB/s for 160 seconds, with
// the phone associated at 72.2M and aawgd reporting a live connection throughout,
// and only ended when the USB transfer finally errored. From the driver's seat
// that is a frozen screen for nearly three minutes.
//
// This only reports. It deliberately does not tear the session down, for two
// reasons. The symptom being chased is Android Auto disappearing off the head
// unit, and an over-eager teardown here would manufacture exactly that. And the
// floor below is calibrated against one drive, with no capture yet of what a
// legitimately idle session looks like (head unit switched to radio, screen off).
// Once a drive shows the floor separating real stalls from idle cleanly, turning
// this into a teardown is a two-line change and converts a three-minute freeze
// into a ~7s reconnect.
static constexpr int STALL_WINDOW_SECONDS = 20;

// 10 kB/s. Healthy sessions on this rig never dropped below ~180 kB/s, and the
// observed stalls sat between 0.4 and 7.5 kB/s, so this sits in a gap of more
// than an order of magnitude on both sides.
static constexpr uint64_t STALL_FLOOR_BYTES_PER_SECOND = 10240;

void AAWProxy::monitorThroughput(std::atomic<bool>& should_exit) {
    uint64_t window_start_tcp_usb = m_tcp_to_usb_bytes;
    uint64_t window_start_usb_tcp = m_usb_to_tcp_bytes;
    std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
    bool stalled = false;

    while (!should_exit) {
        // Poll at 100ms rather than sleeping out the whole window, for one reason
        // that is not about accuracy: this thread is joined on teardown, so however
        // long it sleeps is added to every reconnect. The symptom being measured is
        // a ~7s outage, and a monitor that could add a second to it would be
        // corrupting the very number it exists to record.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (should_exit) {
            break;
        }

        // Elapsed from the clock rather than a count of completed sleeps, so a
        // descheduled thread reports the window it actually measured.
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        long long elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - window_start).count();
        if (elapsed < STALL_WINDOW_SECONDS) {
            continue;
        }

        uint64_t tcp_usb = m_tcp_to_usb_bytes - window_start_tcp_usb;
        uint64_t usb_tcp = m_usb_to_tcp_bytes - window_start_usb_tcp;
        uint64_t total_per_second = (tcp_usb + usb_tcp) / static_cast<uint64_t>(elapsed);

        if (total_per_second < STALL_FLOOR_BYTES_PER_SECOND) {
            // Both directions are reported because which one dried up first is the
            // discriminator: TCP_to_USB dry means the phone stopped sending, while
            // TCP_to_USB moving with USB_to_TCP dry means the head unit stopped
            // answering. No capture so far can tell those apart.
            Logger::instance()->info("%s: %llus at %llu B/s (tcp->usb %llu B, usb->tcp %llu B) [%s]\n",
                stalled ? "Stall continuing" : "Stall",
                (unsigned long long)elapsed,
                (unsigned long long)total_per_second,
                (unsigned long long)tcp_usb,
                (unsigned long long)usb_tcp,
                UsbManager::udcStatus().c_str());
            stalled = true;
        } else if (stalled) {
            Logger::instance()->info("Stall cleared: %llu B/s (tcp->usb %llu B, usb->tcp %llu B)\n",
                (unsigned long long)total_per_second,
                (unsigned long long)tcp_usb,
                (unsigned long long)usb_tcp);
            stalled = false;
        }

        window_start_tcp_usb = m_tcp_to_usb_bytes;
        window_start_usb_tcp = m_usb_to_tcp_bytes;
        window_start = now;
    }
}

void AAWProxy::stopForwarding(std::atomic<bool>& should_exit) {
    Logger::instance()->info("Interrupting threads to stop forwarding\n");
    should_exit = true;

    if (m_usb_tcp_thread) {
        pthread_kill(m_usb_tcp_thread->native_handle(), SIGUSR1);
    }

    if (m_tcp_usb_thread) {
        pthread_kill(m_tcp_usb_thread->native_handle(), SIGUSR1);
    }
}

void AAWProxy::handleClient(int server_sock) {
    struct sockaddr client_address;
    socklen_t client_addresslen = sizeof(client_address);
    if ((m_tcp_fd = accept(server_sock, &client_address, &client_addresslen)) < 0) {
        close(server_sock);
        Logger::instance()->info("accept failed: %s\n", strerror(errno));
        return;
    }

    close(server_sock);

    Logger::instance()->info("Tcp server accepted connection\n");

    // Phone connected via TCP, we can stop retrying bluetooth connection
    BluetoothHandler::instance().stopConnectWithRetry();

    if (Config::instance()->getConnectionStrategy() != ConnectionStrategy::USB_FIRST) {
        if (!UsbManager::instance().enableDefaultAndWaitForAccessory(std::chrono::seconds(30))) {
            // Drive 8 hit this three times. The gadget state says whether the head
            // unit ignored an enumerated device or never enumerated one at all.
            Logger::instance()->info("Teardown: usb accessory did not connect within 30s [%s]\n", UsbManager::udcStatus().c_str());
            return;
        }
    }

    Logger::instance()->info("Opening usb accessory\n");
    if ((m_usb_fd = open("/dev/usb_accessory", O_RDWR)) < 0) {
        Logger::instance()->info("error opening /dev/usb_accessory: %s\n", strerror(errno));
        return;
    }

    // Set timeouts on the TCP socket.
    // Generous enough to ride out brief wifi stalls instead of tearing the session down,
    // short enough that a dead link is still detected in reasonable time.
    struct timeval tv = {
        .tv_sec = 30,
        .tv_usec = 0,
    };

    if (setsockopt(m_tcp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv))) {
        Logger::instance()->info("setsockopt failed: %s\n", strerror(errno));
        return;
    }

    if (setsockopt(m_tcp_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv))) {
        Logger::instance()->info("setsockopt SO_SNDTIMEO failed: %s\n", strerror(errno));
    }

    // Forward small latency-critical packets immediately
    int enable = 1;
    if (setsockopt(m_tcp_fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable))) {
        Logger::instance()->info("setsockopt TCP_NODELAY failed: %s\n", strerror(errno));
    }

    // A dropped wireless link often raises no socket error, leaving the proxy waiting
    // forever on a dead connection. Keepalives make the kernel detect this within ~30s
    // so the reconnection logic can run.
    if (setsockopt(m_tcp_fd, SOL_SOCKET, SO_KEEPALIVE, &enable, sizeof(enable))) {
        Logger::instance()->info("setsockopt SO_KEEPALIVE failed: %s\n", strerror(errno));
    }

    // Log if any keepalive tuning was rejected: otherwise the logs would imply
    // a ~30s dead-link detection the kernel never actually accepted.
    int keepidle = 10;
    int keepintvl = 5;
    int keepcnt = 3;
    if (setsockopt(m_tcp_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle))) {
        Logger::instance()->info("setsockopt TCP_KEEPIDLE failed: %s\n", strerror(errno));
    }
    if (setsockopt(m_tcp_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl))) {
        Logger::instance()->info("setsockopt TCP_KEEPINTVL failed: %s\n", strerror(errno));
    }
    if (setsockopt(m_tcp_fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt))) {
        Logger::instance()->info("setsockopt TCP_KEEPCNT failed: %s\n", strerror(errno));
    }

    unsigned int user_timeout_ms = 30000;
    if (setsockopt(m_tcp_fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout_ms, sizeof(user_timeout_ms))) {
        Logger::instance()->info("setsockopt TCP_USER_TIMEOUT failed: %s\n", strerror(errno));
    }

    // One line recording the timeouts we requested (any setsockopt that failed
    // is logged individually above), so a teardown reason can be read against
    // the settings that were meant to produce it.
    Logger::instance()->info(
        "Socket options requested: rcv/snd timeout %lds, keepalive idle %ds/intvl %ds/cnt %d, user timeout %ums, nodelay on\n",
        (long)tv.tv_sec, keepidle, keepintvl, keepcnt, user_timeout_ms);

    // Setup signal handler
    struct sigaction sa;
    sa.sa_handler = empty_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGUSR1, &sa, NULL)) {
        Logger::instance()->info("Adding signal handler failed: %s\n", strerror(errno));
    }

    Logger::instance()->info("Forwarding data between TCP and USB [%s]\n", UsbManager::udcStatus().c_str());
    std::atomic<bool> should_exit = false;
    m_session_start = std::chrono::steady_clock::now();
    m_usb_tcp_thread = std::thread(&AAWProxy::forward, this, ProxyDirection::USB_to_TCP, std::ref(should_exit));
    m_tcp_usb_thread = std::thread(&AAWProxy::forward, this, ProxyDirection::TCP_to_USB, std::ref(should_exit));

    // Guarded, and the reason is the whole point of the previous round. std::thread
    // construction throws std::system_error if pthread_create fails, and an escape
    // from here would be doubly fatal: the exception would unwind past two joinable
    // forwarding threads, and ~std::thread on a joinable thread calls std::terminate.
    // That is precisely the abort class round 8 existed to remove, and it would be
    // absurd to reintroduce it for a diagnostic.
    //
    // This monitor is pure instrumentation, so losing it must never cost the
    // session. Log and carry on: the drive still gets its data, just not the stall
    // lines, and the log says which.
    try {
        m_monitor_thread = std::thread(&AAWProxy::monitorThroughput, this, std::ref(should_exit));
    } catch (const std::system_error& e) {
        m_monitor_thread = std::nullopt;
        Logger::instance()->info("Could not start throughput monitor, continuing without stall detection: %s\n", e.what());
    }

    m_usb_tcp_thread->join();
    m_usb_tcp_thread = std::nullopt;

    m_tcp_usb_thread->join();
    m_tcp_usb_thread = std::nullopt;

    // Joined last, and only after should_exit is already set by stopForwarding, so
    // at most one 100ms poll is being waited on. Optional because the monitor is
    // allowed to have failed to start without taking the session with it.
    if (m_monitor_thread) {
        m_monitor_thread->join();
        m_monitor_thread = std::nullopt;
    }

    signal(SIGUSR1, SIG_DFL);

    close(m_usb_fd);
    m_usb_fd = -1;

    close(m_tcp_fd);
    m_tcp_fd = -1;

    // One line per session, so a drive can be read as "how long did each session
    // last and how much did it actually carry" without deriving it from the wlan0
    // counters. Sessions that ran their whole life below the healthy floor, as the
    // first session of Drive 8 boot 5 did at 7.5 kB/s, are visible here even when
    // they were too short for the stall window to fire.
    long long duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_session_start).count();
    uint64_t tcp_usb = m_tcp_to_usb_bytes;
    uint64_t usb_tcp = m_usb_to_tcp_bytes;
    // Guard the divisor: a session can end in well under a millisecond if the
    // accessory drops immediately, and a rate line must not be the thing that
    // divides by zero.
    uint64_t divisor_ms = duration_ms > 0 ? static_cast<uint64_t>(duration_ms) : 1;
    Logger::instance()->info(
        "Session ended: %lld.%03llds, tcp->usb %llu B (%llu B/s), usb->tcp %llu B (%llu B/s) [%s]\n",
        duration_ms / 1000, duration_ms % 1000,
        (unsigned long long)tcp_usb, (unsigned long long)(tcp_usb * 1000 / divisor_ms),
        (unsigned long long)usb_tcp, (unsigned long long)(usb_tcp * 1000 / divisor_ms),
        UsbManager::udcStatus().c_str());

    Logger::instance()->info("Forwarding stopped\n");
}

std::optional<std::thread> AAWProxy::startServer(int32_t port) {
    Logger::instance()->info("Starting tcp server\n");
    int server_sock;
    if ((server_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        Logger::instance()->info("creating socket failed: %s\n", strerror(errno));
        return std::nullopt;
    }

    int opt = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        Logger::instance()->info("setsockopt failed: %s\n", strerror(errno));
        return std::nullopt;
    }

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_sock, (struct sockaddr*)&address, sizeof(address)) < 0) {
        Logger::instance()->info("bind failed: %s\n", strerror(errno));
        return std::nullopt;
    }

    if (listen(server_sock, 3) < 0) {
        Logger::instance()->info("listen failed: %s\n", strerror(errno));
        return std::nullopt;
    }

    Logger::instance()->info("Tcp server listening on %d\n", port);

    return std::thread(&AAWProxy::handleClient, this, server_sock);
}
