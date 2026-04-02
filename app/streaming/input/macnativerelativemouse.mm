#include "macnativerelativemouse.h"

#import <ApplicationServices/ApplicationServices.h>

struct MacNativeRelativeMouseCapture::Impl
{
    bool active = false;
    bool cursorHidden = false;
    bool mouseDisassociated = false;
};

MacNativeRelativeMouseCapture::MacNativeRelativeMouseCapture()
    : m_Impl(new Impl())
{
}

MacNativeRelativeMouseCapture::~MacNativeRelativeMouseCapture()
{
    disable();
    delete m_Impl;
}

bool MacNativeRelativeMouseCapture::enable(SDL_Window* window)
{
    if (m_Impl->active) {
        return true;
    }

    if (window == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Native macOS relative capture unavailable: SDL window is null");
        return false;
    }

    // Clear any residual delta before we start polling in capture mode.
    int32_t flushX = 0;
    int32_t flushY = 0;
    CGGetLastMouseDelta(&flushX, &flushY);

    CGError cgError = CGAssociateMouseAndMouseCursorPosition(false);
    if (cgError != kCGErrorSuccess) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Native macOS relative capture unavailable: CGAssociateMouseAndMouseCursorPosition(false) failed: %d",
                    (int)cgError);
        disable();
        return false;
    }
    m_Impl->mouseDisassociated = true;

    cgError = CGDisplayHideCursor(CGMainDisplayID());
    if (cgError != kCGErrorSuccess) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Native macOS relative capture unavailable: CGDisplayHideCursor() failed: %d",
                    (int)cgError);
        disable();
        return false;
    }
    m_Impl->cursorHidden = true;
    m_Impl->active = true;
    return true;
}

void MacNativeRelativeMouseCapture::disable()
{
    if (m_Impl->mouseDisassociated) {
        CGAssociateMouseAndMouseCursorPosition(true);
        m_Impl->mouseDisassociated = false;
    }

    if (m_Impl->cursorHidden) {
        CGDisplayShowCursor(CGMainDisplayID());
        m_Impl->cursorHidden = false;
    }

    int32_t flushX = 0;
    int32_t flushY = 0;
    CGGetLastMouseDelta(&flushX, &flushY);
    m_Impl->active = false;
}

bool MacNativeRelativeMouseCapture::isActive() const
{
    return m_Impl->active;
}

void MacNativeRelativeMouseCapture::consumeDeltas(int* xrel, int* yrel)
{
    int32_t deltaX = 0;
    int32_t deltaY = 0;
    if (m_Impl->active) {
        CGGetLastMouseDelta(&deltaX, &deltaY);
    }

    if (xrel != nullptr) {
        *xrel = static_cast<int>(deltaX);
    }

    if (yrel != nullptr) {
        *yrel = static_cast<int>(deltaY);
    }
}
