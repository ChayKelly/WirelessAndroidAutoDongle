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
            Logger::instance()->info("Teardown: read from %s failed: %s\n", read_name.c_str(), teardownReason(read_errno));
            break;
        }
        else if (len == 0) {
            Logger::instance()->info("Teardown: %s closed the connection (EOF)\n", read_name.c_str());
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
            Logger::instance()->info("Teardown: write to %s failed: %s\n", write_name.c_str(), teardownReason(write_errno));
            break;
        }
        else if (should_exit) {
            break;
        }
    }

    stopForwarding(should_exit);
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
            Logger::instance()->info("Teardown: usb accessory did not connect within 30s\n");
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

    Logger::instance()->info("Forwarding data between TCP and USB\n");
    std::atomic<bool> should_exit = false;
    m_usb_tcp_thread = std::thread(&AAWProxy::forward, this, ProxyDirection::USB_to_TCP, std::ref(should_exit));
    m_tcp_usb_thread = std::thread(&AAWProxy::forward, this, ProxyDirection::TCP_to_USB, std::ref(should_exit));

    m_usb_tcp_thread->join();
    m_usb_tcp_thread = std::nullopt;

    m_tcp_usb_thread->join();
    m_tcp_usb_thread = std::nullopt;

    signal(SIGUSR1, SIG_DFL);

    close(m_usb_fd);
    m_usb_fd = -1;

    close(m_tcp_fd);
    m_tcp_fd = -1;

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
