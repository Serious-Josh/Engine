#pragma once

enum class EventType{
    None,
    WindowClose,
    KeyPressed,
    KeyReleased,
    MouseButtonPressed,
    MouseButtonReleased,
    MouseMoved
};

struct Event{
    EventType type = EventType::None;
    unsigned int keyCode = 0;
};