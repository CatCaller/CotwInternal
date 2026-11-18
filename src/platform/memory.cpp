#include "platform/memory.hpp"

#include <Windows.h>
#include <intrin.h>

extern "C" int _fltused = 0;

namespace cotw::platform
{
    namespace
    {
        bool hasReadableProtection(const DWORD protection) noexcept
        {
            if ((protection & PAGE_NOACCESS) != 0 || (protection & PAGE_GUARD) != 0)
            {
                return false;
            }

            const DWORD baseProtection = protection & 0xFF;
            return baseProtection == PAGE_READONLY
                || baseProtection == PAGE_READWRITE
                || baseProtection == PAGE_WRITECOPY
                || baseProtection == PAGE_EXECUTE_READ
                || baseProtection == PAGE_EXECUTE_READWRITE
                || baseProtection == PAGE_EXECUTE_WRITECOPY;
        }

        bool hasWritableProtection(const DWORD protection) noexcept
        {
            if ((protection & PAGE_NOACCESS) != 0 || (protection & PAGE_GUARD) != 0)
            {
                return false;
            }

            const DWORD baseProtection = protection & 0xFF;
            return baseProtection == PAGE_READWRITE
                || baseProtection == PAGE_WRITECOPY
                || baseProtection == PAGE_EXECUTE_READWRITE
                || baseProtection == PAGE_EXECUTE_WRITECOPY;
        }

        bool queryProtectionRange(
            const void* address,
            const std::size_t size,
            const bool requireWritable) noexcept
        {
            if (address == nullptr || size == 0)
            {
                return false;
            }

            const auto start = reinterpret_cast<std::uintptr_t>(address);
            const auto end = start + size;
            if (end < start)
            {
                return false;
            }

            auto cursor = start;
            while (cursor < end)
            {
                MEMORY_BASIC_INFORMATION information{};
                if (VirtualQuery(reinterpret_cast<const void*>(cursor), &information, sizeof(information)) == 0)
                {
                    return false;
                }

                if (information.State != MEM_COMMIT)
                {
                    return false;
                }

                const bool allowed = requireWritable
                    ? hasWritableProtection(information.Protect)
                    : hasReadableProtection(information.Protect);
                if (!allowed)
                {
                    return false;
                }

                const auto regionEnd = reinterpret_cast<std::uintptr_t>(information.BaseAddress) + information.RegionSize;
                if (regionEnd <= cursor)
                {
                    return false;
                }

                if (end <= regionEnd)
                {
                    return true;
                }

                cursor = regionEnd;
            }

            return true;
        }
    }

    bool isReadable(const void* address, const std::size_t size) noexcept
    {
        return queryProtectionRange(address, size, false);
    }

    bool isWritable(const void* address, const std::size_t size) noexcept
    {
        return queryProtectionRange(address, size, true);
    }

    bool readBytes(const void* address, void* destination, const std::size_t size) noexcept
    {
        if (destination == nullptr || !isReadable(address, size))
        {
            return false;
        }

        copyBytes(destination, address, size);
        return true;
    }

    bool writeBytes(void* address, const void* source, const std::size_t size) noexcept
    {
        if (source == nullptr || !isWritable(address, size))
        {
            return false;
        }

        copyBytes(address, source, size);
        return true;
    }

    void copyBytes(void* destination, const void* source, const std::size_t size) noexcept
    {
        if (destination == nullptr || source == nullptr || size == 0 || destination == source)
        {
            return;
        }

        auto* target = static_cast<unsigned char*>(destination);
        const auto* origin = static_cast<const unsigned char*>(source);
        const auto targetAddress = reinterpret_cast<std::uintptr_t>(destination);
        const auto originAddress = reinterpret_cast<std::uintptr_t>(source);
        if (targetAddress < originAddress || targetAddress >= originAddress + size)
        {
            __movsb(target, origin, size);
            return;
        }

        volatile unsigned char* volatileTarget = target;
        const volatile unsigned char* volatileOrigin = origin;
        for (std::size_t index = size; index > 0; --index)
        {
            volatileTarget[index - 1] = volatileOrigin[index - 1];
        }
    }

    void zeroBytes(void* destination, const std::size_t size) noexcept
    {
        if (destination == nullptr || size == 0)
        {
            return;
        }

        __stosb(static_cast<unsigned char*>(destination), 0, size);
    }

    void* allocate(const std::size_t size) noexcept
    {
        if (size == 0)
        {
            return nullptr;
        }

        return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }

    void deallocate(void* memory) noexcept
    {
        if (memory != nullptr)
        {
            VirtualFree(memory, 0, MEM_RELEASE);
        }
    }

    std::size_t textLength(const char* text) noexcept
    {
        if (text == nullptr)
        {
            return 0;
        }

        std::size_t length = 0;
        while (text[length] != '\0')
        {
            ++length;
        }

        return length;
    }

    bool textEquals(const char* left, const char* right) noexcept
    {
        if (left == nullptr || right == nullptr)
        {
            return left == right;
        }

        std::size_t index = 0;
        while (left[index] != '\0' && right[index] != '\0')
        {
            if (left[index] != right[index])
            {
                return false;
            }

            ++index;
        }

        return left[index] == right[index];
    }

    void copyText(char* destination, const std::size_t capacity, const char* source) noexcept
    {
        if (destination == nullptr || capacity == 0)
        {
            return;
        }

        if (source == nullptr)
        {
            destination[0] = '\0';
            return;
        }

        std::size_t index = 0;
        while (index + 1 < capacity && source[index] != '\0')
        {
            destination[index] = source[index];
            ++index;
        }

        destination[index] = '\0';
    }
}

#pragma function(memcpy, memmove, memset)

extern "C" void* __cdecl memcpy(void* destination, const void* source, const std::size_t size)
{
    cotw::platform::copyBytes(destination, source, size);
    return destination;
}

extern "C" void* __cdecl memmove(void* destination, const void* source, const std::size_t size)
{
    cotw::platform::copyBytes(destination, source, size);
    return destination;
}

extern "C" void* __cdecl memset(void* destination, const int value, const std::size_t size)
{
    if (destination == nullptr || size == 0)
    {
        return destination;
    }

    __stosb(
        static_cast<unsigned char*>(destination),
        static_cast<unsigned char>(value),
        size);
    return destination;
}
