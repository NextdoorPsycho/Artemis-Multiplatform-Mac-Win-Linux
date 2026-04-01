#pragma once

#include <QString>
#include <QHash>
#include <QVector>

#include "SDL_compat.h"
#include <SDL_ttf.h>

namespace Overlay {

enum OverlayType {
    OverlayDebug,
    OverlayStatusUpdate,
    OverlayServerCommands,
    OverlayMax
};

class IOverlayRenderer
{
public:
    virtual ~IOverlayRenderer() = default;

    virtual void notifyOverlayUpdated(OverlayType type) = 0;
};

class OverlayManager
{
public:
    struct DebugOverlaySnapshot {
        bool valid = false;
        int streamWidth = 0;
        int streamHeight = 0;
        QString codecName;
        QString rendererName;
        float streamFps = 0.0f;
        float incomingFps = 0.0f;
        float decodedFps = 0.0f;
        float renderedFps = 0.0f;
        float bandwidthMbps = 0.0f;
        float networkDropPercent = 0.0f;
        float jitterDropPercent = 0.0f;
        float networkLatencyMs = 0.0f;
        float networkLatencyVarianceMs = 0.0f;
        float averageDecodeTimeMs = 0.0f;
        float averageQueueDelayMs = 0.0f;
        float averageRenderTimeMs = 0.0f;
        float minHostLatencyMs = 0.0f;
        float averageHostLatencyMs = 0.0f;
        float maxHostLatencyMs = 0.0f;
        QVector<float> renderedFpsHistory;
        QVector<float> bandwidthHistory;
        QVector<float> latencyHistory;
    };

    OverlayManager();
    ~OverlayManager();

    bool isOverlayEnabled(OverlayType type);
    char* getOverlayText(OverlayType type);
    void updateOverlayText(OverlayType type, const char* text);
    int getOverlayMaxTextLength();
    void setOverlayTextUpdated(OverlayType type);
    void setOverlayState(OverlayType type, bool enabled);
    SDL_Color getOverlayColor(OverlayType type);
    int getOverlayFontSize(OverlayType type);
    SDL_Surface* getUpdatedOverlaySurface(OverlayType type);
    void updateDebugOverlay(const DebugOverlaySnapshot& snapshot);

    void setOverlayRenderer(IOverlayRenderer* renderer);

private:
    void notifyOverlayUpdated(OverlayType type);
    TTF_Font* getHudFont(const QString& familyKey, const QByteArray& fontData, int pointSize);
    SDL_Surface* renderDebugOverlaySurface();

    struct {
        bool enabled;
        int fontSize;
        SDL_Color color;
        char text[512];
        DebugOverlaySnapshot debugSnapshot;

        TTF_Font* font;
        SDL_Surface* surface;
    } m_Overlays[OverlayMax];
    IOverlayRenderer* m_Renderer;
    QByteArray m_FontData;
    QByteArray m_HudRegularFontData;
    QByteArray m_HudMediumFontData;
    QByteArray m_HudSemiBoldFontData;
    QHash<QString, TTF_Font*> m_HudFonts;
};

}
