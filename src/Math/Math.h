#pragma once

constexpr float Pi = 3.14159265358979323846f;

constexpr float ToRadians(float degrees)
{
    return degrees * (Pi / 180.0f);
}

constexpr float ToDegrees(float radians)
{
    return radians * (180.0f / Pi);
}
