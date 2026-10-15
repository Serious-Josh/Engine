#include "Log.h"

#include <iostream>
#include <atomic>

namespace Log
{
    namespace { std::atomic<Sink> outputSink{nullptr}; }
    void SetSink(Sink sink) { outputSink.store(sink); }
    void Info(std::string_view message)
    {
        if(auto sink=outputSink.load()) { sink("Info",message); return; }
        std::cout << "[Info] " << message << "\n";
    }

    void Warning(std::string_view message)
    {
        if(auto sink=outputSink.load()) { sink("Warning",message); return; }
        std::cout << "[Warning] " << message << "\n";
    }

    void Error(std::string_view message)
    {
        if(auto sink=outputSink.load()) { sink("Error",message); return; }
        std::cerr << "[Error] " << message << "\n";
    }
}
