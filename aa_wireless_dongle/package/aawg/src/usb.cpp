#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <future>

#include "common.h"
#include "uevent.h"
#include "usb.h"

constexpr const char* defaultGadgetName = "default";
constexpr const char* accessoryGadgetName = "accessory";

/*static*/ std::string UsbManager::s_udcName;

UsbManager& UsbManager::instance() {
    static UsbManager instance;
    return instance;
}

void UsbManager::init() {
    // Init does not actually do anything but provides an intuitive place to cause the constructor call earlier than first use.
}

UsbManager::UsbManager() {
    Logger::instance()->info("Initializing USB Manager\n");

    disableGadget();

    DIR* dirSysClassUdc = opendir("/sys/class/udc/");
    if (dirSysClassUdc == NULL) {
        Logger::instance()->info("USB Manager: Error opening /sys/class/udc/: %s\n", strerror(errno));
        return;
    }
    
    struct dirent* dirEntry = NULL;
    while ((dirEntry = readdir(dirSysClassUdc)) != NULL) {
        if (dirEntry->d_name[0] == '.') {
            continue;
        }

        s_udcName = dirEntry->d_name;
    }

    closedir(dirSysClassUdc);

    if (s_udcName.empty()) {
        Logger::instance()->info("USB Manager: Did not find a valid UDC to use\n");
    } else {
        Logger::instance()->info("USB Manager: Found UDC %s\n", s_udcName.c_str());
    }
}

// Read a single-line attribute from the UDC's sysfs directory, trimmed.
//
// Returns "-" when the UDC name is unknown or the attribute cannot be read, so a
// missing reading is never mistaken for a real state. That matters here: these
// values are used to decide between two hardware faults, and "we did not measure
// it" has to stay distinguishable from "it read not-attached".
/*static*/ std::string UsbManager::udcAttribute(const char* attribute) {
    if (s_udcName.empty()) {
        return "-";
    }

    std::string path = "/sys/class/udc/" + s_udcName + "/" + attribute;
    FILE* file = fopen(path.c_str(), "r");
    if (file == NULL) {
        return "-";
    }

    // 32 is deliberate rather than generous. Both attributes are short fixed
    // vocabularies from the kernel ("configured", "not-attached", "high-speed",
    // "super-speed-plus"), and these values are appended to teardown lines that
    // BusyBox syslogd truncates at a fixed length. Bounding the read here keeps a
    // sysfs surprise from silently eating the end of the line it was added to.
    char buffer[32] = {0};
    char* line = fgets(buffer, sizeof(buffer), file);
    fclose(file);

    if (line == NULL) {
        return "-";
    }

    std::string value(buffer);
    // Trim the trailing newline sysfs adds, so the value composes into a log line.
    while (!value.empty() && isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }

    return value.empty() ? "-" : value;
}

// The reading that separates the two physical failure stories eight drives of
// logs cannot currently tell apart.
//
// "udcstate=not-attached" means VBUS went away, which is the power or ground
// contact breaking. Anything else, particularly holding at "configured" or
// cycling through "default"/"addressed", means VBUS stayed up and the data pair
// failed instead. "udcspeed" catches the other half: Drive 8 recorded two falls
// back to "full-speed", which is the high-speed handshake failing outright, and
// Android Auto cannot run over 12 Mbit/s.
/*static*/ std::string UsbManager::udcStatus() {
    return "udcstate=" + udcAttribute("state") + " udcspeed=" + udcAttribute("current_speed");
}

void UsbManager::writeGadgetFile(std::string gadgetName, std::string relativeFilePath, const char* content) {
    std::string gadgetFilePath = "/sys/kernel/config/usb_gadget/" + gadgetName + "/" + relativeFilePath;
    FILE* gadgetFile = fopen(gadgetFilePath.c_str(), "w");
    fputs(content, gadgetFile);
    fputc('\n', gadgetFile);
    fclose(gadgetFile);
}

void UsbManager::enableGadget(std::string gadgetName) {
    writeGadgetFile(gadgetName, "UDC", s_udcName.c_str());
}

void UsbManager::disableGadget(std::string gadgetName) {
    writeGadgetFile(gadgetName, "UDC", "");
}

void UsbManager::switchToAccessoryGadget() {
    disableGadget(defaultGadgetName);
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 0.1 second, keep the gadget disabled for a short time to let the host recognize the change
    enableGadget(accessoryGadgetName);

    Logger::instance()->info("USB Manager: Switched to accessory gadget from default\n");
}

void UsbManager::disableGadget() {
    disableGadget(defaultGadgetName);
    disableGadget(accessoryGadgetName);

    Logger::instance()->info("USB Manager: Disabled all USB gadgets\n");
}

bool UsbManager::enableDefaultAndWaitForAccessory(std::chrono::milliseconds timeout) {
    std::shared_ptr<std::promise<void>> accessoryPromise = std::make_shared<std::promise<void>>();
    std::weak_ptr<std::promise<void>> accessoryPromiseWeak = accessoryPromise;

    UeventMonitor::instance().addHandler([accessoryPromiseWeak](UeventEnv env) {
        std::shared_ptr<std::promise<void>> accessoryPromise = accessoryPromiseWeak.lock();

        // If the promise is no longer active, nothing to do.
        if (!accessoryPromise) {
            return true;
        }

        if (auto it = env.find("DEVNAME"); it == env.end() || it->second != "usb_accessory") {
            return false;
        }

        if (auto it = env.find("ACCESSORY"); it == env.end() || it->second != "START") {
            return false;
        }

        // Got an accessory start event
        Logger::instance()->info("USB Manager: Received accessory start request\n");
        UsbManager::instance().switchToAccessoryGadget();
        accessoryPromise->set_value();

        return true;
    });

    enableGadget(defaultGadgetName);

    Logger::instance()->info("USB Manager: Enabled default gadget\n");

    if (timeout == std::chrono::milliseconds(0)) {
        accessoryPromise->get_future().wait();
        return true;
    } else {
        std::future_status status = accessoryPromise->get_future().wait_for(timeout);

        if (status == std::future_status::ready) {
            return true;
        } else {
            Logger::instance()->info("USB Manager: Timeout waiting for accessory start request\n");
            return false;
        }
    }
}
