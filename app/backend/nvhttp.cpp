#include "nvcomputer.h"
#include "nvhttp.h"
#include "nvcomputer.h"
#include "identitymanager.h"
#include "settings/streamingpreferences.h"
#include <Limelight.h>

#include <QDebug>
#include <QUuid>
#include <QtNetwork/QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QXmlStreamReader>
#include <QSslKey>
#include <QImageReader>
#include <QtEndian>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QSysInfo>
#include <QRandomGenerator>
#include <QGuiApplication>
#include <QScreen>

#ifdef Q_OS_DARWIN
#include <ApplicationServices/ApplicationServices.h>
#endif

#define FAST_FAIL_TIMEOUT_MS 2000
#define REQUEST_TIMEOUT_MS 5000
#define LAUNCH_TIMEOUT_MS 120000
#define RESUME_TIMEOUT_MS 30000
#define QUIT_TIMEOUT_MS 30000

namespace {
struct ClientDisplayGeometry {
    int width;
    int height;
    int x;
    int y;
    int fps;
    bool primary;
};

ClientDisplayGeometry getClientDisplayGeometry(QScreen* screen, int screenIndex)
{
#ifdef Q_OS_DARWIN
    constexpr uint32_t kMaxDisplays = 32;
    CGDirectDisplayID displayIds[kMaxDisplays];
    uint32_t displayCount = 0;

    if (CGGetActiveDisplayList(kMaxDisplays, displayIds, &displayCount) == kCGErrorSuccess &&
        screenIndex >= 0 &&
        static_cast<uint32_t>(screenIndex) < displayCount) {
        const auto displayId = displayIds[screenIndex];
        const CGRect bounds = CGDisplayBounds(displayId);
        const auto width = static_cast<int>(CGDisplayPixelsWide(displayId));
        const auto height = static_cast<int>(CGDisplayPixelsHigh(displayId));

        double refreshRate = 0.0;
        if (auto mode = CGDisplayCopyDisplayMode(displayId)) {
            refreshRate = CGDisplayModeGetRefreshRate(mode);
            CFRelease(mode);
        }

        return ClientDisplayGeometry {
            width,
            height,
            static_cast<int>(bounds.origin.x),
            static_cast<int>(bounds.origin.y),
            qRound((refreshRate > 0.0 ? refreshRate : screen->refreshRate()) * 1000.0),
            CGDisplayIsMain(displayId)
        };
    }
#endif

    const QRect geometry = screen->geometry();
    const qreal scaleFactor = screen->devicePixelRatio();
    return ClientDisplayGeometry {
        qRound(geometry.width() * scaleFactor),
        qRound(geometry.height() * scaleFactor),
        qRound(geometry.x() * scaleFactor),
        qRound(geometry.y() * scaleFactor),
        qRound(screen->refreshRate() * 1000.0),
        screen == QGuiApplication::primaryScreen()
    };
}

QByteArray serializeClientDisplays(const QStringList& selectedDisplayIds)
{
    QJsonArray displays;
    const QList<QScreen*> screens = QGuiApplication::screens();
    bool skippedForSelection = false;

    for (int screenIndex = 0; screenIndex < screens.count(); ++screenIndex) {
        QScreen* screen = screens.at(screenIndex);
        if (screen == nullptr) {
            continue;
        }

        if (!selectedDisplayIds.isEmpty() && !selectedDisplayIds.contains(screen->name())) {
            skippedForSelection = true;
            continue;
        }

        const auto displayGeometry = getClientDisplayGeometry(screen, screenIndex);

        QJsonObject display;
        display["id"] = screen->name();
        display["width"] = displayGeometry.width;
        display["height"] = displayGeometry.height;
        display["fps"] = displayGeometry.fps > 0 ? displayGeometry.fps : 60000;
        display["x"] = displayGeometry.x;
        display["y"] = displayGeometry.y;
        display["primary"] = displayGeometry.primary;
        displays.append(display);
    }

    if (displays.isEmpty() && skippedForSelection) {
        for (int screenIndex = 0; screenIndex < screens.count(); ++screenIndex) {
            QScreen* screen = screens.at(screenIndex);
            if (screen == nullptr) {
                continue;
            }

            const auto displayGeometry = getClientDisplayGeometry(screen, screenIndex);

            QJsonObject display;
            display["id"] = screen->name();
            display["width"] = displayGeometry.width;
            display["height"] = displayGeometry.height;
            display["fps"] = displayGeometry.fps > 0 ? displayGeometry.fps : 60000;
            display["x"] = displayGeometry.x;
            display["y"] = displayGeometry.y;
            display["primary"] = displayGeometry.primary;
            displays.append(display);
        }
    }

    return QJsonDocument(displays).toJson(QJsonDocument::Compact);
}
}

