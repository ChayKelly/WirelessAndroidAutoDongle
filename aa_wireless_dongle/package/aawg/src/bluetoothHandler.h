#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

#include "bluetoothCommon.h"

class BluezAdapterProxy;
class AAWirelessProfile;
class HSPHSProfile;
class BLEAdvertisement;

class BluetoothHandler {
public:
    static BluetoothHandler& instance();

    // False means talking to bluez failed outright, which is recoverable by restarting this
    // process and is therefore the caller's cue to exit. It does NOT mean "no adapter":
    // a board with no bluetooth hardware still returns true, because restarting cannot
    // conjure hardware and a restart loop would be the worse bug.
    bool init();

    // Distinguishes "no bluetooth hardware at all", which is permanent and must not be
    // retried, from "the adapter is there but will not power on", which must be.
    bool hasAdapter() const;

    // Returns false if the adapter could not be powered on. Callers must handle that:
    // an unhandled D-Bus throw from here terminated the daemon 23 times on Drive 7.
    bool powerOn();
    void powerOff();

    std::optional<std::thread> connectWithRetry();
    void stopConnectWithRetry();
    void handshakeStarted();
    void handshakeEnded();

private:
    enum class ConnectResult {
        NoDevices,    // nothing known to connect to yet, so nothing has failed
        Connected,
        Unreachable,  // known device, not connected, would not connect: phone is away or asleep
    };

    BluetoothHandler() {};
    BluetoothHandler(BluetoothHandler const&);
    BluetoothHandler& operator=(BluetoothHandler const&);

    // Out-param rather than a return value so an empty enumeration ("bluez answered, there
    // is nothing there") stays distinguishable from a failed one ("bluez did not answer").
    // Collapsing those two was how a transient startup failure became a permanently
    // bluetooth-less daemon instead of a restart that would have fixed it.
    bool getBluezObjects(DBus::ManagedObjects& objects);

    bool initAdapter();
    bool setPower(bool on);
    bool setPairable(bool pairable);
    bool exportProfiles();
    ConnectResult connectDevice();
    void disconnectDevice();
    void resetAdapter(int attempts);

    bool startAdvertising();
    void stopAdvertising();

    void retryConnectLoop();
    bool stopRequested();
    bool waitForStop(std::chrono::seconds timeout);

    std::mutex m_retryMutex;
    std::condition_variable m_retryCondition;
    bool m_stopRequested = false;
    std::atomic<bool> m_handshakeInProgress{false};
    std::chrono::steady_clock::time_point m_handshakeEnded;

    std::shared_ptr<DBus::Dispatcher> m_dispatcher;
    std::shared_ptr<DBus::Connection> m_connection;
    std::shared_ptr<BluezAdapterProxy> m_adapter;

    std::shared_ptr<AAWirelessProfile> m_aawProfile;
    std::shared_ptr<HSPHSProfile> m_hspProfile;

    std::shared_ptr<BLEAdvertisement> m_leAdvertisement;

    std::string m_adapterAlias;
};
