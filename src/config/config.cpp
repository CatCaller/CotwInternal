#include "config/config.hpp"

#include "platform/memory.hpp"
#include "platform/text.hpp"

namespace cotw::config
{
    namespace
    {
        constexpr char kDefaultConfig[] =
            "[General]\n"
            "EnableNoSenses=1\n"
            "EnableNoSmell=1\n"
            "EnableSpeed=1\n"
            "SpeedMultiplier=2.0\n"
            "HighlightDistance=600\n"
            "\n"
            "[Keys]\n"
            "ToggleSenses=113\n"
            "ToggleSpeed=114\n";

        void buildIniPath(char (&path)[MAX_PATH], const HMODULE module) noexcept
        {
            const DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
            if (length == 0 || length >= MAX_PATH)
            {
                platform::copyText(path, MAX_PATH, "CotwInternal.ini");
                return;
            }

            for (DWORD index = length; index > 0; --index)
            {
                if (path[index - 1] == '.')
                {
                    path[index - 1] = '\0';
                    break;
                }
            }

            char withExtension[MAX_PATH]{};
            platform::copyText(withExtension, MAX_PATH, path);
            const std::size_t baseLength = platform::textLength(withExtension);
            if (baseLength + 5 < MAX_PATH)
            {
                withExtension[baseLength] = '.';
                withExtension[baseLength + 1] = 'i';
                withExtension[baseLength + 2] = 'n';
                withExtension[baseLength + 3] = 'i';
                withExtension[baseLength + 4] = '\0';
            }

            platform::copyText(path, MAX_PATH, withExtension);
        }

        void writeDefaultConfig(const char* path) noexcept
        {
            if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
            {
                return;
            }

            const HANDLE file = CreateFileA(
                path,
                GENERIC_WRITE,
                0,
                nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);

            if (file == INVALID_HANDLE_VALUE)
            {
                return;
            }

            DWORD written = 0;
            WriteFile(file, kDefaultConfig, static_cast<DWORD>(platform::textLength(kDefaultConfig)), &written, nullptr);
            CloseHandle(file);
        }

        int readInteger(const char* section, const char* key, const int fallback, const char* path) noexcept
        {
            return GetPrivateProfileIntA(section, key, fallback, path);
        }

        float parseFloat(const char* text, const float fallback) noexcept
        {
            if (text == nullptr)
            {
                return fallback;
            }

            std::size_t index = 0;
            while (text[index] == ' ' || text[index] == '\t')
            {
                ++index;
            }

            bool negative = false;
            if (text[index] == '-')
            {
                negative = true;
                ++index;
            }
            else if (text[index] == '+')
            {
                ++index;
            }

            bool hasDigits = false;
            float value = 0.0f;
            while (text[index] >= '0' && text[index] <= '9')
            {
                value = value * 10.0f + static_cast<float>(text[index] - '0');
                hasDigits = true;
                ++index;
            }

            if (text[index] == '.')
            {
                ++index;
                float scale = 0.1f;
                while (text[index] >= '0' && text[index] <= '9')
                {
                    value += static_cast<float>(text[index] - '0') * scale;
                    scale *= 0.1f;
                    hasDigits = true;
                    ++index;
                }
            }

            if (!hasDigits)
            {
                return fallback;
            }

            return negative ? -value : value;
        }

        float readFloat(const char* section, const char* key, const float fallback, const char* path) noexcept
        {
            char buffer[64]{};
            GetPrivateProfileStringA(section, key, "", buffer, sizeof(buffer), path);
            return parseFloat(buffer, fallback);
        }

        bool readBoolean(const char* section, const char* key, const bool fallback, const char* path) noexcept
        {
            const int value = readInteger(section, key, fallback ? 1 : 0, path);
            return value != 0;
        }
    }

    void loadConfig(Config& config, const HMODULE module) noexcept
    {
        char path[MAX_PATH]{};
        buildIniPath(path, module);
        writeDefaultConfig(path);

        config.noSensesEnabled = readBoolean("General", "EnableNoSenses", true, path);
        config.noSmellEnabled = readBoolean("General", "EnableNoSmell", true, path);
        config.speedEnabled = readBoolean("General", "EnableSpeed", true, path);
        config.speedMultiplier = readFloat("General", "SpeedMultiplier", 2.0f, path);
        config.highlightDistance = readFloat("General", "HighlightDistance", 600.0f, path);

        if (config.speedMultiplier < 0.1f || config.speedMultiplier > 20.0f)
        {
            config.speedMultiplier = 2.0f;
        }

        if (config.highlightDistance < 20.0f || config.highlightDistance > 2000.0f)
        {
            config.highlightDistance = 600.0f;
        }

        config.keyToggleSenses = readInteger("Keys", "ToggleSenses", 0x71, path);
        config.keyToggleSpeed = readInteger("Keys", "ToggleSpeed", 0x72, path);
    }
}