NvHTTP::NvHTTP(NvAddress address, uint16_t httpsPort, QSslCertificate serverCert) :
    m_ServerCert(serverCert)
{
    m_BaseUrlHttp.setScheme("http");
    m_BaseUrlHttps.setScheme("https");

    setAddress(address);
    setHttpsPort(httpsPort);

    // Never use a proxy server
    QNetworkProxy noProxy(QNetworkProxy::NoProxy);
    m_Nam.setProxy(noProxy);

    connect(&m_Nam, &QNetworkAccessManager::sslErrors, this, &NvHTTP::handleSslErrors);
}

NvHTTP::NvHTTP(NvComputer* computer) :
    NvHTTP(computer->activeAddress, computer->activeHttpsPort, computer->serverCert)
{

}

void NvHTTP::setServerCert(QSslCertificate serverCert)
{
    m_ServerCert = serverCert;
}

void NvHTTP::setAddress(NvAddress address)
{
    Q_ASSERT(!address.isNull());

    m_Address = address;

    m_BaseUrlHttp.setHost(address.address());
    m_BaseUrlHttps.setHost(address.address());

    m_BaseUrlHttp.setPort(address.port());
}

void NvHTTP::setHttpsPort(uint16_t port)
{
    m_BaseUrlHttps.setPort(port);
}

NvAddress NvHTTP::address()
{
    return m_Address;
}

QSslCertificate NvHTTP::serverCert()
{
    return m_ServerCert;
}

uint16_t NvHTTP::httpPort()
{
    return m_BaseUrlHttp.port();
}

uint16_t NvHTTP::httpsPort()
{
    return m_BaseUrlHttps.port();
}

QVector<int>
NvHTTP::parseQuad(QString quad)
{
    QVector<int> ret;

    // Return an empty vector for old GFE versions
    // that were missing GfeVersion.
    if (quad.isEmpty()) {
        return ret;
    }

    QStringList parts = quad.split(".");
    ret.reserve(parts.length());
    for (int i = 0; i < parts.length(); i++)
    {
        ret.append(parts.at(i).toInt());
    }

    return ret;
}

int
NvHTTP::getCurrentGame(QString serverInfo)
{
    // GFE 2.8 started keeping currentgame set to the last game played. As a result, it no longer
    // has the semantics that its name would indicate. To contain the effects of this change as much
    // as possible, we'll force the current game to zero if the server isn't in a streaming session.
    QString serverState = getXmlString(serverInfo, "state");
    if (serverState != nullptr && serverState.endsWith("_SERVER_BUSY"))
    {
        return getXmlString(serverInfo, "currentgame").toInt();
    }
    else
    {
        return 0;
    }
}

