#include "platform/text.hpp"

#include <intrin.h>

#include "platform/memory.hpp"

namespace cotw::platform
{
    TextBuilder::TextBuilder() noexcept
        : buffer_{}, length_(0)
    {
        buffer_[0] = '\0';
    }

    void TextBuilder::clear() noexcept
    {
        length_ = 0;
        buffer_[0] = '\0';
    }

    TextBuilder& TextBuilder::append(const char* text) noexcept
    {
        if (text == nullptr)
        {
            return *this;
        }

        while (*text != '\0' && length_ + 1 < kCapacity)
        {
            buffer_[length_] = *text;
            ++length_;
            ++text;
        }

        buffer_[length_] = '\0';
        return *this;
    }

    TextBuilder& TextBuilder::appendChar(const char value) noexcept
    {
        if (length_ + 1 < kCapacity)
        {
            buffer_[length_] = value;
            ++length_;
            buffer_[length_] = '\0';
        }

        return *this;
    }

    TextBuilder& TextBuilder::appendUnsigned(std::uint64_t value, const std::uint32_t base) noexcept
    {
        if (base < 2 || base > 16)
        {
            return *this;
        }

        char digits[65]{};
        std::size_t count = 0;
        if (value == 0)
        {
            digits[count++] = '0';
        }
        else
        {
            while (value != 0 && count < 64)
            {
                const std::uint32_t digit = static_cast<std::uint32_t>(value % base);
                digits[count++] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + (digit - 10));
                value /= base;
            }
        }

        while (count > 0)
        {
            --count;
            appendChar(digits[count]);
        }

        return *this;
    }

    TextBuilder& TextBuilder::appendSigned(const std::int64_t value) noexcept
    {
        if (value < 0)
        {
            appendChar('-');
            const std::uint64_t magnitude = static_cast<std::uint64_t>(-(value + 1)) + 1;
            return appendUnsigned(magnitude);
        }

        return appendUnsigned(static_cast<std::uint64_t>(value));
    }

    TextBuilder& TextBuilder::appendHex(const std::uint64_t value) noexcept
    {
        append("0x");
        return appendUnsigned(value, 16);
    }

    TextBuilder& TextBuilder::appendFloat(const float value, const std::uint32_t decimals) noexcept
    {
        if (value != value || value > 1.0e30f || value < -1.0e30f)
        {
            return append("nan");
        }

        const float absolute = value < 0.0f ? -value : value;
        const int whole = _mm_cvtt_ss2si(_mm_set_ss(absolute));
        if (value < 0.0f)
        {
            appendChar('-');
        }

        appendUnsigned(static_cast<std::uint64_t>(whole));
        if (decimals == 0)
        {
            return *this;
        }

        appendChar('.');

        float fraction = absolute - static_cast<float>(whole);
        for (std::uint32_t index = 0; index < decimals; ++index)
        {
            fraction *= 10.0f;
            const int digit = _mm_cvtt_ss2si(_mm_set_ss(fraction));
            appendChar(static_cast<char>('0' + digit));
            fraction -= static_cast<float>(digit);
        }

        return *this;
    }

    const char* TextBuilder::c_str() const noexcept
    {
        return buffer_;
    }

    std::size_t TextBuilder::length() const noexcept
    {
        return length_;
    }
}
