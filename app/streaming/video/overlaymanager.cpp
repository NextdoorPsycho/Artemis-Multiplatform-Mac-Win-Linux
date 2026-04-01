#include "overlaymanager.h"
#include "path.h"

#include <algorithm>

using namespace Overlay;

namespace {

constexpr int kHudWidth = 438;
constexpr int kHudHeight = 586;
constexpr int kHudPadding = 16;
constexpr int kHudSectionGap = 10;

SDL_Color rgba(Uint8 r, Uint8 g, Uint8 b, Uint8 a = 0xFF)
{
    return SDL_Color{r, g, b, a};
}

Uint32 mapColor(SDL_Surface* surface, SDL_Color color)
{
    return SDL_MapRGBA(surface->format, color.r, color.g, color.b, color.a);
}

void fillRect(SDL_Surface* surface, const SDL_Rect& rect, SDL_Color color)
{
    SDL_FillRect(surface, &rect, mapColor(surface, color));
}

void strokeRect(SDL_Surface* surface, const SDL_Rect& rect, SDL_Color color)
{
    SDL_Rect top{rect.x, rect.y, rect.w, 1};
    SDL_Rect bottom{rect.x, rect.y + rect.h - 1, rect.w, 1};
    SDL_Rect left{rect.x, rect.y, 1, rect.h};
    SDL_Rect right{rect.x + rect.w - 1, rect.y, 1, rect.h};
    fillRect(surface, top, color);
    fillRect(surface, bottom, color);
    fillRect(surface, left, color);
    fillRect(surface, right, color);
}

int blitText(SDL_Surface* target, TTF_Font* font, const QString& text, SDL_Color color, int x, int y, int wrapWidth = 0)
{
    if (font == nullptr || text.isEmpty()) {
        return 0;
    }

    QByteArray utf8 = text.toUtf8();
    SDL_Surface* textSurface = wrapWidth > 0 ?
                TTF_RenderUTF8_Blended_Wrapped(font, utf8.constData(), color, wrapWidth) :
                TTF_RenderUTF8_Blended(font, utf8.constData(), color);
    if (textSurface == nullptr) {
        return 0;
    }

    SDL_Rect dst{x, y, textSurface->w, textSurface->h};
    SDL_BlitSurface(textSurface, nullptr, target, &dst);
    int height = textSurface->h;
    SDL_FreeSurface(textSurface);
    return height;
}

void drawGraph(SDL_Surface* surface,
               const SDL_Rect& rect,
               const QVector<float>& history,
               float ceiling,
               SDL_Color lineColor,
               SDL_Color fillColor)
{
    fillRect(surface, rect, rgba(12, 12, 15, 235));
    strokeRect(surface, rect, rgba(56, 56, 64, 255));

    for (int i = 1; i <= 3; ++i) {
        SDL_Rect guide{rect.x + 1, rect.y + (rect.h * i) / 4, rect.w - 2, 1};
        fillRect(surface, guide, rgba(34, 34, 40, 255));
    }

    if (history.isEmpty() || ceiling <= 0.0f) {
        return;
    }

    const int usableWidth = std::max(1, rect.w - 8);
    const int barCount = std::min(static_cast<int>(history.size()), usableWidth / 3);
    if (barCount <= 0) {
        return;
    }

    const int startIndex = history.size() - barCount;
    const int stride = std::max(3, usableWidth / std::max(1, barCount));
    for (int i = 0; i < barCount; ++i) {
        float sample = std::max(0.0f, history[startIndex + i]);
        int barHeight = static_cast<int>(((rect.h - 12) * std::min(sample, ceiling)) / ceiling);
        SDL_Rect bar{
            rect.x + 4 + (i * stride),
            rect.y + rect.h - 6 - barHeight,
            std::max(2, stride - 1),
            std::max(1, barHeight)
        };
        fillRect(surface, bar, fillColor);
    }

    SDL_Rect currentMarker{
        rect.x + rect.w - 4,
        rect.y + 4,
        1,
        rect.h - 8
    };
    fillRect(surface, currentMarker, lineColor);
}

}

