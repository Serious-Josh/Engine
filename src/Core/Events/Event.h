#pragma once

enum class EventType
{
    None,
    WindowClose,
    WindowResized,
    KeyPressed,
    KeyReleased,
    MouseButtonPressed,
    MouseButtonReleased,
    MouseMoved,
    MouseScrolled,
    // The window lost keyboard focus; key-up messages for held keys will not arrive.
    FocusLost,
    FocusGained,
    // Raw relative mouse motion (mouseX/mouseY hold the delta): no acceleration, no screen edges.
    MouseRawMoved
};

struct Event
{
    EventType type = EventType::None;
    unsigned int code = 0;

    int mouseX = 0;
    int mouseY = 0;

    float scrollX = 0.0f;
    float scrollY = 0.0f;

    unsigned int width = 0;
    unsigned int height = 0;
};
