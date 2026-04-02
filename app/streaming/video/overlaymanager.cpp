#include "overlaymanager.h"
#include "path.h"

#include <algorithm>

using namespace Overlay;

namespace {

constexpr int kHudWidth = 388;
constexpr int kHudHeight = 852;
constexpr int kHudPadding = 18;
constexpr int kHudSectionGap = 10;

SDL_Color rgba(Uint8 r, Uint8 g, Uint8 b, Uint8 a = 0xFF)
{
    return SDL_Color{r, g, b, a};
}

float clamp01(float value)
{
    return std::max(0.0f, std::min(1.0f, value));
}

SDL_Color lerpColor(SDL_Color a, SDL_Color b, float t)
{
    t = clamp01(t);
    auto mix = [=](Uint8 first, Uint8 second) {
        return static_cast<Uint8>(first + (second - first) * t);
    };
    return rgba(mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a));
}

Uint32 mapColor(SDL_Surface* surface, SDL_Color color)
{
    return SDL_MapRGBA(surface->format, color.r, color.g, color.b, color.a);
}

void putPixel(SDL_Surface* surface, int x, int y, SDL_Color color)
{
    if (surface == nullptr || x < 0 || y < 0 || x >= surface->w || y >= surface->h) {
        return;
    }

    Uint8* row = static_cast<Uint8*>(surface->pixels) + (y * surface->pitch);
    Uint32* pixel = reinterpret_cast<Uint32*>(row) + x;
    *pixel = mapColor(surface, color);
}

