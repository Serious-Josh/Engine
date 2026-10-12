#include "Log.h"

#include <iostream>

namespace Log
{
    void Info(std::string_view message)
    {
        std::cout << "[Info] " << message << "\n";
    }

    void Warning(std::string_view message)
    {
        std::cout << "[Warning] " << message << "\n";
    }

    void Error(std::string_view message)
    {
        std::cerr << "[Error] " << message << "\n";
    }
}
