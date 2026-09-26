#include "Input.h"

void Input::Update()
{
    m_scrollX = 0.0f;
    m_scrollY = 0.0f;

    for(unsigned int i = 0; i < 256; ++i){
        m_previousKeys[i] = m_keys[i];
    }

    for(unsigned int i = 0; i < 3; ++i){
        m_previousMouseButtons[i] = m_mouseButtons[i];
    }

    m_previousMouseX = m_mouseX;
    m_previousMouseY = m_mouseY;
}

// --------------------
// Key Input Handling
// --------------------

void Input::SetKeyState(unsigned int keyCode, bool pressed){
    if(keyCode < 256){
        m_keys[keyCode] = pressed;
    }
}

bool Input::IsKeyDown(unsigned int keyCode) const{
    if(keyCode < 256){
        return m_keys[keyCode];
    }

    return false;
}

bool Input::WasKeyPressed(unsigned int keyCode) const{
    if(keyCode < 256){
        return m_keys[keyCode] && !m_previousKeys[keyCode];
    }

    return false;
}

bool Input::WasKeyReleased(unsigned int keyCode) const{
    if(keyCode < 256){
        return !m_keys[keyCode] && m_previousKeys[keyCode];
    }

    return false;
}



// --------------------
// Mouse Input Handling
// --------------------

void Input::SetMouseButtonState(unsigned int button, bool pressed)
{
    if(button < 3){
        m_mouseButtons[button] = pressed;
    }
}

bool Input::IsMouseButtonDown(unsigned int button) const
{
    if(button < 3){
        return m_mouseButtons[button];
    }

    return false;
}

bool Input::WasMouseButtonPressed(unsigned int button) const
{
    if(button < 3){
        return m_mouseButtons[button] && !m_previousMouseButtons[button];
    }

    return false;
}

bool Input::WasMouseButtonReleased(unsigned int button) const
{
    if(button < 3){
        return !m_mouseButtons[button] && m_previousMouseButtons[button];
    }

    return false;
}

void Input::SetMousePosition(int x, int y)
{
    if(!m_mousePositionInitialized){
        m_mouseX = x;
        m_mouseY = y;
    
        m_previousMouseX = x;
        m_previousMouseY = y;

        m_mousePositionInitialized = true;
        return;
    }

    m_mouseX = x;
    m_mouseY = y;
}

int Input::GetMouseX() const
{
    return m_mouseX;
}

int Input::GetMouseY() const
{
    return m_mouseY;
}

int Input::GetMouseDeltaX() const
{
    return m_mouseX - m_previousMouseX;
}

int Input::GetMouseDeltaY() const
{
    return m_mouseY - m_previousMouseY;
}

void Input::SetScroll(float x, float y)
{
    m_scrollX += x;
    m_scrollY += y;
}

float Input::GetScrollX() const
{
    return m_scrollX;
}

float Input::GetScrollY() const
{
    return m_scrollY;
}