#pragma once

#include <iostream>

#define ENABLE_LOGGING

#ifdef ENABLE_LOGGING
#define LOG_INFO(msg) std::cout << "[InputBridge INFO] " << msg << std::endl
#else
#define LOG_INFO(msg)
#endif

#define LOG_ERROR(msg) std::cerr << "[InputBridge ERROR] " << msg << std::endl
