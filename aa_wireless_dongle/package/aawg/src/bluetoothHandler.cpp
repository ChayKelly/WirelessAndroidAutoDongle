#include <stdio.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include "common.h"
#include "bluetoothHandler.h"
#include "bluetoothProfiles.h"
#include "bluetoothAdvertisement.h"

// Escalation thresholds, counted in consecutive failed retry rounds 20s apart.
//
// A device that claims to be Connected but whose profile will not complete is stale bluez
// state, and power cycling the controller can genuinely clear that, so escalate quickly.
// A device that is simply not connected usually means the phone is away, asleep, or has
// stopped accepting connections, and no amount of resetting the local controller can make
// a remote device answer. That case escalates slowly, as a long shot rather than a fix.
static constexpr int STALE_FAILURES_BEFORE_ADAPTER_RESET = 3;
static constexpr int ABSENT_FAILURES_BEFORE_ADAPTER_RESET = 12;

// Each reset doubles the threshold up to this ceiling (8 minutes at a 20s retry). Without
// it, a phone left at home would power cycle the adapter every minute for the whole drive.
static constexpr int MAX_FAILURES_BEFORE_ADAPTER_RESET = 24;

static constexpr std::chrono::seconds ADAPTER_RESET_SETTLE_TIME{2};

static constexpr const char* ADAPTER_ALIAS_PREFIX = "WirelessAADongle-";
static constexpr const char* ADAPTER_ALIAS_DONGLE_PREFIX = "AndroidAuto-Dongle-";

static constexpr const char* BLUEZ_BUS_NAME = "org.bluez";
static constexpr const char* BLUEZ_ROOT_OBJECT_PATH = "/";
static constexpr const char* BLUEZ_OBJECT_PATH = "/org/bluez";

static constexpr const char* INTERFACE_BLUEZ_ADAPTER = "org.bluez.Adapter1";
static constexpr const char* INTERFACE_BLUEZ_LE_ADVERTISING_MANAGER = "org.bluez.LEAdvertisingManager1";

static constexpr const char* INTERFACE_BLUEZ_DEVICE = "org.bluez.Device1";
static constexpr const char* INTERFACE_BLUEZ_PROFILE_MANAGER = "org.bluez.ProfileManager1";

static constexpr const char* LE_ADVERTISEMENT_OBJECT_PATH = "/com/aawgd/bluetooth/advertisement";

static constexpr const char* AAWG_PROFILE_OBJECT_PATH = "/com/aawgd/bluetooth/aawg";
static constexpr const char* AAWG_PROFILE_UUID = "4de17a00-52cb-11e6-bdf4-0800200c9a66";

static constexpr const char* HSP_HS_PROFILE_OBJECT_PATH = "/com/aawgd/bluetooth/hsp";
static constexpr const char* HSP_AG_UUID = "00001112-0000-1000-8000-00805f9b34fb";
static constexpr const char* HSP_HS_UUID = "00001108-0000-1000-8000-00805f9b34fb";


class BluezAdapterProxy: private DBus::ObjectProxy {
    BluezAdapterProxy(std::shared_ptr<DBus::Connection> conn, DBus::Path path): DBus::ObjectProxy(conn, BLUEZ_BUS_NAME, path) {
        alias = this->create_property<std::string>(INTERFACE_BLUEZ_ADAPTER, "Alias");
        powered = this->create_property<bool>(INTERFACE_BLUEZ_ADAPTER, "Powered");
        discoverable = this->create_property<bool>(INTERFACE_BLUEZ_ADAPTER, "Discoverable");
        pairable = this->create_property<bool>(INTERFACE_BLUEZ_ADAPTER, "Pairable");

        registerAdvertisement = this->create_method<void(DBus::Path, DBus::Properties)>(INTERFACE_BLUEZ_LE_ADVERTISING_MANAGER, "RegisterAdvertisement");
        unregisterAdvertisement = this->create_method<void(DBus::Path)>(INTERFACE_BLUEZ_LE_ADVERTISING_MANAGER, "UnregisterAdvertisement");
    }

public:
    static std::shared_ptr<BluezAdapterProxy> create(std::shared_ptr<DBus::Connection> conn, DBus::Path path)
    {
      return std::shared_ptr<BluezAdapterProxy>(new BluezAdapterProxy(conn, path));
    }

