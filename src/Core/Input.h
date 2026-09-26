#pragma once

#include "InputCodes.h"

class Input
{
    public:
        void SetKeyState(unsigned int keyCode, bool pressed);

        bool IsKeyDown(unsigned int keyCode) const;
        bool WasKeyPressed(unsigned int keyCode) const;
        bool WasKeyReleased(unsigned int keyCode) const;

        void SetMouseButtonState(unsigned int button, bool pressed);

        bool IsMouseButtonDown(unsigned int button) const;
        bool WasMouseButtonPressed(unsigned int button) const;
        bool WasMouseButtonReleased(unsigned int button) const;

        void SetMousePosition(int x, int y);

        int GetMouseX() const;
        int GetMouseY() const;

        int GetMouseDeltaX() const;
        int GetMouseDeltaY() const;

        void SetScroll(float x, float y);
        
        float GetScrollX() const;
        float GetScrollY() const;

        void Update();

    private:
        bool m_keys[256]{};
        bool m_previousKeys[256]{};

        bool m_mouseButtons[3]{};
        bool m_previousMouseButtons[3]{};

        int m_mouseX = 0;
        int m_mouseY = 0;

        int m_previousMouseX = 0;
        int m_previousMouseY = 0;

        float m_scrollX = 0.0f;
        float m_scrollY = 0.0f;

        bool m_mousePositionInitialized = false;
};