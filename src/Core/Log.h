#pragma once

#include <string_view>

namespace Log
{
    void Info(std::string_view message);
    void Warning(std::string_view message);
    void Error(std::string_view message);
}