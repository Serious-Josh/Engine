#include "Input.h"

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