    std::shared_ptr<DBus::PropertyProxy<std::string>> alias;
    std::shared_ptr<DBus::PropertyProxy<bool>> powered;
    std::shared_ptr<DBus::PropertyProxy<bool>> discoverable;
    std::shared_ptr<DBus::PropertyProxy<bool>> pairable;

    std::shared_ptr<DBus::MethodProxy<void(DBus::Path, DBus::Properties)>> registerAdvertisement;
    std::shared_ptr<DBus::MethodProxy<void(DBus::Path)>> unregisterAdvertisement;
};


// Every outbound D-Bus call can throw, and most of them had no handler anywhere up the
// stack, so one bluez or controller fault took the whole daemon down. Drive 2 died on
// DBus::ErrorNoReply. Drive 7 died 23 times in a single boot on DBus::Error
// "Authentication Failed", thrown by setPower(true) once the controller stopped answering
// HCI_Reset (kernel: "hci0: Opcode 0x0c03 failed: -110"). aawgd-supervise respawned it
// every 5.3s for two minutes and none of it could ever work, because restarting a process
// cannot reset a wedged controller.
//
// Failing to power the adapter is a fault to report and retry. It is not a reason to abort,
// so every outbound call goes through here and reports instead of throwing.
template <typename Call>
static bool dbusTry(const char* what, Call&& call) {
    try {
        call();
        return true;
    } catch (const DBus::Error& e) {
        Logger::instance()->info("D-Bus call failed (%s): %s\n", what, e.what());
    } catch (const std::exception& e) {
        Logger::instance()->info("D-Bus call failed (%s): %s\n", what, e.what());
    } catch (...) {
        Logger::instance()->info("D-Bus call failed (%s): unknown exception\n", what);
    }
    return false;
}

BluetoothHandler& BluetoothHandler::instance() {
    static BluetoothHandler instance;
    return instance;
}

bool BluetoothHandler::getBluezObjects(DBus::ManagedObjects& objects) {
    return dbusTry("GetManagedObjects", [&] {
        std::shared_ptr<DBus::ObjectProxy> m_bluezRootObject = m_connection->create_object_proxy(BLUEZ_BUS_NAME, BLUEZ_ROOT_OBJECT_PATH);
        DBus::MethodProxy getManagedObjects = *(m_bluezRootObject->create_method<DBus::ManagedObjects(void)>("org.freedesktop.DBus.ObjectManager", "GetManagedObjects"));

        objects = getManagedObjects();
    });
}

bool BluetoothHandler::initAdapter() {
    DBus::ManagedObjects objects;

    // Failing to enumerate is not the same as enumerating nothing. Carrying on here would
    // leave m_adapter null for the rest of the drive with no way back, where the throw this
    // replaced took the process down and the supervisor restarted into a working bluez.
    if (!getBluezObjects(objects)) {
        Logger::instance()->info("Could not enumerate bluez objects\n");
        return false;
    }

    std::string adapter_path;
    for (auto const& [path, interfaces]: objects) {
        for (auto const& [interface, properties]: interfaces) {
            if (interface == INTERFACE_BLUEZ_ADAPTER) {
                adapter_path = path;
                Logger::instance()->info("Using bluetooth adapter at path: %s\n", path.c_str());
                break;
            }
        }
        if (!adapter_path.empty()) {
            break;
        }
    }

    if (adapter_path.empty()) {
        // bluez answered and there is genuinely no adapter. Restarting cannot change that,
        // so this is a success: the daemon runs on without bluetooth, as upstream does.
        Logger::instance()->info("Did not find any bluetooth adapters\n");
    }
    else {
        m_adapter = BluezAdapterProxy::create(m_connection, adapter_path);

        // A cosmetic setting, so a failure here must not cost us the adapter itself.
        if (dbusTry("Adapter.Alias", [&] { m_adapter->alias->set_value(m_adapterAlias); })) {
            Logger::instance()->info("Bluetooth adapter alias: %s\n", m_adapterAlias.c_str());
        }
    }

    return true;
}

