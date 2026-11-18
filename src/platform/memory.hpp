#pragma once

#include <cstddef>
#include <cstdint>

namespace cotw::platform
{
    bool isReadable(const void* address, std::size_t size) noexcept;
    bool isWritable(const void* address, std::size_t size) noexcept;
    bool readBytes(const void* address, void* destination, std::size_t size) noexcept;
    bool writeBytes(void* address, const void* source, std::size_t size) noexcept;

    void copyBytes(void* destination, const void* source, std::size_t size) noexcept;
    void zeroBytes(void* destination, std::size_t size) noexcept;

    void* allocate(std::size_t size) noexcept;
    void deallocate(void* memory) noexcept;

    std::size_t textLength(const char* text) noexcept;
    bool textEquals(const char* left, const char* right) noexcept;
    void copyText(char* destination, std::size_t capacity, const char* source) noexcept;
}
