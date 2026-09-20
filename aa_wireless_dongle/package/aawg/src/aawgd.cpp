#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include <algorithm>

#include "common.h"
#include "bluetoothHandler.h"
#include "proxyHandler.h"
#include "uevent.h"
#include "usb.h"

// Retry pacing for an adapter that will not power on. Capped so a controller that never
// comes back logs about once a minute rather than filling the drive's log.
static constexpr unsigned int BT_POWER_ON_RETRY_MIN_SECONDS = 2;
static constexpr unsigned int BT_POWER_ON_RETRY_MAX_SECONDS = 60;

// About five minutes of paced retries before handing the problem back to the supervisor.
// Retrying in place fixes a controller that is briefly busy; it cannot fix bluez state that
// has gone stale underneath us, because this process is still holding proxies to objects
// that may no longer exist. Only a fresh process re-enumerates and re-registers.
static constexpr int BT_POWER_ON_MAX_ATTEMPTS = 10;

// Powering the adapter on used to be an unguarded D-Bus call. When the controller stopped
// answering HCI_Reset on Drive 7 the throw reached no handler and terminated the daemon,
// and aawgd-supervise respawned it every 5.3 seconds: 23 aborts in under two minutes, each
// new process running this same call against the same wedged controller.
//
// Restarting the process cannot reset a controller, so retry in place first. That keeps one
// continuous log, keeps the USB gadget and the D-Bus registrations up, and leaves the
// heartbeat running so the daemon is visibly alive rather than thrashing.
//
// Then, after roughly five minutes, exit anyway. In-place retry is the right answer for a
// controller that is briefly busy and the wrong one for bluez state that has gone stale,
// where only a fresh process re-enumerates and re-registers. The two cannot be told apart
// from here, so do the cheap one first and fall back to the thorough one.
//
// What is deliberately NOT here is rebooting the board. That is the only thing that cleared
// Drive 7, but it is a decision to take on purpose, not a side effect of an exception.
static void powerOnBluetoothWithRetry() {
    // No adapter at all is permanent, not a fault to wait out. Upstream carries on without
    // bluetooth in that case and so must this, or a board with no adapter would spin here
    // for the whole drive.
    if (!BluetoothHandler::instance().hasAdapter()) {
        return;
    }

    unsigned int backoff = BT_POWER_ON_RETRY_MIN_SECONDS;

    for (int attempt = 1; !BluetoothHandler::instance().powerOn(); attempt++) {
        if (attempt >= BT_POWER_ON_MAX_ATTEMPTS) {
            Logger::instance()->info("Bluetooth adapter would not power on after %d attempts, exiting so bluez state is rebuilt\n", attempt);

            // exit() rather than return, for two reasons. Returning would unwind past a
            // joinable proxy thread, and ~std::thread on a joinable thread calls terminate,
            // which is the abort this whole change exists to remove. And the proxy thread
            // may already have exited while we were sleeping here, in which case nothing
            // downstream would ever join it and main would wait forever on a dead session.
            exit(1);
        }

        Logger::instance()->info("Bluetooth adapter would not power on (attempt %d), retrying in %us\n", attempt, backoff);
        sleep(backoff);
        backoff = std::min(backoff * 2, BT_POWER_ON_RETRY_MAX_SECONDS);
    }
}

int main(void) {
    Logger::instance()->info("AA Wireless Dongle\n");

    // Writing to a socket closed by the peer raises SIGPIPE, which kills the
    // process by default. Ignore it so the write fails with EPIPE and the
    // normal error handling and reconnection logic can run instead.
    signal(SIGPIPE, SIG_IGN);

    // Global init
    std::optional<std::thread> ueventThread =  UeventMonitor::instance().start();
    UsbManager::instance().init();

    // Failing to reach bluez at all is recoverable by starting over, and the supervisor is
    // what does that, so exit and let it. This is not the same as finding no adapter, which
    // init() reports as success because no amount of restarting will produce one.
    if (!BluetoothHandler::instance().init()) {
        Logger::instance()->info("Bluetooth initialisation failed, exiting for a clean restart\n");
        exit(1);
    }

    ConnectionStrategy connectionStrategy = Config::instance()->getConnectionStrategy();
    if (connectionStrategy == ConnectionStrategy::DONGLE_MODE) {
        powerOnBluetoothWithRetry();
    }

    while (true) {
        Logger::instance()->info("Connection Strategy: %d\n", connectionStrategy);

        // Per connection setup and processing
        if (connectionStrategy == ConnectionStrategy::USB_FIRST) {
            Logger::instance()->info("Waiting for the accessory to connect first\n");
            UsbManager::instance().enableDefaultAndWaitForAccessory();
        }

        AAWProxy proxy;
        std::optional<std::thread> proxyThread = proxy.startServer(Config::instance()->getWifiInfo().port);

        if (!proxyThread) {
            return 1;
        }

        if (connectionStrategy != ConnectionStrategy::DONGLE_MODE) {
            // This is the exact line Drive 7 died on, 23 times in one boot.
            powerOnBluetoothWithRetry();
        }

        std::optional<std::thread> btConnectionThread = BluetoothHandler::instance().connectWithRetry();

        proxyThread->join();

        if (btConnectionThread) {
            BluetoothHandler::instance().stopConnectWithRetry();
            btConnectionThread->join();
        }

        UsbManager::instance().disableGadget();

        if (proxy.endedOnUsbError()) {
            // Drive 10 boots 46 and 50 logged dwc2 FIFO flush HANG! messages
            // followed by a reset storm or an accessory that never configured.
            Logger::instance()->info("USB link failed, settling the controller for 3s before re-presenting the gadget\n");
            sleep(3);
        }

        if (connectionStrategy != ConnectionStrategy::DONGLE_MODE) {
            // sleep for a couple of seconds before retrying
            sleep(2);
        }
    }

    ueventThread->join();

    return 0;
}
