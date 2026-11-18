#pragma once

#include <cstddef>

namespace cotw::platform
{
    bool installCallHook(void* site, void* detour, void* expectedTarget) noexcept;
    bool installVtableHook(void** vtable, std::size_t index, void* detour, void** original) noexcept;
}
