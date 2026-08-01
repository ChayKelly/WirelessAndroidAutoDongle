#include <string>
#include <chrono>

class UsbManager {
public:
    static UsbManager& instance();

    void init();
    bool enableDefaultAndWaitForAccessory(std::chrono::milliseconds timeout = std::chrono::milliseconds(0));
    void switchToAccessoryGadget();
    void disableGadget();

    // One-line snapshot of the USB gadget link, for logging next to a teardown.
    // Static because it reads sysfs only and callers need it from the forwarding
    // threads at the instant a transfer fails.
    static std::string udcStatus();

private:
    UsbManager();
    UsbManager(UsbManager const&);
    UsbManager& operator=(UsbManager const&);

    void writeGadgetFile(std::string gadgetName, std::string relativeFilePath, const char* content);
    void enableGadget(std::string name);
    void disableGadget(std::string name);

    static std::string udcAttribute(const char* attribute);

    static std::string s_udcName;
};