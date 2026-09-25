#pragma once

#include "InputCodes.h"

class Input{
    public:
        void SetKeyState(unsigned int keyCode, bool pressed);

        bool IsKeyDown(unsigned int keyCode) const;

    private:
        bool m_keys[256]{};
};