QString
NvHTTP::getServerInfo(NvLogLevel logLevel, bool fastFail)
{
    QString serverInfo;
    
    // Add devicename parameter to match Android client behavior
    QString deviceName = QSysInfo::machineHostName();
    if (deviceName.isEmpty()) {
        deviceName = "Artemis";
    }
    QString deviceNameParam = "devicename=" + deviceName;

    // Check if we have a pinned cert and HTTPS port for this host yet
    if (!m_ServerCert.isNull() && httpsPort() != 0)
    {
        // If we have a server cert, we must use HTTPS.
        serverInfo = openConnectionToString(m_BaseUrlHttps,
                                            "serverinfo",
                                            deviceNameParam,
                                            fastFail ? FAST_FAIL_TIMEOUT_MS : REQUEST_TIMEOUT_MS,
                                            logLevel);
        // Only log response if not suppressing output (polling requests use NVLL_NONE)
        if (logLevel != NvLogLevel::NVLL_NONE) {
            qInfo() << "getServerInfo HTTPS response:" << serverInfo;
        }
        verifyResponseStatus(serverInfo);
    }
    else
    {
        // Only use HTTP prior to pairing or fetching HTTPS port
        serverInfo = openConnectionToString(m_BaseUrlHttp,
                                            "serverinfo",
                                            deviceNameParam,
                                            fastFail ? FAST_FAIL_TIMEOUT_MS : REQUEST_TIMEOUT_MS,
                                            logLevel);
        // Only log response if not suppressing output (polling requests use NVLL_NONE)
        if (logLevel != NvLogLevel::NVLL_NONE) {
            qInfo() << "getServerInfo response:" << serverInfo;
        }

        // Populate the HTTPS port
        uint16_t httpsPort = getXmlString(serverInfo, "HttpsPort").toUShort();
        if (httpsPort == 0) {
            httpsPort = DEFAULT_HTTPS_PORT;
        }
        setHttpsPort(httpsPort);

        // If we just needed to determine the HTTPS port, we'll try again over
        // HTTPS now that we have the port number
        if (!m_ServerCert.isNull()) {
            return getServerInfo(logLevel, fastFail);
        }
    }

    return serverInfo;
}

