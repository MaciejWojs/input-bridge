#include "linux_uinput_injector.hpp"

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

LinuxUinputInjector::~LinuxUinputInjector() {
    Shutdown();
}

bool LinuxUinputInjector::Initialize(std::string& error_msg) {
    Shutdown();

    if (access("/dev/uinput", F_OK) != 0) {
        error_msg = "uinput device node /dev/uinput is not present.";
        return false;
    }

    m_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (m_fd < 0) {
        error_msg = std::string("Cannot open /dev/uinput: ") + std::strerror(errno);
        return false;
    }

    ioctl(m_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(m_fd, UI_SET_EVBIT, EV_REL);
    ioctl(m_fd, UI_SET_EVBIT, EV_ABS);
    ioctl(m_fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(m_fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(m_fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(m_fd, UI_SET_RELBIT, REL_X);
    ioctl(m_fd, UI_SET_RELBIT, REL_Y);
    ioctl(m_fd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(m_fd, UI_SET_ABSBIT, ABS_X);
    ioctl(m_fd, UI_SET_ABSBIT, ABS_Y);

    for (int key = KEY_ESC; key <= KEY_MICMUTE; ++key) {
        ioctl(m_fd, UI_SET_KEYBIT, key);
    }

    uinput_setup usetup{};
    std::snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "input-bridge-uinput");
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x1;
    usetup.id.product = 0x1;
    usetup.id.version = 1;

    if (ioctl(m_fd, UI_DEV_SETUP, &usetup) < 0) {
        error_msg = std::string("UI_DEV_SETUP failed: ") + std::strerror(errno);
        Shutdown();
        return false;
    }

    uinput_abs_setup abs_setup{};
    abs_setup.code = ABS_X;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = 32767;
    ioctl(m_fd, UI_ABS_SETUP, &abs_setup);
    abs_setup.code = ABS_Y;
    ioctl(m_fd, UI_ABS_SETUP, &abs_setup);

    if (ioctl(m_fd, UI_DEV_CREATE) < 0) {
        error_msg = std::string("UI_DEV_CREATE failed: ") + std::strerror(errno);
        Shutdown();
        return false;
    }

    return true;
}

void LinuxUinputInjector::Shutdown() {
    if (m_fd >= 0) {
        ioctl(m_fd, UI_DEV_DESTROY);
        close(m_fd);
        m_fd = -1;
    }
}

bool LinuxUinputInjector::IsReady() const {
    return m_fd >= 0;
}

bool LinuxUinputInjector::EmitEvent(uint16_t type, uint16_t code, int32_t value) {
    if (m_fd < 0) {
        return false;
    }

    input_event ev{};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return write(m_fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev));
}

bool LinuxUinputInjector::EmitSync() {
    return EmitEvent(EV_SYN, SYN_REPORT, 0);
}

bool LinuxUinputInjector::MoveRelative(int32_t x, int32_t y) {
    return EmitEvent(EV_REL, REL_X, x) && EmitEvent(EV_REL, REL_Y, y) && EmitSync();
}

bool LinuxUinputInjector::MoveAbsolute(int32_t x, int32_t y, int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) {
        return false;
    }

    const int32_t ax = (x * 32767) / width;
    const int32_t ay = (y * 32767) / height;
    return EmitEvent(EV_ABS, ABS_X, ax) && EmitEvent(EV_ABS, ABS_Y, ay) && EmitSync();
}

bool LinuxUinputInjector::MouseClick(int32_t button, bool down) {
    uint16_t code = BTN_LEFT;
    if (button == 1) code = BTN_RIGHT;
    if (button == 2) code = BTN_MIDDLE;
    return EmitEvent(EV_KEY, code, down ? 1 : 0) && EmitSync();
}

bool LinuxUinputInjector::KeyPress(uint32_t evdev_code, bool down) {
    return EmitEvent(EV_KEY, static_cast<uint16_t>(evdev_code), down ? 1 : 0) && EmitSync();
}

bool LinuxUinputInjector::Scroll(int32_t delta) {
    return EmitEvent(EV_REL, REL_WHEEL, delta) && EmitSync();
}
