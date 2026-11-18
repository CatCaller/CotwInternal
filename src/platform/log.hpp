#pragma once

#include <Windows.h>

namespace cotw::platform
{
    bool initializeLog(HMODULE module) noexcept;
    void shutdownLog() noexcept;
    void writeLogLine(const char* text) noexcept;
}