void
NvHTTP::startApp(QString verb,
                bool isGfe,
                int appId,
                QString appUuid,
                PSTREAM_CONFIGURATION streamConfig,
                bool sops,
                bool localAudio,
                int gamepadMask,
                bool persistGameControllersOnDisconnect,
                QString& rtspSessionUrl)
{
    int riKeyId;

    memcpy(&riKeyId, streamConfig->remoteInputAesIv, sizeof(riKeyId));
    riKeyId = qFromBigEndian(riKeyId);

    // Get streaming preferences for Apollo parameters
    StreamingPreferences* prefs = StreamingPreferences::get();
    
    // Build base parameters - prefer UUID when available, fallback to appId
    QString baseParams = "appid="+QString::number(appId)+
                        "&mode="+QString::number(streamConfig->width)+"x"+
                        QString::number(streamConfig->height)+"x";

    // TODO: Future State - replace teh above block with the below.
    // Build base parameters - prefer UUID when available, fallback to appId
    //QString baseParams;

    //if (!appUuid.isEmpty()) {
        // If a UUID is present, use it and omit the appID
    //    baseParams = "appuuid=" + appUuid;
    //    qInfo() << "Launching with UUID:" << appUuid;
    //} else {
        // Otherwise, fall back to using the appID
    //    baseParams = "appid=" + QString::number(appId);
    //    qInfo() << "Launching with App ID:" << appId;
    //}

    //baseParams += "&mode=" + QString::number(streamConfig->width) + "x" +
    //            QString::number(streamConfig->height) + "x";
    
    // Handle fractional refresh rate for Apollo servers
    const bool useSoleDisplayFractionalRefreshRate = prefs->soleDisplay && streamConfig->fps > 1000;
    if (useSoleDisplayFractionalRefreshRate || (!prefs->soleDisplay && prefs->enableFractionalRefreshRate)) {
        const double requestedRefreshRate = useSoleDisplayFractionalRefreshRate ?
            (static_cast<double>(streamConfig->fps) / 1000.0) :
            prefs->customRefreshRate;

        // Send fractional rate directly (Apollo will handle the conversion)
        baseParams += QString::number(requestedRefreshRate, 'f', 2);
        qInfo() << "Using fractional refresh rate:" << requestedRefreshRate << "Hz";
    } else {
        // Using an FPS value over 60 causes SOPS to default to 720p60,
        // so force it to 0 to ensure the correct resolution is set. We
        // used to use 60 here but that locked the frame rate to 60 FPS
        // on GFE 3.20.3. We don't need this hack for Sunshine.
        baseParams += QString::number((streamConfig->fps > 60 && isGfe) ? 0 : streamConfig->fps);
    }

    // TODO: Remove this block in future state
    if (!appUuid.isEmpty()) {
        baseParams += "&appuuid="+appUuid;
        qInfo() << "Launching app with ID:" << appId << "and UUID:" << appUuid;
    } else {
        qInfo() << "Launching app with ID:" << appId << "(no UUID available)";
    }
    
    const bool effectiveLocalAudio = isGfe ? localAudio : false;
    if (!isGfe && localAudio) {
        qInfo() << "Forcing localAudioPlayMode=0 for Sunshine/Apollo to preserve client audio";
    }

    // Continue with standard parameters
    QString allParams = baseParams +
                    "&additionalStates=1&sops="+QString::number(sops ? 1 : 0)+
                    "&rikey="+QByteArray(streamConfig->remoteInputAesKey, sizeof(streamConfig->remoteInputAesKey)).toHex()+
                    "&rikeyid="+QString::number(riKeyId)+
                    ((streamConfig->supportedVideoFormats & VIDEO_FORMAT_MASK_10BIT) ?
                        "&hdrMode=1&clientHdrCapVersion=0&clientHdrCapSupportedFlagsInUint32=0&clientHdrCapMetaDataId=NV_STATIC_METADATA_TYPE_1&clientHdrCapDisplayData=0x0x0x0x0x0x0x0x0x0x0" :
                        "")+
                    "&localAudioPlayMode="+QString::number(effectiveLocalAudio ? 1 : 0)+
                    "&surroundAudioInfo="+QString::number(SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(streamConfig->audioConfiguration))+
                    "&remoteControllersBitmap="+QString::number(gamepadMask)+
                    "&gcmap="+QString::number(gamepadMask)+
                    "&gcpersist="+QString::number(persistGameControllersOnDisconnect ? 1 : 0);
    
    // Add Apollo-specific parameters
    if (prefs->useVirtualDisplay || prefs->soleDisplay || prefs->allDisplayStreaming) {
        allParams += "&virtualDisplay=1";
        qInfo() << "Requesting virtual display from Apollo server";
    }

    if (prefs->soleDisplay) {
        allParams += "&soleDisplay=1";
        qInfo() << "Requesting sole-display mode from Apollo server";
    }

    if (prefs->allDisplayStreaming && !prefs->soleDisplay) {
        const QByteArray clientDisplays = serializeClientDisplays(prefs->selectedClientDisplayIds);
        allParams += "&multiDisplay=1";
        allParams += "&clientDisplays=" + QString::fromUtf8(clientDisplays.toPercentEncoding());
        qInfo() << "Requesting multi-display streaming with" << clientDisplays;
    }
    
    if (!prefs->soleDisplay && !prefs->allDisplayStreaming && prefs->enableResolutionScaling && prefs->resolutionScaleFactor != 100) {
        allParams += "&scaleFactor=" + QString::number(prefs->resolutionScaleFactor);
        qInfo() << "Requesting resolution scaling:" << prefs->resolutionScaleFactor << "%";
    }
    
    // Add Limelight parameters
    allParams += LiGetLaunchUrlQueryParameters();

    QString response = openConnectionToString(m_BaseUrlHttps,
                                             verb,
                                             allParams,
                                             LAUNCH_TIMEOUT_MS);

    qInfo() << "Launch response:" << response;

    // Throws if the request failed
    verifyResponseStatus(response);

    rtspSessionUrl = getXmlString(response, "sessionUrl0");
}

void
NvHTTP::quitApp()
{
    QString response =
            openConnectionToString(m_BaseUrlHttps,
                                   "cancel",
                                   nullptr,
                                   QUIT_TIMEOUT_MS);

    qInfo() << "Quit response:" << response;

    // Throws if the request failed
    verifyResponseStatus(response);

    // Newer GFE versions will just return success even if quitting fails
    // if we're not the original requester.
    if (getCurrentGame(getServerInfo(NvHTTP::NVLL_ERROR)) != 0) {
        // Generate a synthetic GfeResponseException letting the caller know
        // that they can't kill someone else's stream.
        throw GfeHttpResponseException(599, "");
    }
}

