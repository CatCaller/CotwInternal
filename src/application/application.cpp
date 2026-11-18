#include "application/application.hpp"

#include "application/feature_state.hpp"
#include "config/config.hpp"
#include "game/game_memory.hpp"
#include "game/organ_highlight.hpp"
#include "game/senses.hpp"
#include "game/speed.hpp"
#include "game/animal_highlight.hpp"
#include "platform/log.hpp"
#include "platform/memory.hpp"
#include "platform/text.hpp"

namespace cotw::application
{
    namespace
    {
        class KeyState
        {
        public:
            KeyState() noexcept
                : previous_{}, current_{}
            {
            }

            void updateKey(const int key) noexcept
            {
                if (key <= 0 || key >= 256)
                {
                    return;
                }

                previous_[key] = current_[key];
                const SHORT state = GetAsyncKeyState(key);
                current_[key] = (state & 0x8000) != 0;
            }

            bool pressed(const int key) const noexcept
            {
                if (key <= 0 || key >= 256)
                {
                    return false;
                }

                return current_[key] && !previous_[key];
            }

        private:
            bool previous_[256];
            bool current_[256];
        };

        void logBoolean(const char* label, const bool value) noexcept
        {
            platform::TextBuilder builder;
            builder.append(label);
            builder.append(value ? ": on" : ": off");
            platform::writeLogLine(builder.c_str());
        }
    }

    DWORD WINAPI run(void* module) noexcept
    {
        const HMODULE instance = static_cast<HMODULE>(module);
        Sleep(50);
        platform::initializeLog(instance);
        platform::writeLogLine("CotwInternal 1.0 starting");

        config::Config configuration{};
        config::loadConfig(configuration, instance);

        features.noSensesEnabled = configuration.noSensesEnabled;
        features.noSmellEnabled = configuration.noSmellEnabled;
        features.speedEnabled = configuration.speedEnabled;
        features.speedMultiplier = configuration.speedMultiplier;
        features.highlightDistance = configuration.highlightDistance;

        platform::writeLogLine("Orange animal outlines are always active");
        logBoolean("No senses", features.noSensesEnabled);
        logBoolean("No smell", features.noSmellEnabled);
        logBoolean("Speed", features.speedEnabled);

        platform::TextBuilder configurationLine;
        configurationLine.append("Speed multiplier ");
        configurationLine.appendFloat(features.speedMultiplier, 2);
        configurationLine.append(", highlight distance ");
        configurationLine.appendFloat(features.highlightDistance, 0);
        platform::writeLogLine(configurationLine.c_str());

        if (!game::initialize())
        {
            platform::writeLogLine("theHunterCotW_F.exe was not found; nothing was hooked");
            platform::shutdownLog();
            FreeLibraryAndExitThread(instance, 0);
        }

        platform::TextBuilder baseLine;
        baseLine.append("Game module base ");
        baseLine.appendHex(game::base());
        platform::writeLogLine(baseLine.c_str());

        game::initializeSpeed(configuration);
        game::installSenseSuppression(configuration);
        if (!game::installAnimalOutlines())
        {
            platform::writeLogLine("Animal outline installation incomplete");
        }
        if (!game::installOrganHighlight())
        {
            platform::writeLogLine("Organ highlight installation incomplete");
        }

        KeyState keys;
        unsigned long long lastSenseUpdate = 0;
        for (;;)
        {
            keys.updateKey(configuration.keyToggleSenses);
            keys.updateKey(configuration.keyToggleSpeed);

            const unsigned long long now = GetTickCount64();
            if (now - lastSenseUpdate >= 5000)
            {
                lastSenseUpdate = now;
                game::updateSenseSuppression();
            }

            if (keys.pressed(configuration.keyToggleSenses))
            {
                const bool enabled = !features.noSensesEnabled;
                features.noSensesEnabled = enabled;
                features.noSmellEnabled = enabled;
                logBoolean("No senses", enabled);
                logBoolean("No smell", enabled);
            }

            if (keys.pressed(configuration.keyToggleSpeed))
            {
                features.speedEnabled = !features.speedEnabled;
                logBoolean("Speed", features.speedEnabled);
            }

            game::updateSpeed();
            Sleep(1);
        }
    }
}
