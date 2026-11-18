#include "platform/log.hpp"

#include "platform/memory.hpp"
#include "platform/text.hpp"

namespace cotw::platform
{
    namespace
    {
        HANDLE logFile = INVALID_HANDLE_VALUE;
        CRITICAL_SECTION logLock{};
        bool logLockReady = false;

        void appendTimestamp(TextBuilder& builder) noexcept
        {
            SYSTEMTIME time{};
            GetLocalTime(&time);

            builder.appendChar('[');
            if (time.wHour < 10)
            {
                builder.appendChar('0');
            }
            builder.appendUnsigned(time.wHour);
            builder.appendChar(':');
            if (time.wMinute < 10)
            {
                builder.appendChar('0');
            }
            builder.appendUnsigned(time.wMinute);
            builder.appendChar(':');
            if (time.wSecond < 10)
            {
                builder.appendChar('0');
            }
            builder.appendUnsigned(time.wSecond);
            builder.appendChar('.');
            if (time.wMilliseconds < 100)
            {
                builder.appendChar('0');
            }
            if (time.wMilliseconds < 10)
            {
                builder.appendChar('0');
            }
            builder.appendUnsigned(time.wMilliseconds);
            builder.append("] ");
        }

        void writeRaw(const char* text, const std::size_t length) noexcept
        {
            if (logFile == INVALID_HANDLE_VALUE || text == nullptr || length == 0)
            {
                return;
            }

            DWORD written = 0;
            WriteFile(logFile, text, static_cast<DWORD>(length), &written, nullptr);
        }
    }

    bool initializeLog(const HMODULE module) noexcept
    {
        InitializeCriticalSection(&logLock);
        logLockReady = true;

        char path[MAX_PATH]{};
        DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
        {
            copyText(path, MAX_PATH, "CotwInternal.log");
        }
        else
        {
            for (DWORD index = length; index > 0; --index)
            {
                if (path[index - 1] == '.')
                {
                    path[index - 1] = '\0';
                    break;
                }
            }

            char withExtension[MAX_PATH]{};
            copyText(withExtension, MAX_PATH, path);
            const std::size_t baseLength = textLength(withExtension);
            if (baseLength + 5 < MAX_PATH)
            {
                withExtension[baseLength] = '.';
                withExtension[baseLength + 1] = 'l';
                withExtension[baseLength + 2] = 'o';
                withExtension[baseLength + 3] = 'g';
                withExtension[baseLength + 4] = '\0';
            }

            copyText(path, MAX_PATH, withExtension);
        }

        logFile = CreateFileA(
            path,
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (logFile == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        writeLogLine("CotwInternal loaded");
        return true;
    }

    void shutdownLog() noexcept
    {
        if (logFile != INVALID_HANDLE_VALUE)
        {
            writeLogLine("CotwInternal unloaded");
            CloseHandle(logFile);
            logFile = INVALID_HANDLE_VALUE;
        }

        if (logLockReady)
        {
            DeleteCriticalSection(&logLock);
            logLockReady = false;
        }
    }

    void writeLogLine(const char* text) noexcept
    {
        if (logFile == INVALID_HANDLE_VALUE)
        {
            return;
        }

        if (logLockReady)
        {
            EnterCriticalSection(&logLock);
        }

        TextBuilder builder;
        appendTimestamp(builder);
        builder.append(text != nullptr ? text : "");
        builder.appendChar('\n');
        writeRaw(builder.c_str(), builder.length());

        if (logLockReady)
        {
            LeaveCriticalSection(&logLock);
        }
    }
}