QVector<NvDisplayMode>
NvHTTP::getDisplayModeList(QString serverInfo)
{
    QXmlStreamReader xmlReader(serverInfo);
    QVector<NvDisplayMode> modes;

    while (!xmlReader.atEnd()) {
        while (xmlReader.readNextStartElement()) {
            auto name = xmlReader.name();
            if (name == QString("DisplayMode")) {
                modes.append(NvDisplayMode());
            }
            else if (name == QString("Width")) {
                modes.last().width = xmlReader.readElementText().toInt();
            }
            else if (name == QString("Height")) {
                modes.last().height = xmlReader.readElementText().toInt();
            }
            else if (name == QString("RefreshRate")) {
                modes.last().refreshRate = xmlReader.readElementText().toInt();
            }
        }
    }

    return modes;
}

QVector<NvApp>
NvHTTP::getAppList()
{
    QString appxml = openConnectionToString(m_BaseUrlHttps,
                                            "applist",
                                            nullptr,
                                            REQUEST_TIMEOUT_MS,
                                            NvLogLevel::NVLL_ERROR);
    verifyResponseStatus(appxml);

    QXmlStreamReader xmlReader(appxml);
    QVector<NvApp> apps;
    while (!xmlReader.atEnd()) {
        while (xmlReader.readNextStartElement()) {
            auto name = xmlReader.name();
            if (name == QString("App")) {
                // We must have a valid app before advancing to the next one
                if (!apps.isEmpty() && !apps.last().isInitialized()) {
                    qWarning() << "Invalid applist XML";
                    throw std::runtime_error("Invalid applist XML");
                }
                apps.append(NvApp());
            }
            else if (name == QString("AppTitle")) {
                apps.last().name = xmlReader.readElementText();
            }
            else if (name == QString("ID")) {
                apps.last().id = xmlReader.readElementText().toInt();
            }
            else if (name == QString("UUID")) {
                apps.last().uuid = xmlReader.readElementText();
            }
            else if (name == QString("IsHdrSupported")) {
                apps.last().hdrSupported = xmlReader.readElementText() == "1";
            }
            else if (name == QString("IsAppCollectorGame")) {
                apps.last().isAppCollectorGame = xmlReader.readElementText() == "1";
            }
        }
    }

    return apps;
}

void
NvHTTP::verifyResponseStatus(QString xml)
{
    QXmlStreamReader xmlReader(xml);

    while (xmlReader.readNextStartElement())
    {
        if (xmlReader.name() == QString("root"))
        {
            // Status code can be 0xFFFFFFFF in some rare cases on GFE 3.20.3, and
            // QString::toInt() will fail in that case, so use QString::toUInt()
            // and cast the result to an int instead.
            int statusCode = (int)xmlReader.attributes().value("status_code").toUInt();
            if (statusCode == 200)
            {
                // Successful
                return;
            }
            else
            {
                QString statusMessage = xmlReader.attributes().value("status_message").toString();
                if (statusCode != 401) {
                    // 401 is expected for unpaired PCs when we fetch serverinfo over HTTPS
                    qWarning() << "Request failed:" << statusCode << statusMessage;
                }
                if (statusCode == -1 && statusMessage == "Invalid") {
                    // Special case handling an audio capture error which GFE doesn't
                    // provide any useful status message for.
                    statusCode = 418;
                    statusMessage = tr("Missing audio capture device. Reinstalling GeForce Experience should resolve this error.");
                }
                throw GfeHttpResponseException(statusCode, statusMessage);
            }
        }
    }

    throw GfeHttpResponseException(-1, "Malformed XML (missing root element)");
}

QImage
NvHTTP::getBoxArt(int appId)
{
    QNetworkReply* reply = openConnection(m_BaseUrlHttps,
                                          "appasset",
                                          "appid="+QString::number(appId)+
                                          "&AssetType=2&AssetIdx=0",
                                          REQUEST_TIMEOUT_MS,
                                          NvLogLevel::NVLL_VERBOSE);
    QImage image = QImageReader(reply).read();
    delete reply;

    return image;
}

QByteArray
NvHTTP::getXmlStringFromHex(QString xml,
                            QString tagName)
{
    QString str = getXmlString(xml, tagName);
    if (str == nullptr)
    {
        return nullptr;
    }

    return QByteArray::fromHex(str.toLatin1());
}