void drawLineSegment(SDL_Surface* surface, SDL_Point from, SDL_Point to, SDL_Color color, bool thick = false)
{
    const int dx = std::abs(to.x - from.x);
    const int dy = std::abs(to.y - from.y);
    const int sx = from.x < to.x ? 1 : -1;
    const int sy = from.y < to.y ? 1 : -1;
    int err = dx - dy;
    int x = from.x;
    int y = from.y;

    while (true) {
        putPixel(surface, x, y, color);
        if (thick) {
            putPixel(surface, x, y + 1, color);
        }

        if (x == to.x && y == to.y) {
            break;
        }

        const int err2 = err * 2;
        if (err2 > -dy) {
            err -= dy;
            x += sx;
        }
        if (err2 < dx) {
            err += dx;
            y += sy;
        }
    }
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

void fillGradientRect(SDL_Surface* surface,
                      const SDL_Rect& rect,
                      SDL_Color topColor,
                      SDL_Color bottomColor)
{
    if (rect.w <= 0 || rect.h <= 0) {
        return;
    }

    for (int row = 0; row < rect.h; ++row) {
        const float t = rect.h <= 1 ? 0.0f : (float)row / (float)(rect.h - 1);
        SDL_Rect line{rect.x, rect.y + row, rect.w, 1};
        fillRect(surface, line, lerpColor(topColor, bottomColor, t));
    }
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

int measureTextWidth(TTF_Font* font, const QString& text)
{
    if (font == nullptr || text.isEmpty()) {
        return 0;
    }

    int width = 0;
    int height = 0;
    QByteArray utf8 = text.toUtf8();
    if (TTF_SizeUTF8(font, utf8.constData(), &width, &height) != 0) {
        return 0;
    }

    return width;
}

QString elideText(TTF_Font* font, const QString& text, int maxWidth)
{
    if (font == nullptr || text.isEmpty() || maxWidth <= 0) {
        return QString();
    }

    if (measureTextWidth(font, text) <= maxWidth) {
        return text;
    }

    const QString ellipsis = QStringLiteral("...");
    const int ellipsisWidth = measureTextWidth(font, ellipsis);
    if (ellipsisWidth >= maxWidth) {
        return ellipsis;
    }

    QString fitted = text;
    while (!fitted.isEmpty() && measureTextWidth(font, fitted) + ellipsisWidth > maxWidth) {
        fitted.chop(1);
    }

    return fitted + ellipsis;
}

void drawGraph(SDL_Surface* surface,
               const SDL_Rect& rect,
               const QVector<float>& history,
               float ceiling,
               SDL_Color lineColor,
               SDL_Color fillColor)
{
    fillGradientRect(surface, rect, rgba(11, 18, 30, 224), rgba(7, 11, 21, 236));
    strokeRect(surface, rect, rgba(64, 78, 104, 236));

    for (int i = 1; i <= 3; ++i) {
        SDL_Rect guide{rect.x + 1, rect.y + (rect.h * i) / 4, rect.w - 2, 1};
        fillRect(surface, guide, rgba(26, 34, 50, 255));
    }

    if (history.isEmpty() || ceiling <= 0.0f) {
        return;
    }

    const int sampleCount = std::min((int)history.size(), std::max(2, rect.w - 12));
    if (sampleCount <= 1) {
        return;
    }

    const int startIndex = history.size() - sampleCount;
    const int x0 = rect.x + 5;
    const int y0 = rect.y + rect.h - 6;
    const int usableWidth = rect.w - 10;
    const int usableHeight = rect.h - 12;
    QVector<SDL_Point> points;
    points.reserve(sampleCount);

    for (int i = 0; i < sampleCount; ++i) {
        const float sample = std::max(0.0f, history[startIndex + i]);
        const float normalized = clamp01(sample / ceiling);
        const int x = x0 + ((usableWidth - 1) * i) / (sampleCount - 1);
        const int y = y0 - (int)(usableHeight * normalized);
        points.push_back(SDL_Point{x, y});
    }

    const SDL_Color areaFill = rgba(fillColor.r, fillColor.g, fillColor.b, (Uint8)std::min(96, (int)fillColor.a));
    const SDL_Color glowColor = rgba(lineColor.r, lineColor.g, lineColor.b, 84);

    for (int i = 0; i < points.size(); ++i) {
        const SDL_Point point = points[i];
        if ((i % 2) == 0) {
            fillRect(surface, SDL_Rect{point.x, point.y, 1, std::max(1, y0 - point.y)}, areaFill);
        }
    }

    for (int i = 1; i < points.size(); ++i) {
        drawLineSegment(surface,
                        SDL_Point{points[i - 1].x, points[i - 1].y + 1},
                        SDL_Point{points[i].x, points[i].y + 1},
                        glowColor);
        drawLineSegment(surface, points[i - 1], points[i], lineColor, true);
    }

    const SDL_Point lastPoint = points.back();
    fillRect(surface, SDL_Rect{lastPoint.x - 2, lastPoint.y - 2, 5, 5}, rgba(7, 10, 18, 224));
    fillRect(surface, SDL_Rect{lastPoint.x - 1, lastPoint.y - 1, 3, 3}, lineColor);
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
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, kHudWidth, kHudHeight, 32, SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) {
        return nullptr;
    }

    SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_BLEND);
    fillGradientRect(surface, SDL_Rect{0, 0, kHudWidth, kHudHeight}, rgba(10, 10, 12, 230), rgba(4, 4, 5, 238));
    fillRect(surface, SDL_Rect{0, 0, kHudWidth, 2}, rgba(238, 240, 242, 150));
    strokeRect(surface, SDL_Rect{0, 0, kHudWidth, kHudHeight}, rgba(88, 92, 98, 232));

    TTF_Font* titleFont = getHudFont(QStringLiteral("semibold"), m_HudSemiBoldFontData, 20);
    TTF_Font* sectionTitleFont = getHudFont(QStringLiteral("semibold"), m_HudSemiBoldFontData, 12);
    TTF_Font* valueFont = getHudFont(QStringLiteral("semibold"), m_HudSemiBoldFontData, 14);
    TTF_Font* labelFont = getHudFont(QStringLiteral("medium"), m_HudMediumFontData, 10);
    TTF_Font* bodyFont = getHudFont(QStringLiteral("regular"), m_HudRegularFontData, 10);
    TTF_Font* microFont = getHudFont(QStringLiteral("medium"), m_HudMediumFontData, 9);

    const SDL_Color primary = rgba(242, 243, 245);
    const SDL_Color secondary = rgba(186, 190, 196);
    const SDL_Color tertiary = rgba(124, 128, 136);
    const SDL_Color strong = rgba(228, 230, 234);
    const SDL_Color cardBorder = rgba(76, 80, 86, 236);
    const SDL_Color cardTop = rgba(18, 19, 22, 232);
    const SDL_Color cardBottom = rgba(10, 10, 12, 236);
    const SDL_Color traceBright = rgba(234, 236, 239);
    const SDL_Color traceMid = rgba(182, 186, 192);
    const SDL_Color traceDim = rgba(132, 138, 146);

    const int contentWidth = kHudWidth - (kHudPadding * 2);

    auto historyPeak = [](const QVector<float>& history, float fallback) {
        float peak = fallback;
        for (float value : history) {
            peak = std::max(peak, value);
        }
        return peak;
    };

    auto historyAverage = [](const QVector<float>& history, float fallback) {
        if (history.isEmpty()) {
            return fallback;
        }
        float total = 0.0f;
        for (float value : history) {
            total += value;
        }
        return total / history.size();
    };

    auto drawCard = [&](const SDL_Rect& rect) {
        fillGradientRect(surface, rect, cardTop, cardBottom);
        strokeRect(surface, rect, cardBorder);
        fillRect(surface, SDL_Rect{rect.x, rect.y, rect.w, 1}, rgba(220, 223, 228, 52));
    };

    auto drawRightPill = [&](int rightEdge, int top, const QString& text) {
        const int pillWidth = measureTextWidth(labelFont, text) + 18;
        SDL_Rect pill{rightEdge - pillWidth, top, pillWidth, 20};
        fillGradientRect(surface, pill, rgba(28, 29, 33, 220), rgba(18, 19, 21, 224));
        strokeRect(surface, pill, rgba(98, 102, 110, 220));
        blitText(surface, labelFont, text, secondary, pill.x + 9, pill.y + 4);
        return pillWidth;
    };

    auto drawMetricRow = [&](const SDL_Rect& rect, int y, const QString& label, const QString& value, SDL_Color valueColor) {
        const int leftX = rect.x + 14;
        const int rightX = rect.x + rect.w - 14;
        blitText(surface, bodyFont, label, secondary, leftX, y);

        const int labelWidth = measureTextWidth(bodyFont, label);
        const int maxValueWidth = std::max(24, rightX - (leftX + labelWidth + 18));

        TTF_Font* activeValueFont = bodyFont;
        QString fittedValue = value;
        int valueWidth = measureTextWidth(activeValueFont, fittedValue);

        if (valueWidth > maxValueWidth) {
            activeValueFont = microFont;
            fittedValue = elideText(activeValueFont, fittedValue, maxValueWidth);
            valueWidth = measureTextWidth(activeValueFont, fittedValue);
        }

        blitText(surface, activeValueFont, fittedValue, valueColor, rightX - valueWidth, y);
    };

    auto drawDivider = [&](const SDL_Rect& rect, int y) {
        fillRect(surface, SDL_Rect{rect.x + 14, y, rect.w - 28, 1}, rgba(46, 49, 54, 255));
    };

    int currentY = kHudPadding;
    auto nextSection = [&](int height) {
        SDL_Rect rect{kHudPadding, currentY, contentWidth, height};
        currentY += height + kHudSectionGap;
        return rect;
    };

    const SDL_Rect headerRect = nextSection(104);
    drawCard(headerRect);

    int pillRight = headerRect.x + headerRect.w - 14;
    pillRight -= drawRightPill(pillRight, headerRect.y + 12, QStringLiteral("LIVE")) + 8;
    drawRightPill(pillRight,
                  headerRect.y + 12,
                  stats.streamFps > 0.0f ?
                      QStringLiteral("%1 FPS target").arg(QString::number(stats.streamFps, 'f', 0)) :
                      QStringLiteral("Variable FPS"));

    const QString rendererLine = elideText(microFont,
                                           stats.rendererName.isEmpty() ?
                                               QStringLiteral("Renderer unavailable") :
                                               stats.rendererName,
                                           headerRect.w - 28);
    int headerY = headerRect.y + 12;
    headerY += blitText(surface, labelFont, QStringLiteral("ARTEMIS TELEMETRY"), tertiary, headerRect.x + 14, headerY);
    headerY += 4;
    headerY += blitText(surface,
                        titleFont,
                        QStringLiteral("%1 x %2")
                            .arg(stats.streamWidth)
                            .arg(stats.streamHeight),
                        primary,
                        headerRect.x + 14,
                        headerY);
    headerY += 2;
    headerY += blitText(surface,
                        sectionTitleFont,
                        stats.codecName.isEmpty() ? QStringLiteral("Unknown codec") : stats.codecName,
                        strong,
                        headerRect.x + 14,
                        headerY);
    headerY += 2;
    headerY += blitText(surface,
                        microFont,
                        rendererLine,
                        secondary,
                        headerRect.x + 14,
                        headerY);
    blitText(surface,
             microFont,
             QStringLiteral("In %1   Dec %2   Ren %3")
                 .arg(QString::number(stats.incomingFps, 'f', 1))
                 .arg(QString::number(stats.decodedFps, 'f', 1))
                 .arg(QString::number(stats.renderedFps, 'f', 1)),
             tertiary,
             headerRect.x + 14,
             headerRect.y + headerRect.h - 18);

    const SDL_Rect summaryRect = nextSection(146);
    drawCard(summaryRect);
    blitText(surface, sectionTitleFont, QStringLiteral("SESSION"), primary, summaryRect.x + 14, summaryRect.y + 12);

    const float renderRatio = stats.streamFps > 0.0f ? stats.renderedFps / stats.streamFps : 1.0f;
    int summaryY = summaryRect.y + 32;
    drawMetricRow(summaryRect, summaryY, QStringLiteral("Rendered"), QStringLiteral("%1 FPS").arg(QString::number(stats.renderedFps, 'f', 1)), renderRatio >= 0.97f ? primary : strong);
    summaryY += 18;
    drawMetricRow(summaryRect, summaryY, QStringLiteral("Target"), QStringLiteral("%1 FPS").arg(QString::number(stats.streamFps, 'f', 0)), strong);
    summaryY += 18;
    drawMetricRow(summaryRect, summaryY, QStringLiteral("Incoming"), QStringLiteral("%1 FPS").arg(QString::number(stats.incomingFps, 'f', 1)), strong);
    summaryY += 18;
    drawMetricRow(summaryRect, summaryY, QStringLiteral("Throughput"), QStringLiteral("%1 Mbps").arg(QString::number(stats.bandwidthMbps, 'f', 1)), strong);
    summaryY += 18;
    drawMetricRow(summaryRect, summaryY, QStringLiteral("RTT"), stats.networkLatencyMs > 0.0f ? QStringLiteral("%1 ms").arg(QString::number(stats.networkLatencyMs, 'f', 1)) : QStringLiteral("N/A"), strong);
    summaryY += 18;
    drawMetricRow(summaryRect, summaryY, QStringLiteral("Variance"), QStringLiteral("%1 ms").arg(QString::number(stats.networkLatencyVarianceMs, 'f', 1)), strong);
    summaryY += 18;
    drawMetricRow(summaryRect,
                  summaryY,
                  QStringLiteral("Loss/Jitter"),
                  QStringLiteral("%1%% / %2%%")
                      .arg(QString::number(stats.networkDropPercent, 'f', 1))
                      .arg(QString::number(stats.jitterDropPercent, 'f', 1)),
                  strong);

    const SDL_Rect systemRect = nextSection(206);
    drawCard(systemRect);
    blitText(surface, sectionTitleFont, QStringLiteral("PIPELINE"), primary, systemRect.x + 14, systemRect.y + 12);
    blitText(surface,
             microFont,
             QStringLiteral("Delivery headroom %1%%")
                 .arg(QString::number(std::max(0.0f, renderRatio) * 100.0f, 'f', 0)),
             tertiary,
             systemRect.x + systemRect.w - 130,
             systemRect.y + 13);

    int pipelineY = systemRect.y + 34;
    drawMetricRow(systemRect, pipelineY, QStringLiteral("Incoming"), QStringLiteral("%1 FPS").arg(QString::number(stats.incomingFps, 'f', 1)), strong);
    pipelineY += 20;
    drawMetricRow(systemRect, pipelineY, QStringLiteral("Decoded"), QStringLiteral("%1 FPS").arg(QString::number(stats.decodedFps, 'f', 1)), strong);
    pipelineY += 20;
    drawMetricRow(systemRect, pipelineY, QStringLiteral("Rendered"), QStringLiteral("%1 FPS").arg(QString::number(stats.renderedFps, 'f', 1)), strong);
    pipelineY += 24;
    drawDivider(systemRect, pipelineY - 8);
    drawMetricRow(systemRect, pipelineY, QStringLiteral("Decode"), QStringLiteral("%1 ms").arg(QString::number(stats.averageDecodeTimeMs, 'f', 2)), strong);
    pipelineY += 20;
    drawMetricRow(systemRect, pipelineY, QStringLiteral("Queue"), QStringLiteral("%1 ms").arg(QString::number(stats.averageQueueDelayMs, 'f', 2)), strong);
    pipelineY += 20;
    drawMetricRow(systemRect, pipelineY, QStringLiteral("Render"), QStringLiteral("%1 ms").arg(QString::number(stats.averageRenderTimeMs, 'f', 2)), strong);
    pipelineY += 24;
    drawDivider(systemRect, pipelineY - 8);
    drawMetricRow(systemRect,
                  pipelineY,
                  QStringLiteral("Host latency"),
                  QStringLiteral("%1 / %2 / %3 ms")
                      .arg(QString::number(stats.minHostLatencyMs, 'f', 1))
                      .arg(QString::number(stats.averageHostLatencyMs, 'f', 1))
                      .arg(QString::number(stats.maxHostLatencyMs, 'f', 1)),
                  strong);
    pipelineY += 20;
    drawMetricRow(systemRect,
                  pipelineY,
                  QStringLiteral("Loss / Jitter"),
                  QStringLiteral("%1%% / %2%%")
                      .arg(QString::number(stats.networkDropPercent, 'f', 1))
                      .arg(QString::number(stats.jitterDropPercent, 'f', 1)),
                  strong);

    const SDL_Rect trendsRect = nextSection(340);
    drawCard(trendsRect);
    blitText(surface, sectionTitleFont, QStringLiteral("TRENDS"), primary, trendsRect.x + 14, trendsRect.y + 12);
    blitText(surface,
             microFont,
             QStringLiteral("Frame pace, throughput, and latency over the same sample window"),
             tertiary,
             trendsRect.x + 14,
             trendsRect.y + 28,
             trendsRect.w - 28);

    auto drawTrendRow = [&](int top,
                            const QString& title,
                            const QString& currentValue,
                            const QString& detail,
                            const QVector<float>& history,
                            float ceiling,
                            SDL_Color lineTone,
                            SDL_Color fillTone) {
        const int rowHeight = 88;
        const SDL_Rect rowRect{trendsRect.x + 14, top, trendsRect.w - 28, rowHeight};
        fillGradientRect(surface, rowRect, rgba(22, 23, 26, 214), rgba(12, 12, 14, 220));
        strokeRect(surface, rowRect, rgba(58, 61, 66, 220));
        blitText(surface, sectionTitleFont, title, primary, rowRect.x + 12, rowRect.y + 10);
        const int currentWidth = measureTextWidth(valueFont, currentValue);
        blitText(surface, valueFont, currentValue, lineTone, rowRect.x + rowRect.w - 12 - currentWidth, rowRect.y + 8);
        blitText(surface, microFont, detail, secondary, rowRect.x + 12, rowRect.y + 28, rowRect.w - 24);
        drawGraph(surface,
                  SDL_Rect{rowRect.x + 12, rowRect.y + 46, rowRect.w - 24, 28},
                  history,
                  std::max(ceiling, 1.0f),
                  lineTone,
                  fillTone);
    };

    const float latencyPeak = historyPeak(stats.latencyHistory, std::max(4.0f, stats.networkLatencyMs));
    drawTrendRow(trendsRect.y + 52,
                 QStringLiteral("Frame Pace"),
                 QStringLiteral("%1 FPS").arg(QString::number(stats.renderedFps, 'f', 1)),
                 QStringLiteral("Target %1 FPS   Decoded %2 FPS")
                     .arg(QString::number(stats.streamFps, 'f', 0))
                     .arg(QString::number(stats.decodedFps, 'f', 1)),
                 stats.renderedFpsHistory,
                 historyPeak(stats.renderedFpsHistory, std::max(60.0f, stats.streamFps * 1.08f)),
                 traceBright,
                 rgba(236, 238, 242, 42));
    drawTrendRow(trendsRect.y + 150,
                 QStringLiteral("Throughput"),
                 QStringLiteral("%1 Mbps").arg(QString::number(stats.bandwidthMbps, 'f', 1)),
                 QStringLiteral("Net loss %1%%   Jitter %2%%")
                     .arg(QString::number(stats.networkDropPercent, 'f', 1))
                     .arg(QString::number(stats.jitterDropPercent, 'f', 1)),
                 stats.bandwidthHistory,
                 historyPeak(stats.bandwidthHistory, std::max(15.0f, stats.bandwidthMbps * 1.15f)),
                 traceMid,
                 rgba(188, 192, 198, 36));
    drawTrendRow(trendsRect.y + 248,
                 QStringLiteral("Latency"),
                 stats.networkLatencyMs > 0.0f ?
                     QStringLiteral("%1 ms").arg(QString::number(stats.networkLatencyMs, 'f', 1)) :
                     QStringLiteral("N/A"),
                 QStringLiteral("Average %1 ms   Peak %2 ms")
                     .arg(QString::number(historyAverage(stats.latencyHistory, stats.networkLatencyMs), 'f', 1))
                     .arg(QString::number(latencyPeak, 'f', 1)),
                 stats.latencyHistory,
                 std::max(4.0f, latencyPeak * 1.08f),
                 traceDim,
                 rgba(144, 148, 154, 34));

    return surface;
}
