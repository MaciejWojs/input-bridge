#pragma once

#include "../platform_input.hpp"

#include <memory>

std::unique_ptr<IPlatformInput> CreateLinuxPlatformInputFromFactory();