bool BluetoothHandler::setPower(bool on) {
    if (!m_adapter) {
        return false;
    }

    if (!dbusTry("Adapter.Powered", [&] { m_adapter->powered->set_value(on); })) {
        return false;
    }

    // Logged only on success, deliberately. The absence of this line before each of Drive 7's
    // aborts is what proved the throw was in set_value and not in the setPairable that follows.
    Logger::instance()->info("Bluetooth adapter was powered %s\n", on ? "on" : "off");
    return true;
}

bool BluetoothHandler::setPairable(bool pairable) {
    if (!m_adapter) {
        return false;
    }

    // Both are attempted even if the first fails, hence the deliberate operand order.
    bool ok = dbusTry("Adapter.Discoverable", [&] { m_adapter->discoverable->set_value(pairable); });
    ok = dbusTry("Adapter.Pairable", [&] { m_adapter->pairable->set_value(pairable); }) && ok;

    if (ok) {
        Logger::instance()->info("Bluetooth adapter is now discoverable and pairable\n");
    }
    return ok;
}

bool BluetoothHandler::exportProfiles() {
    // Both are held at function scope so the method proxy outlives the object proxy it
    // came from, matching how connectDevice keeps its device proxy alive.
    std::shared_ptr<DBus::ObjectProxy> bluezObject;
    std::shared_ptr<DBus::MethodProxy<void(DBus::Path, std::string, DBus::Properties)>> registerProfile;

    // Every failure below is reported rather than swallowed. Without the AA Wireless profile
    // the phone has no way to start a session at all, so a daemon that keeps running without
    // it is a dongle that is up, silent, and unfixable short of a power cycle.
    if (!dbusTry("ProfileManager proxy", [&] {
        bluezObject = m_connection->create_object_proxy(BLUEZ_BUS_NAME, BLUEZ_OBJECT_PATH);
        registerProfile = bluezObject->create_method<void(DBus::Path, std::string, DBus::Properties)>(INTERFACE_BLUEZ_PROFILE_MANAGER, "RegisterProfile");
    }) || !registerProfile) {
        Logger::instance()->info("Could not reach the bluez profile manager\n");
        return false;
    }

    // Register AA Wireless Profile
    m_aawProfile = AAWirelessProfile::create(AAWG_PROFILE_OBJECT_PATH);
    if (m_connection->register_object(m_aawProfile, DBus::ThreadForCalling::DispatcherThread) != DBus::RegistrationStatus::Success) {
        Logger::instance()->info("Failed to register AA Wireless profile\n");
    }

    if (!dbusTry("RegisterProfile(AA Wireless)", [&] {
        (*registerProfile)(AAWG_PROFILE_OBJECT_PATH, AAWG_PROFILE_UUID, {
            {"Name", DBus::Variant("AA Wireless")},
            {"Role", DBus::Variant("server")},
            {"Channel", DBus::Variant(uint16_t(8))},
        });
    })) {
        return false;
    }
    Logger::instance()->info("Bluetooth AA Wireless profile active\n");

    if (Config::instance()->getConnectionStrategy() != ConnectionStrategy::DONGLE_MODE) {
        // Register HSP Handset profile
        m_hspProfile = HSPHSProfile::create(HSP_HS_PROFILE_OBJECT_PATH);
        if (m_connection->register_object(m_hspProfile, DBus::ThreadForCalling::DispatcherThread) != DBus::RegistrationStatus::Success) {
            Logger::instance()->info("Failed to register HSP Handset profile\n");
        }
        if (!dbusTry("RegisterProfile(HSP HS)", [&] {
            (*registerProfile)(HSP_HS_PROFILE_OBJECT_PATH, HSP_HS_UUID, {
                {"Name", DBus::Variant("HSP HS")},
            });
        })) {
            return false;
        }
        Logger::instance()->info("HSP Handset profile active\n");
    }

    return true;
}