QString
NvHTTP::getXmlString(QString xml,
                     QString tagName)
{
    QXmlStreamReader xmlReader(xml);

    while (!xmlReader.atEnd())
    {
        if (xmlReader.readNext() != QXmlStreamReader::StartElement)
        {
            continue;
        }

        if (xmlReader.name() == tagName)
        {
            return xmlReader.readElementText();
        }
    }

    return nullptr;
}

QStringList
NvHTTP::getXmlArray(QString xml, QString tagName)
{
    QStringList result;
    QXmlStreamReader xmlReader(xml);

    while (!xmlReader.atEnd())
    {
        if (xmlReader.readNext() != QXmlStreamReader::StartElement)
        {
            continue;
        }

        if (xmlReader.name() == tagName)
        {
            result.append(xmlReader.readElementText());
        }
    }

    return result;
}

void NvHTTP::handleSslErrors(QNetworkReply* reply, const QList<QSslError>& errors)
{
    bool ignoreErrors = true;

    if (m_ServerCert.isNull()) {
        // We should never make an HTTPS request without a cert
        Q_ASSERT(!m_ServerCert.isNull());
        return;
    }

    for (const QSslError& error : errors) {
        if (m_ServerCert != error.certificate()) {
            ignoreErrors = false;
            break;
        }
    }

    if (ignoreErrors) {
        reply->ignoreSslErrors(errors);
    }
}

QString
NvHTTP::openConnectionToString(QUrl baseUrl,
                               QString command,
                               QString arguments,
                               int timeoutMs,
                               NvLogLevel logLevel)
{
    QNetworkReply* reply = openConnection(baseUrl, command, arguments, timeoutMs, logLevel);
    QString ret;

    QTextStream stream(reply);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    stream.setEncoding(QStringConverter::Utf8);
#else
    stream.setCodec("UTF-8");
#endif

    ret = stream.readAll();
    delete reply;

    return ret;
}

QNetworkReply*
NvHTTP::openConnection(QUrl baseUrl,
                       QString command,
                       QString arguments,
                       int timeoutMs,
                       NvLogLevel logLevel)
{
    // Suppress debug output for polling requests to reduce log noise
    bool suppressDebugOutput = (logLevel == NvLogLevel::NVLL_NONE);
    if (!suppressDebugOutput) {
        qDebug() << "NvHTTP::openConnection - URL:" << baseUrl.toString() << "Command:" << command << "Arguments:" << arguments;
    }

    // Port must be set
    Q_ASSERT(baseUrl.port(0) != 0);

    // Build a URL for the request
    QUrl url(baseUrl);
    url.setPath("/" + command);

    // Use a machine-specific UID to match Apollo server expectations
    // Generate a uniqueid based on hostname + timestamp for uniqueness
    static QString machineUniqueId;
    if (machineUniqueId.isEmpty()) {
        QString hostname = QSysInfo::machineHostName();
        if (hostname.isEmpty()) hostname = "artemis";
        // Take first 8 chars of hostname and pad with random hex
        QString hostPart = hostname.left(8).toUpper();
        while (hostPart.length() < 8) {
            hostPart += QString("%1").arg(QRandomGenerator::global()->bounded(16), 1, 16).toUpper();
        }
        // Add 8 random hex chars
        QString randomPart;
        for (int i = 0; i < 8; i++) {
            randomPart += QString("%1").arg(QRandomGenerator::global()->bounded(16), 1, 16).toUpper();
        }
        machineUniqueId = hostPart + randomPart;
    }
    url.setQuery("uniqueid=" + machineUniqueId + "&uuid=" +
                 QUuid::createUuid().toString(QUuid::WithoutBraces) +
                 ((arguments != nullptr) ? ("&" + arguments) : ""));

    QNetworkRequest request(url);

    // Add our client certificate
    request.setSslConfiguration(IdentityManager::get()->getSslConfig());

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Disable HTTP/2 (GFE 3.22 doesn't like it) and Qt 6 enables it by default
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0) && QT_VERSION < QT_VERSION_CHECK(5, 15, 1) && !defined(QT_NO_BEARERMANAGEMENT)
    // HACK: Set network accessibility to work around QTBUG-80947 (introduced in Qt 5.14.0 and fixed in Qt 5.15.1)
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    m_Nam.setNetworkAccessible(QNetworkAccessManager::Accessible);
    QT_WARNING_POP
#endif

    QNetworkReply* reply = m_Nam.get(request);

    // Run the request with a timeout if requested
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &loop, &QEventLoop::quit);
    if (timeoutMs) {
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    }
    if (logLevel >= NvLogLevel::NVLL_VERBOSE) {
        qInfo() << "Executing request:" << url.toString();
    }
    loop.exec(QEventLoop::ExcludeUserInputEvents);

    // Abort the request if it timed out
    if (!reply->isFinished())
    {
        if (logLevel >= NvLogLevel::NVLL_ERROR) {
            qWarning() << "Aborting timed out request for" << url.toString();
        }
        reply->abort();
    }

    // We must clear out cached authentication and connections or
    // GFE will puke next time
    m_Nam.clearAccessCache();

    // Handle error
    if (reply->error() != QNetworkReply::NoError)
    {
        if (logLevel >= NvLogLevel::NVLL_ERROR) {
            qWarning() << command << "request failed with error:" << reply->error();
        }

        if (reply->error() == QNetworkReply::SslHandshakeFailedError) {
            // This will trigger falling back to HTTP for the serverinfo query
            // then pairing again to get the updated certificate.
            GfeHttpResponseException exception(401, "Server certificate mismatch");
            delete reply;
            throw exception;
        }
        else if (reply->error() == QNetworkReply::OperationCanceledError) {
            QtNetworkReplyException exception(QNetworkReply::TimeoutError, "Request timed out");
            delete reply;
            throw exception;
        }
        else {
            QtNetworkReplyException exception(reply->error(), reply->errorString());
            delete reply;
            throw exception;
        }
    }

    return reply;
}

