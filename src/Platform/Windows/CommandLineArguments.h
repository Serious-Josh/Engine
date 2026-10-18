#pragma once

#include <string>
#include <vector>

namespace CommandLineArguments
{
    // The process command line as UTF-16 arguments, split by CommandLineToArgvW exactly as the
    // packaged runtime does. Element 0 is the executable. Never passes through the ANSI code
    // page, so non-ASCII paths survive. Throws std::runtime_error if Windows cannot split it.
    std::vector<std::wstring> Read();
}