bool BluetoothHandler::startAdvertising() {
    if (!m_adapter) {
        return false;
    }

    // Register Advertisement Object
    m_leAdvertisement = BLEAdvertisement::create(LE_ADVERTISEMENT_OBJECT_PATH);

    m_leAdvertisement->type->set_value("peripheral");
    m_leAdvertisement->serviceUUIDs->set_value(std::vector<std::string>{AAWG_PROFILE_UUID});
    m_leAdvertisement->localName->set_value(m_adapterAlias);

    if (m_connection->register_object(m_leAdvertisement, DBus::ThreadForCalling::DispatcherThread) != DBus::RegistrationStatus::Success) {
        Logger::instance()->info("Failed to register BLE Advertisement\n");
    }

    if (!dbusTry("RegisterAdvertisement", [&] { (*m_adapter->registerAdvertisement)(LE_ADVERTISEMENT_OBJECT_PATH, {}); })) {
        return false;
    }

    Logger::instance()->info("BLE Advertisement started\n");
    return true;
}

void BluetoothHandler::stopAdvertising() {
    if (!m_adapter) {
        return;
    }

    if (dbusTry("UnregisterAdvertisement", [&] { (*m_adapter->unregisterAdvertisement)(LE_ADVERTISEMENT_OBJECT_PATH); })) {
        Logger::instance()->info("BLE Advertisement stopped\n");
    }
}

BluetoothHandler::ConnectResult BluetoothHandler::connectDevice() {
    DBus::ManagedObjects objects;

    // Here, unlike at startup, a failed enumeration is just a bad round: the retry loop
    // comes back in 20 seconds. NoDevices rather than Unreachable, so a bluez hiccup cannot
    // accumulate towards an adapter power cycle it had nothing to do with.
    if (!getBluezObjects(objects)) {
        return ConnectResult::NoDevices;
    }

    std::vector<std::string> device_paths;
    for (auto const& [path, interfaces]: objects) {
        for (auto const& [interface, properties]: interfaces) {
            if (interface == INTERFACE_BLUEZ_DEVICE) {
                device_paths.push_back(path);
            }
        }
    }

    if (!device_paths.size()) {
        Logger::instance()->info("Did not find any connected bluetooth device\n");
        return ConnectResult::NoDevices;
    }

    const bool isDongleMode = (Config::instance()->getConnectionStrategy() == ConnectionStrategy::DONGLE_MODE);
    bool anyConnected = false;
    bool anyClaimedConnected = false;

    Logger::instance()->info("Found %d bluetooth devices\n", device_paths.size());

    for (const std::string &device_path: device_paths) {
        Logger::instance()->info("Trying to connect bluetooth device at path: %s\n", device_path.c_str());

        // Everything below is inside the try, including creating the proxies. Creating them
        // outside it meant a throw here escaped connectDevice and killed the retry thread
        // outright, which is precisely the thread that is supposed to recover from a stall.
        try {
            std::shared_ptr<DBus::ObjectProxy> bluezDevice = m_connection->create_object_proxy(BLUEZ_BUS_NAME, device_path);
            DBus::MethodProxy connectProfile = *(bluezDevice->create_method<void(std::string)>(INTERFACE_BLUEZ_DEVICE, "ConnectProfile"));
            DBus::MethodProxy disconnect = *(bluezDevice->create_method<void()>(INTERFACE_BLUEZ_DEVICE, "Disconnect"));

            std::shared_ptr<DBus::PropertyProxy<bool>> deviceConnected = bluezDevice->create_property<bool>(INTERFACE_BLUEZ_DEVICE, "Connected");

            // Read the property, not the proxy handle. create_property() always returns a
            // valid pointer, so testing the pointer disconnected the device on every single
            // attempt, including the first one after boot when nothing can be connected yet.
            bool alreadyConnected = false;
            if (deviceConnected) {
                try {
                    alreadyConnected = deviceConnected->value();
                } catch (const std::exception&) {
                    // Treat an unreadable property as not connected and let ConnectProfile decide.
                    alreadyConnected = false;
                }
            }

            // Logged because it is the one fact that says whether a failure to connect is a
            // phone that has gone away or bluez holding a device object that is already dead.
            // No capture before now recorded it, so the escalation below is calibrated blind.
            Logger::instance()->info("Bluetooth device reports connected=%s\n", alreadyConnected ? "yes" : "no");

            if (alreadyConnected) {
                anyClaimedConnected = true;
                Logger::instance()->info("Bluetooth device already connected, disconnecting\n");
                disconnect();
            }
            connectProfile(isDongleMode ? "" : HSP_AG_UUID);
            Logger::instance()->info("Bluetooth connected to the device\n");
            anyConnected = true;
            if (!isDongleMode) {
                return ConnectResult::Connected;
            }
        } catch (DBus::Error& e) {
            if (!isDongleMode) {
                // The error text is the discriminator this project has been guessing at:
                // bluez says "Host is down" when the device object is stale and the ACL
                // underneath is gone, which is a different fault to a phone that is simply
                // absent. Drive 7 threw it away because e was declared and never read.
                Logger::instance()->info("Failed to connect device at path %s: %s\n", device_path.c_str(), e.what());
            }
        } catch (const std::exception& e) {
            // Anything that is not a DBus::Error would otherwise unwind out of the retry
            // thread. Log and carry on to the next device instead.
            if (!isDongleMode) {
                Logger::instance()->info("Failed to connect device at path %s: %s\n", device_path.c_str(), e.what());
            }
        }
    }

    if (!isDongleMode) {
        Logger::instance()->info("Failed to connect to any known bluetooth device\n");
    }

    if (anyConnected) {
        return ConnectResult::Connected;
    }

    return anyClaimedConnected ? ConnectResult::Wedged : ConnectResult::Unreachable;
}

