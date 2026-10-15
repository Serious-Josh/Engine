#pragma once

#include <string_view>

namespace Log
{
    // Optional process-wide sink, configured before worker threads start.
    using Sink = void (*)(std::string_view level, std::string_view message);
    void SetSink(Sink sink);

    void Info(std::string_view message);
    void Warning(std::string_view message);
    void Error(std::string_view message);
}
