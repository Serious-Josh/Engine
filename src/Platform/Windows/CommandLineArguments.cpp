#include "Platform/Windows/CommandLineArguments.h"

#include <Windows.h>
#include <shellapi.h>

#include <stdexcept>

namespace CommandLineArguments
{
    std::vector<std::wstring> Read()
    {
        int count = 0;
        wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);

        if(!arguments)
        {
            throw std::runtime_error("Cannot read the process command line.");
        }

        std::vector<std::wstring> result;
        result.reserve(static_cast<std::size_t>(count));

        for(int index = 0; index < count; ++index)
        {
            result.emplace_back(arguments[index]);
        }

        // CommandLineToArgvW allocates the array with LocalAlloc.
        LocalFree(arguments);
        return result;
    }
}