OverlayManager::OverlayManager() :
    m_Renderer(nullptr),
    m_FontData(Path::readDataFile("ModeSeven.ttf")),
    m_HudRegularFontData(Path::readDataFile("fonts/Geist-Regular.ttf")),
    m_HudMediumFontData(Path::readDataFile("fonts/Geist-Medium.ttf")),
    m_HudSemiBoldFontData(Path::readDataFile("fonts/Geist-SemiBold.ttf"))
{
    for (auto& overlay : m_Overlays) {
        overlay.enabled = false;
        overlay.fontSize = 0;
        overlay.color = {0, 0, 0, 0};
        overlay.text[0] = '\0';
        overlay.font = nullptr;
        overlay.surface = nullptr;
    }

    m_Overlays[OverlayType::OverlayDebug].color = {0xD0, 0xD0, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayDebug].fontSize = 20;

    m_Overlays[OverlayType::OverlayStatusUpdate].color = {0xCC, 0x00, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayStatusUpdate].fontSize = 36;

    m_Overlays[OverlayType::OverlayServerCommands].color = {0x00, 0xCC, 0xCC, 0xFF};
    m_Overlays[OverlayType::OverlayServerCommands].fontSize = 24;

    // While TTF will usually not be initialized here, it is valid for that not to
    // be the case, since Session destruction is deferred and could overlap with
    // the lifetime of a new Session object.
    //SDL_assert(TTF_WasInit() == 0);

    if (TTF_Init() != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TTF_Init() failed: %s",
                    TTF_GetError());
        return;
    }
}

OverlayManager::~OverlayManager()
{
    for (int i = 0; i < OverlayType::OverlayMax; i++) {
        if (m_Overlays[i].surface != nullptr) {
            SDL_FreeSurface(m_Overlays[i].surface);
        }
        if (m_Overlays[i].font != nullptr) {
            TTF_CloseFont(m_Overlays[i].font);
        }
    }

    for (TTF_Font* font : m_HudFonts) {
        if (font != nullptr) {
            TTF_CloseFont(font);
        }
    }

    TTF_Quit();

    // For similar reasons to the comment in the constructor, this will usually,
    // but not always, deinitialize TTF. In the cases where Session objects overlap
    // in lifetime, there may be an additional reference on TTF for the new Session
    // that means it will not be cleaned up here.
    //SDL_assert(TTF_WasInit() == 0);
}

bool OverlayManager::isOverlayEnabled(OverlayType type)
{
    return m_Overlays[type].enabled;
}

char* OverlayManager::getOverlayText(OverlayType type)
{
    return m_Overlays[type].text;
}

void OverlayManager::updateOverlayText(OverlayType type, const char* text)
{
    strncpy(m_Overlays[type].text, text, sizeof(m_Overlays[0].text));
    m_Overlays[type].text[getOverlayMaxTextLength() - 1] = '\0';

    setOverlayTextUpdated(type);
}

int OverlayManager::getOverlayMaxTextLength()
{
    return sizeof(m_Overlays[0].text);
}

int OverlayManager::getOverlayFontSize(OverlayType type)
{
    return m_Overlays[type].fontSize;
}

SDL_Surface* OverlayManager::getUpdatedOverlaySurface(OverlayType type)
{
    // If a new surface is available, return it. If not, return nullptr.
    // Caller must free the surface on success.
    return (SDL_Surface*)SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, nullptr);
}

void OverlayManager::updateDebugOverlay(const DebugOverlaySnapshot& snapshot)
{
    m_Overlays[OverlayType::OverlayDebug].debugSnapshot = snapshot;
    if (m_Overlays[OverlayType::OverlayDebug].enabled) {
        notifyOverlayUpdated(OverlayType::OverlayDebug);
    }
}

void OverlayManager::setOverlayTextUpdated(OverlayType type)
{
    // Only update the overlay state if it's enabled. If it's not enabled,
    // the renderer has already been notified by setOverlayState().
    if (m_Overlays[type].enabled) {
        notifyOverlayUpdated(type);
    }
}

void OverlayManager::setOverlayState(OverlayType type, bool enabled)
{
    bool stateChanged = m_Overlays[type].enabled != enabled;

    m_Overlays[type].enabled = enabled;

    if (stateChanged) {
        if (!enabled) {
            // Set the text to empty string on disable
            m_Overlays[type].text[0] = 0;
        }

        notifyOverlayUpdated(type);
    }
}

