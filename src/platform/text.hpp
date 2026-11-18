#pragma once

#include <cstddef>
#include <cstdint>

namespace cotw::platform
{
    class TextBuilder
    {
    public:
        TextBuilder() noexcept;

        void clear() noexcept;
        TextBuilder& append(const char* text) noexcept;
        TextBuilder& appendChar(char value) noexcept;
        TextBuilder& appendUnsigned(std::uint64_t value, std::uint32_t base = 10) noexcept;
        TextBuilder& appendSigned(std::int64_t value) noexcept;
        TextBuilder& appendHex(std::uint64_t value) noexcept;
        TextBuilder& appendFloat(float value, std::uint32_t decimals = 2) noexcept;

        const char* c_str() const noexcept;
        std::size_t length() const noexcept;

    private:
        static constexpr std::size_t kCapacity = 768;

        char buffer_[kCapacity];
        std::size_t length_;
    };
}