void BluetoothHandler::resetAdapter(int attempts) {
    if (!m_adapter) {
        return;
    }

    Logger::instance()->info("Bluetooth reconnect stuck after %d attempts, power cycling the adapter\n", attempts);

    // setPower and setPairable report rather than throw now, so the outcome is checked
    // instead of caught. A reset that cannot power the adapter back on is the wedged
    // controller case, and saying so plainly is the only useful thing to do about it here.
    setPower(false);
    std::this_thread::sleep_for(ADAPTER_RESET_SETTLE_TIME);

    if (!setPower(true)) {
        Logger::instance()->info("Bluetooth adapter did not come back after the power cycle\n");
        return;
    }

    setPairable(true);
}

void BluetoothHandler::retryConnectLoop() {
    bool should_exit = false;
    int consecutiveFailures = 0;
    int staleThreshold = STALE_FAILURES_BEFORE_ADAPTER_RESET;
    int absentThreshold = ABSENT_FAILURES_BEFORE_ADAPTER_RESET;
    const bool isDongleMode = (Config::instance()->getConnectionStrategy() == ConnectionStrategy::DONGLE_MODE);
    std::future<void> connectWithRetryFuture = connectWithRetryPromise->get_future();

    while (!should_exit) {
        ConnectResult result = ConnectResult::NoDevices;

        // connectDevice talks to bluez before it reaches its own per-device try, so a throw
        // from getBluezObjects would unwind out of this loop and end the thread silently:
        // no more retries, no supervisor restart, and heartbeats still saying aawgd is up.
        try {
            result = connectDevice();
        } catch (const std::exception& e) {
            Logger::instance()->info("Bluetooth connect attempt threw: %s\n", e.what());
        } catch (...) {
            Logger::instance()->info("Bluetooth connect attempt threw an unknown exception\n");
        }

        // Waiting for a phone to appear at all is the normal state at boot and must never
        // trigger a reset, which is why NoDevices is not counted here.
        int threshold = 0;
        if (result == ConnectResult::Wedged) {
            threshold = staleThreshold;
        } else if (result == ConnectResult::Unreachable) {
            threshold = absentThreshold;
        }

        if (threshold > 0 && !isDongleMode) {
            if (++consecutiveFailures >= threshold) {
                resetAdapter(consecutiveFailures);
                consecutiveFailures = 0;

                // Back off both thresholds, so a phone that is simply not there cannot hold
                // the adapter in a power cycle every minute for the rest of the drive.
                staleThreshold = std::min(staleThreshold * 2, MAX_FAILURES_BEFORE_ADAPTER_RESET);
                absentThreshold = std::min(absentThreshold * 2, MAX_FAILURES_BEFORE_ADAPTER_RESET);
            }
        } else {
            consecutiveFailures = 0;
        }

        if (connectWithRetryFuture.wait_for(std::chrono::seconds(20)) == std::future_status::ready) {
            should_exit = true;
            connectWithRetryPromise = nullptr;
        }
    }

    if (Config::instance()->getConnectionStrategy() != ConnectionStrategy::DONGLE_MODE) {
        BluetoothHandler::instance().powerOff();
    }
}