SDL_Color OverlayManager::getOverlayColor(OverlayType type)
{
    return m_Overlays[type].color;
}

void OverlayManager::setOverlayRenderer(IOverlayRenderer* renderer)
{
    m_Renderer = renderer;
}

void OverlayManager::notifyOverlayUpdated(OverlayType type)
{
    if (m_Renderer == nullptr) {
        return;
    }

    SDL_Surface* oldSurface = (SDL_Surface*)SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, nullptr);
    if (oldSurface != nullptr) {
        SDL_FreeSurface(oldSurface);
    }

    if (type == OverlayType::OverlayDebug &&
        m_Overlays[type].enabled &&
        m_Overlays[type].debugSnapshot.valid) {
        SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, renderDebugOverlaySurface());
        m_Renderer->notifyOverlayUpdated(type);
        return;
    }

    // Construct the required font to render the overlay
    if (m_Overlays[type].font == nullptr) {
        if (m_FontData.isEmpty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL overlay font failed to load");
            return;
        }

        // m_FontData must stay around until the font is closed
        m_Overlays[type].font = TTF_OpenFontRW(SDL_RWFromConstMem(m_FontData.constData(), m_FontData.size()),
                                               1,
                                               m_Overlays[type].fontSize);
        if (m_Overlays[type].font == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "TTF_OpenFont() failed: %s",
                        TTF_GetError());

            // Can't proceed without a font
            return;
        }
    }

    if (m_Overlays[type].enabled) {
        // The _Wrapped variant is required for line breaks to work
        SDL_Surface* surface = TTF_RenderText_Blended_Wrapped(m_Overlays[type].font,
                                                              m_Overlays[type].text,
                                                              m_Overlays[type].color,
                                                              1024);
        SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, surface);
    }

    // Notify the renderer
    m_Renderer->notifyOverlayUpdated(type);
}

TTF_Font* OverlayManager::getHudFont(const QString& familyKey, const QByteArray& fontData, int pointSize)
{
    const QString cacheKey = familyKey + QStringLiteral(":%1").arg(pointSize);
    auto it = m_HudFonts.constFind(cacheKey);
    if (it != m_HudFonts.constEnd()) {
        return it.value();
    }

    if (fontData.isEmpty()) {
        return nullptr;
    }

    TTF_Font* font = TTF_OpenFontRW(SDL_RWFromConstMem(fontData.constData(), fontData.size()), 1, pointSize);
    if (font == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TTF_OpenFont() failed for HUD font %s: %s",
                    cacheKey.toUtf8().constData(),
                    TTF_GetError());
        return nullptr;
    }

    m_HudFonts.insert(cacheKey, font);
    return font;
}