// Artemis clipboard sync methods (Apollo servers only)
QString
NvHTTP::getClipboardContent()
{
    try {
        QString response = openConnectionToString(m_BaseUrlHttps,
                                                  "actions/clipboard",
                                                  "type=text",
                                                  REQUEST_TIMEOUT_MS,
                                                  NvLogLevel::NVLL_VERBOSE);
        
        qDebug() << "NvHTTP: Retrieved clipboard content from server";
        return response;
    }
    catch (const GfeHttpResponseException& e) {
        qWarning() << "NvHTTP: Failed to get clipboard content:" << e.getStatusMessage();
        return QString();
    }
    catch (const QtNetworkReplyException& e) {
        qWarning() << "NvHTTP: Network error getting clipboard:" << e.getErrorText();
        return QString();
    }
}

bool
NvHTTP::sendClipboardContent(const QString& content)
{
    try {
        // Build a URL for the POST request
        QUrl url(m_BaseUrlHttps);
        url.setPath("/actions/clipboard");
        url.setQuery("type=text");

        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "text/plain; charset=utf-8");
        request.setSslConfiguration(IdentityManager::get()->getSslConfig());

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif

        // Send POST request with clipboard content
        QNetworkReply* reply = m_Nam.post(request, content.toUtf8());

        // Wait for response with timeout
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &loop, &QEventLoop::quit);
        QTimer::singleShot(REQUEST_TIMEOUT_MS, &loop, &QEventLoop::quit);
        
        qDebug() << "NvHTTP: Sending clipboard content to server:" << url.toString();
        loop.exec(QEventLoop::ExcludeUserInputEvents);

        // Check for timeout
        if (!reply->isFinished()) {
            qWarning() << "NvHTTP: Clipboard send request timed out";
            reply->abort();
            delete reply;
            return false;
        }

        // Check for network errors
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "NvHTTP: Failed to send clipboard content:" << reply->errorString();
            delete reply;
            return false;
        }

        // Check HTTP status code
        int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        delete reply;

        if (statusCode == 200) {
            qDebug() << "NvHTTP: Successfully sent clipboard content to server";
            return true;
        } else {
            qWarning() << "NvHTTP: Server returned error status:" << statusCode;
            return false;
        }
    }
    catch (const std::exception& e) {
        qWarning() << "NvHTTP: Exception sending clipboard content:" << e.what();
        return false;
    }
}

// Server command methods are now handled through LiSendExecServerCmd in the moonlight-common-c library
// instead of direct HTTP requests. This provides better compatibility with the Apollo protocol.
