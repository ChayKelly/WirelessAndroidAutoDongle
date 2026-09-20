#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>

class AAWProxy {
public:
    std::optional<std::thread> startServer(int32_t port);
    bool endedOnUsbError() const;

private:
    enum class ProxyDirection {
        TCP_to_USB,
        USB_to_TCP
    };

    void handleClient(int server_fd);
    void forward(ProxyDirection direction, std::atomic<bool>& should_exit);
    void stopForwarding(std::atomic<bool>& should_exit);

    // Watches the byte counters below and logs when a session goes quiet without
    // dropping. Runs on its own thread so it keeps sampling while both forwarding
    // threads are blocked in read().
    void monitorThroughput(std::atomic<bool>& should_exit);

    ssize_t readFully(int fd, unsigned char *buf, size_t nbyte);
    ssize_t writeFully(int fd, unsigned char *buf, size_t nbyte);
    ssize_t readMessage(int fd, unsigned char *buf, size_t nbyte);

    int m_usb_fd = -1;
    int m_tcp_fd = -1;

    std::optional<std::thread> m_usb_tcp_thread = std::nullopt;
    std::optional<std::thread> m_tcp_usb_thread = std::nullopt;
    std::optional<std::thread> m_monitor_thread = std::nullopt;

    std::atomic<bool> m_log_communication = false;
    std::atomic<bool> m_usb_error{false};

    // Bytes successfully forwarded this session, split by direction.
    //
    // TCP_to_USB carries the phone's rendered video to the head unit and is the
    // large one: a healthy session on this rig runs 180 to 550 kB/s. USB_to_TCP is
    // the much smaller return channel (touch and control events). They are split
    // because "which side stopped first" is precisely what the Drive 8 stalls
    // could not be read for, the only throughput figure in that capture being the
    // wlan0 interface counter at 45s granularity, which is neither per-session nor
    // per-direction.
    //
    // A fresh AAWProxy is constructed for every session in main(), so these reset
    // naturally and always describe one session.
    std::atomic<uint64_t> m_tcp_to_usb_bytes{0};
    std::atomic<uint64_t> m_usb_to_tcp_bytes{0};
    std::chrono::steady_clock::time_point m_session_start;
};
