#pragma once
#include <Windows.h>
#include <string>
#include <string_view>
#include <stdexcept>
namespace WindowsText
{
inline std::wstring FromUTF8(std::string_view text)
{
    if(text.empty()) return {};
    const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(!size) throw std::runtime_error("Invalid UTF-8 window title.");
    std::wstring wide(size,L'\0');
    if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),wide.data(),size)!=size)
        throw std::runtime_error("Cannot convert UTF-8 window title.");
    return wide;
}
}
