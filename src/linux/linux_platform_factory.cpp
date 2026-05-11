#include "linux_platform_factory.hpp"
#include "platform_input_linux.hpp"

std::unique_ptr<IPlatformInput> CreateLinuxPlatformInputFromFactory() {
    return CreateLinuxPlatformInput();
}