SDL_Surface* OverlayManager::renderDebugOverlaySurface()
{
    const DebugOverlaySnapshot& stats = m_Overlays[OverlayType::OverlayDebug].debugSnapshot;
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, kHudWidth, kHudHeight, 32, SDL_PIXELFORMAT_RGBA32);
    if (surface == nullptr) {
        return nullptr;
    }

    SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_BLEND);
    fillRect(surface, SDL_Rect{0, 0, kHudWidth, kHudHeight}, rgba(8, 8, 11, 176));
    strokeRect(surface, SDL_Rect{0, 0, kHudWidth, kHudHeight}, rgba(72, 72, 82, 218));

    TTF_Font* titleFont = getHudFont(QStringLiteral("semibold"), m_HudSemiBoldFontData, 16);
    TTF_Font* sectionTitleFont = getHudFont(QStringLiteral("semibold"), m_HudSemiBoldFontData, 13);
    TTF_Font* valueFont = getHudFont(QStringLiteral("semibold"), m_HudSemiBoldFontData, 17);
    TTF_Font* labelFont = getHudFont(QStringLiteral("medium"), m_HudMediumFontData, 10);
    TTF_Font* bodyFont = getHudFont(QStringLiteral("regular"), m_HudRegularFontData, 11);

    const SDL_Color primary = rgba(245, 245, 247);
    const SDL_Color secondary = rgba(163, 163, 173);
    const SDL_Color accent = rgba(122, 196, 255);
    const SDL_Color green = rgba(110, 231, 183);
    const SDL_Color amber = rgba(251, 191, 36);
    const SDL_Color red = rgba(248, 113, 113);
    const SDL_Color sectionFill = rgba(14, 14, 18, 165);
    const SDL_Color sectionBorder = rgba(54, 54, 62, 210);

    int y = kHudPadding;
    y += blitText(surface, labelFont, QStringLiteral("STREAM HUD"), secondary, kHudPadding, y);
    y += 3;
    y += blitText(surface,
                  titleFont,
                  QStringLiteral("%1x%2  %3")
                      .arg(stats.streamWidth)
                      .arg(stats.streamHeight)
                      .arg(stats.codecName.isEmpty() ? QStringLiteral("Unknown codec") : stats.codecName),
                  primary,
                  kHudPadding,
                  y);
    y += blitText(surface,
                  bodyFont,
                  QStringLiteral("Renderer: %1. All values below are measured locally from the active session.")
                      .arg(stats.rendererName.isEmpty() ? QStringLiteral("Unknown renderer") : stats.rendererName),
                  secondary,
                  kHudPadding,
                  y + 1,
                  kHudWidth - (kHudPadding * 2));
    y += 12;

    auto sectionRect = [&](int height) {
        SDL_Rect rect{kHudPadding, y, kHudWidth - (kHudPadding * 2), height};
        fillRect(surface, rect, sectionFill);
        strokeRect(surface, rect, sectionBorder);
        y += height + kHudSectionGap;
        return rect;
    };

    {
        SDL_Rect rect = sectionRect(94);
        int tx = rect.x + 12;
        int ty = rect.y + 10;
        ty += blitText(surface, labelFont, QStringLiteral("CONNECTION OVERVIEW"), secondary, tx, ty);
        ty += 4;
        ty += blitText(surface,
                       valueFont,
                       QStringLiteral("%1 Mbps active throughput").arg(QString::number(stats.bandwidthMbps, 'f', 1)),
                       primary,
                       tx,
                       ty);
        ty += 4;
        ty += blitText(surface,
                       bodyFont,
                       QStringLiteral("Incoming video rate is %1 FPS. Rendering is currently %2 FPS, which is what you are actually seeing on the client.")
                           .arg(QString::number(stats.incomingFps, 'f', 1))
                           .arg(QString::number(stats.renderedFps, 'f', 1)),
                       secondary,
                       tx,
                       ty,
                       rect.w - 24);
    }

    {
        SDL_Rect rect = sectionRect(110);
        int tx = rect.x + 12;
        int ty = rect.y + 10;
        ty += blitText(surface, labelFont, QStringLiteral("FRAME DELIVERY"), secondary, tx, ty);
        ty += 6;
        ty += blitText(surface,
                       bodyFont,
                       QStringLiteral("Incoming from host: %1 FPS").arg(QString::number(stats.incomingFps, 'f', 1)),
                       primary,
                       tx,
                       ty);
        ty += blitText(surface,
                       bodyFont,
                       QStringLiteral("Decoded on client: %1 FPS").arg(QString::number(stats.decodedFps, 'f', 1)),
                       primary,
                       tx,
                       ty + 2);
        ty += blitText(surface,
                       bodyFont,
                       QStringLiteral("Presented to display: %1 FPS").arg(QString::number(stats.renderedFps, 'f', 1)),
                       primary,
                       tx,
                       ty + 2);
        ty += 6;
        ty += blitText(surface,
                       bodyFont,
                       QStringLiteral("Frames dropped by network loss: %1%").arg(QString::number(stats.networkDropPercent, 'f', 1)),
                       stats.networkDropPercent > 1.0f ? amber : secondary,
                       tx,
                       ty);
        ty += blitText(surface,
                       bodyFont,
                       QStringLiteral("Frames dropped by pacing or jitter handling: %1%").arg(QString::number(stats.jitterDropPercent, 'f', 1)),
                       stats.jitterDropPercent > 1.0f ? red : secondary,
                       tx,
                       ty + 2);
    }

    {
        SDL_Rect rect = sectionRect(112);
        int tx = rect.x + 12;
        int ty = rect.y + 10;
        ty += blitText(surface, labelFont, QStringLiteral("LATENCY AND TIMING"), secondary, tx, ty);
        ty += 6;
        ty += blitText(surface,
                       bodyFont,
                       QStringLiteral("Network RTT: %1 ms  |  variance: %2 ms")
                           .arg(stats.networkLatencyMs > 0.0f ? QString::number(stats.networkLatencyMs, 'f', 1) : QStringLiteral("N/A"))
                           .arg(stats.networkLatencyVarianceMs > 0.0f ? QString::number(stats.networkLatencyVarianceMs, 'f', 1) : QStringLiteral("N/A")),
                       primary,
                       tx,
                       ty);
        ty += blitText(surface,
                       bodyFont,
                       QStringLiteral("Host processing: min %1 ms  avg %2 ms  max %3 ms")
                           .arg(QString::number(stats.minHostLatencyMs, 'f', 1))
                           .arg(QString::number(stats.averageHostLatencyMs, 'f', 1))
                           .arg(QString::number(stats.maxHostLatencyMs, 'f', 1)),
                       primary,
                       tx,
                       ty + 2);
        ty += 6;
        ty += blitText(surface,
                       bodyFont,
                       QStringLiteral("Decode time: %1 ms").arg(QString::number(stats.averageDecodeTimeMs, 'f', 2)),
                       secondary,
                       tx,
                       ty);
        ty += blitText(surface,
                       bodyFont,
                       QStringLiteral("Frame queue delay: %1 ms").arg(QString::number(stats.averageQueueDelayMs, 'f', 2)),
                       secondary,
                       tx,
                       ty + 2);
        ty += blitText(surface,
                       bodyFont,
                       QStringLiteral("Render plus display sync: %1 ms").arg(QString::number(stats.averageRenderTimeMs, 'f', 2)),
                       secondary,
                       tx,
                       ty + 2);
    }

    const struct {
        QString title;
        QString subtitle;
        QString currentValue;
        QVector<float> history;
        float ceiling;
        SDL_Color stroke;
        SDL_Color fill;
    } charts[] = {
        {QStringLiteral("Rendered FPS history"),
         QStringLiteral("What the client is actually presenting over time."),
         QStringLiteral("%1 FPS").arg(QString::number(stats.renderedFps, 'f', 1)),
         stats.renderedFpsHistory,
         std::max(stats.streamFps, 60.0f),
         accent,
         rgba(56, 189, 248, 170)},
        {QStringLiteral("Active throughput history"),
         QStringLiteral("Measured incoming video bandwidth, not configured target bitrate."),
         QStringLiteral("%1 Mbps").arg(QString::number(stats.bandwidthMbps, 'f', 1)),
         stats.bandwidthHistory,
         std::max(stats.bandwidthMbps * 1.4f, 20.0f),
         green,
         rgba(74, 222, 128, 170)},
        {QStringLiteral("RTT history"),
         QStringLiteral("Round-trip network latency measured by the client."),
         stats.networkLatencyMs > 0.0f ?
             QStringLiteral("%1 ms").arg(QString::number(stats.networkLatencyMs, 'f', 1)) :
             QStringLiteral("N/A"),
         stats.latencyHistory,
         std::max(stats.networkLatencyMs * 1.5f, 20.0f),
         red,
         rgba(248, 113, 113, 170)}
    };

    for (const auto& chart : charts) {
        SDL_Rect rect = sectionRect(100);
        int tx = rect.x + 12;
        int ty = rect.y + 10;
        ty += blitText(surface, sectionTitleFont, chart.title, primary, tx, ty);
        blitText(surface, bodyFont, chart.currentValue, chart.stroke, rect.x + rect.w - 110, rect.y + 10);
        ty += blitText(surface, bodyFont, chart.subtitle, secondary, tx, ty + 2, rect.w - 24);

        SDL_Rect graphRect{
            rect.x + 12,
            rect.y + 42,
            rect.w - 24,
            46
        };
        drawGraph(surface, graphRect, chart.history, chart.ceiling, chart.stroke, chart.fill);
    }

    return surface;
}
