#pragma once

#include "SDL_compat.h"

class MacNativeRelativeMouseCapture
{
public:
    MacNativeRelativeMouseCapture();
    ~MacNativeRelativeMouseCapture();

    bool enable(SDL_Window* window);
    void disable();

    bool isActive() const;
    void consumeDeltas(int* xrel, int* yrel);

private:
    struct Impl;
    Impl* m_Impl;
};