bool BluetoothHandler::init() {
    // DBus::set_logging_function( DBus::log_std_err );
    // DBus::set_log_level( SL_TRACE );

    // Deliberately NOT guarded, unlike everything below it. If the system bus itself cannot
    // be reached there is nothing to degrade to, and the usual cause is bluetoothd not being
    // up yet, which a supervisor restart three seconds later genuinely does fix. That is the
    // opposite of the wedged-controller case, where restarting the process achieves nothing
    // and the retry has to happen in place.
    m_dispatcher = DBus::StandaloneDispatcher::create();
    m_connection = m_dispatcher->create_connection( DBus::BusType::SYSTEM );

    std::string adapterAliasPrefix = (Config::instance()->getConnectionStrategy() == ConnectionStrategy::DONGLE_MODE) ? ADAPTER_ALIAS_DONGLE_PREFIX : ADAPTER_ALIAS_PREFIX;

    m_adapterAlias = adapterAliasPrefix + Config::instance()->getUniqueSuffix();

    if (!initAdapter()) {
        return false;
    }

    return exportProfiles();
}

bool BluetoothHandler::hasAdapter() const {
    return m_adapter != nullptr;
}

bool BluetoothHandler::powerOn() {
    if (!m_adapter) {
        return false;
    }

    if (!setPower(true)) {
        return false;
    }

    // Deliberately not fatal. Discoverable and pairable only matter while a phone is being
    // bonded; an already-bonded phone connects without either. Failing powerOn here would
    // block a working session for the sake of a pairing that is not being attempted.
    //
    // The one case where it does bite is the first boot after a reflash, which wipes the
    // bond and forces a re-pair. That is a narrow window and a known one, so it is logged
    // rather than either ignored or escalated.
    if (!setPairable(true)) {
        Logger::instance()->info("Bluetooth adapter is powered but not discoverable, pairing will not work until this is fixed or the board is rebooted\n");
    }

    if (Config::instance()->getConnectionStrategy() == ConnectionStrategy::DONGLE_MODE) {
        // Also deliberately not fatal, and this one is a known limitation rather than a
        // considered trade. Retrying would re-register the same object path and almost
        // certainly fail on AlreadyExists, turning a degraded dongle mode into a retry loop
        // that can never succeed. Dongle mode is not the strategy this hardware runs, so it
        // is logged plainly and left for someone who can test that path.
        if (!startAdvertising()) {
            Logger::instance()->info("BLE advertisement failed, dongle mode will not be discoverable until restart\n");
        }
    }

    return true;
}

std::optional<std::thread> BluetoothHandler::connectWithRetry() {
    if (!m_adapter) {
        return std::nullopt;
    }

    connectWithRetryPromise = std::make_shared<std::promise<void>>();
    return std::thread(&BluetoothHandler::retryConnectLoop, this);
}

void BluetoothHandler::stopConnectWithRetry() {
    if (connectWithRetryPromise) {
        connectWithRetryPromise->set_value();
    }
}

void BluetoothHandler::powerOff() {
    if (!m_adapter) {
        return;
    }

    if (Config::instance()->getConnectionStrategy() == ConnectionStrategy::DONGLE_MODE) {
        stopAdvertising();
    }
    setPower(false);
}