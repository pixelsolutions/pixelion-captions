#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include "auth-service.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <utility>

// macOS adapter for PixelionAuthService.
//
// The Qt-based OAuth loopback, PKCE, state verification, and token-exchange
// logic is ported directly from the Windows adapter
// (src/platform/windows/auth-service.cpp) so observable behavior matches
// Windows exactly. The native pieces differ by platform and are implemented in
// their own tasks:
//   * Task 3.2 - Keychain refresh-credential storage (saveRefreshToken /
//                readRefreshToken / deleteRefreshToken below) is implemented
//                with Security.framework SecItem on a kSecClassGenericPassword
//                item.
//   * Task 3.3 - loadProfile() (GET /me, GET /me/usage) ported verbatim from
//                the Windows adapter; email and plan reach the state callback.
//   * Task 3.4 - auth error-path mapping (Keychain failures, port-in-use, etc.);
//                the portable inline token-exchange errors are ported as-is.

#ifndef PIXELION_COGNITO_CLIENT_ID
#define PIXELION_COGNITO_CLIENT_ID ""
#endif
#ifndef PIXELION_COGNITO_REDIRECT_URI
#define PIXELION_COGNITO_REDIRECT_URI ""
#endif
#ifndef PIXELION_API_BASE_URL
#define PIXELION_API_BASE_URL "https://9yw5ciothk.execute-api.us-east-1.amazonaws.com/prod"
#endif

namespace {
    constexpr auto kCognitoOrigin = "https://auth.pixelion.ai";
    // Keychain generic-password item identity. kSecAttrService is tied to the
    // bundle id; kSecAttrAccount mirrors the fixed credential target the Windows
    // adapter uses (CRED_TYPE_GENERIC target "PixelionLiveCaptions/refresh-token"),
    // so both platforms key the single refresh credential the same way.
    constexpr auto kKeychainService = "ai.pixelion.captions";
    constexpr auto kKeychainAccount = "PixelionLiveCaptions/refresh-token";
    constexpr int kAuthTimeoutMs = 120000;
    constexpr int kRequestTimeoutMs = 10000;
    constexpr qsizetype kMaxResponseBytes = 1024 * 1024;

    QString configuredClientId()
    {
        return QString::fromUtf8(PIXELION_COGNITO_CLIENT_ID);
    }

    QString configuredRedirectUri()
    {
        return QString::fromUtf8(PIXELION_COGNITO_REDIRECT_URI);
    }

    QString base64Url(const QByteArray &bytes)
    {
        return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    }

    QByteArray secureRandomBytes(int count)
    {
        QByteArray bytes(count, Qt::Uninitialized);
        auto *rng = QRandomGenerator::system();
        for (int i = 0; i < count; i += static_cast<int>(sizeof(quint32))) {
            const quint32 value = rng->generate();
            const auto available = std::min(static_cast<int>(sizeof(value)), count - i);
            for (int j = 0; j < available; ++j)
                bytes[i + j] = static_cast<char>((value >> (j * 8)) & 0xff);
        }
        return bytes;
    }

    // --- Keychain credential storage (Task 3.2) -------------------------------
    //
    // These are the macOS analogues of the Windows Credential Manager helpers
    // (CredWriteW / CredReadW / CredDeleteW). They store the refresh credential in
    // a Security.framework kSecClassGenericPassword item keyed by
    // (kSecAttrService="ai.pixelion.captions", kSecAttrAccount) with
    // kSecAttrAccessibleAfterFirstUnlock. Only the refresh credential is ever
    // persisted; access/id tokens stay in memory only (see finishSignIn).
    //
    // Observable behavior mirrors Windows: save overwrites any existing item and
    // returns success/failure; read returns the stored credential or empty on
    // not-found/error; delete is a no-op when nothing is stored.

    // Base query identifying the single generic-password item. Returned dictionary
    // is autoreleased; callers add the extra keys they need.
    NSMutableDictionary *keychainBaseQuery()
    {
        return [NSMutableDictionary dictionaryWithObjectsAndKeys:(__bridge id) kSecClassGenericPassword,
                                                                 (__bridge id) kSecClass,
                                                                 [NSString stringWithUTF8String:kKeychainService],
                                                                 (__bridge id) kSecAttrService,
                                                                 [NSString stringWithUTF8String:kKeychainAccount],
                                                                 (__bridge id) kSecAttrAccount, nil];
    }

    bool saveRefreshToken(const QString &token)
    {
        const QByteArray utf8 = token.toUtf8();
        NSData *data = [NSData dataWithBytes:utf8.constData() length:static_cast<NSUInteger>(utf8.size())];

        NSMutableDictionary *addQuery = keychainBaseQuery();
        addQuery[(__bridge id) kSecAttrAccessible] = (__bridge id) kSecAttrAccessibleAfterFirstUnlock;
        addQuery[(__bridge id) kSecValueData] = data;

        OSStatus status = SecItemAdd((__bridge CFDictionaryRef) addQuery, nullptr);
        if (status == errSecDuplicateItem) {
            // Item already exists: overwrite its value (CredWriteW semantics).
            NSDictionary *matchQuery = keychainBaseQuery();
            NSDictionary *attributesToUpdate = @ {
                (__bridge id) kSecValueData: data,
                (__bridge id) kSecAttrAccessible: (__bridge id) kSecAttrAccessibleAfterFirstUnlock,
            };
            status =
                SecItemUpdate((__bridge CFDictionaryRef) matchQuery, (__bridge CFDictionaryRef) attributesToUpdate);
        }
        return status == errSecSuccess;
    }

    QString readRefreshToken()
    {
        NSMutableDictionary *query = keychainBaseQuery();
        query[(__bridge id) kSecReturnData] = @YES;
        query[(__bridge id) kSecMatchLimit] = (__bridge id) kSecMatchLimitOne;

        CFTypeRef result = nullptr;
        const OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef) query, &result);
        if (status != errSecSuccess || result == nullptr)
            return {};

        // SecItemCopyMatching returns a CFDataRef we own (Get/Copy rule); copy its
        // bytes out, then release. Works the same under ARC or MRC.
        CFDataRef data = reinterpret_cast<CFDataRef>(result);
        const QByteArray bytes(reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
                               static_cast<qsizetype>(CFDataGetLength(data)));
        CFRelease(data);
        return QString::fromUtf8(bytes);
    }

    void deleteRefreshToken()
    {
        NSDictionary *query = keychainBaseQuery();
        const OSStatus status = SecItemDelete((__bridge CFDictionaryRef) query);
        // errSecItemNotFound is a no-op success (matches CredDeleteW when absent).
        (void) status;
    }

    QString idTokenClaim(const QString &token, const QString &claim)
    {
        const auto parts = token.split('.');
        if (parts.size() < 2)
            return {};
        QByteArray payload = parts.at(1).toLatin1();
        while (payload.size() % 4 != 0)
            payload.append('=');
        const auto decoded = QByteArray::fromBase64(payload, QByteArray::Base64UrlEncoding);
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(decoded, &parseError);
        return parseError.error == QJsonParseError::NoError && document.isObject()
                   ? document.object().value(claim).toString()
                   : QString();
    }

    bool jsonString(const QJsonObject &object, const char *key, QString *out)
    {
        const auto value = object.value(QLatin1String(key));
        if (!value.isString() || value.toString().isEmpty())
            return false;
        *out = value.toString();
        return true;
    }

    QNetworkRequest secureRequest(const QUrl &url)
    {
        QNetworkRequest request(url);
        // Follow only no-less-safe HTTPS redirects. Hosted Cognito can route
        // auth.pixelion.ai to its regional hosted-UI endpoint.
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setTransferTimeout(kRequestTimeoutMs);
        request.setRawHeader("Accept", "application/json");
        return request;
    }
}  // namespace

PixelionAuthService::PixelionAuthService(QObject *parent) :
    QObject(parent),
    network_(new QNetworkAccessManager(this)),
    callbackServer_(new QTcpServer(this)),
    callbackTimeout_(new QTimer(this))
{
    callbackTimeout_->setSingleShot(true);
    connect(callbackServer_, &QTcpServer::newConnection, this, &PixelionAuthService::handleCallback);
    connect(callbackTimeout_, &QTimer::timeout, this, [this] {
        failSignIn(QStringLiteral("Sign-in timed out. Please try again."));
    });
}

PixelionAuthService::~PixelionAuthService()
{
    if (callbackServer_->isListening())
        callbackServer_->close();
}

void PixelionAuthService::setStateCallback(StateCallback callback)
{
    stateCallback_ = std::move(callback);
}

void PixelionAuthService::setUsageCallback(UsageCallback callback)
{
    usageCallback_ = std::move(callback);
    if (usageCallback_)
        usageCallback_(usageInfo_);
}

void PixelionAuthService::publishUsage(const UsageInfo &usage)
{
    usageInfo_ = usage;
    if (usageCallback_)
        usageCallback_(usageInfo_);
}

void PixelionAuthService::refreshUsage()
{
    const quint64 generation = ++usageRequestGeneration_;
    UsageInfo loading;
    loading.loading = true;
    publishUsage(loading);
    withAccessToken([this, generation](bool ok, const QString &access, const QString &) {
        if (generation != usageRequestGeneration_)
            return;
        if (!ok || !isSignedIn()) {
            publishUsage(UsageInfo {});
            return;
        }
        QNetworkRequest request =
            secureRequest(QUrl(QStringLiteral(PIXELION_API_BASE_URL) + QStringLiteral("/me/usage")));
        request.setRawHeader("Authorization", QByteArray("Bearer ") + access.toUtf8());
        auto *reply = network_->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
            const QByteArray body = reply->readAll().left(kMaxResponseBytes);
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool networkOk = reply->error() == QNetworkReply::NoError;
            reply->deleteLater();
            if (generation != usageRequestGeneration_ || !isSignedIn())
                return;
            if (!networkOk || status != 200) {
                publishUsage(UsageInfo {});
                return;
            }
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(body, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                publishUsage(UsageInfo {});
                return;
            }
            const auto object = document.object();
            const auto minutes = object.value(QStringLiteral("minutes")).toObject();
            UsageInfo usage;
            usage.periodKind = object.value(QStringLiteral("period_kind")).toString(QStringLiteral("monthly"));
            usage.unlimited = minutes.value(QStringLiteral("unlimited")).toBool(false);
            if (usage.unlimited) {
                usage.known = true;
            } else {
                const auto usedValue = minutes.value(QStringLiteral("used"));
                const auto limitValue = minutes.value(QStringLiteral("limit"));
                if (usedValue.isDouble() && limitValue.isDouble() && limitValue.toDouble() > 0.0) {
                    const int limit = static_cast<int>(std::floor(limitValue.toDouble()));
                    const int remaining = std::clamp(
                        static_cast<int>(std::floor(limitValue.toDouble() - usedValue.toDouble())), 0, limit);
                    usage.known = true;
                    usage.limitMinutes = limit;
                    usage.usedMinutes = limit - remaining;
                }
            }
            publishUsage(usage.known ? usage : UsageInfo {});
        });
    });
}

void PixelionAuthService::updateUsageRemainingMinutes(int remainingMinutes)
{
    if (!usageInfo_.known || usageInfo_.unlimited || usageInfo_.limitMinutes <= 0)
        return;
    const int remaining = std::clamp(remainingMinutes, 0, usageInfo_.limitMinutes);
    usageInfo_.usedMinutes = usageInfo_.limitMinutes - remaining;
    if (usageCallback_)
        usageCallback_(usageInfo_);
}

bool PixelionAuthService::isSignedIn() const
{
    return !accessToken_.isEmpty() && !email_.isEmpty();
}

QString PixelionAuthService::email() const
{
    return email_;
}
QString PixelionAuthService::userId() const
{
    return userId_;
}
QString PixelionAuthService::plan() const
{
    return plan_;
}

void PixelionAuthService::publish(const QString &message)
{
    if (stateCallback_)
        stateCallback_(email_, plan_, message);
}

void PixelionAuthService::restore()
{
    if (readRefreshToken().isEmpty()) {
        publish(QStringLiteral("Sign in to Pixelion to start a live session."));
        return;
    }
    publish(QStringLiteral("Restoring your Pixelion account…"));
    refreshAccessToken([this](bool ok, const QString &, const QString &error) {
        if (!ok) {
            publish(error.isEmpty() ? QStringLiteral("Could not restore your account. Sign in again.") : error);
            return;
        }
        loadProfile();
    });
}

void PixelionAuthService::signIn(bool createAccount)
{
    if (signInPending_)
        return;
    const QString clientId = configuredClientId();
    const QString redirect = configuredRedirectUri();
    const QUrl redirectUrl(redirect);
    if (clientId.isEmpty() || redirectUrl.scheme() != QStringLiteral("http") ||
        (redirectUrl.host() != QStringLiteral("localhost") && redirectUrl.host() != QStringLiteral("127.0.0.1"))) {
        publish(QStringLiteral("Native Pixelion sign-in is not configured for this build."));
        return;
    }
    if (callbackServer_->isListening())
        callbackServer_->close();
    if (!callbackServer_->listen(QHostAddress::LocalHost, redirectUrl.port())) {
        // Task 3.4: port 19432 in use -> sign-in failure "callback port
        // unavailable", stay signed-out, do not terminate. Same message and
        // behavior as the Windows adapter.
        publish(QStringLiteral("Pixelion Live is using the sign-in callback. Close it, then try again."));
        return;
    }

    pendingVerifier_ = base64Url(secureRandomBytes(32));
    pendingState_ = base64Url(secureRandomBytes(24));
    const auto challenge = base64Url(QCryptographicHash::hash(pendingVerifier_.toUtf8(), QCryptographicHash::Sha256));
    QUrl authUrl(QString::fromLatin1(kCognitoOrigin) +
                 (createAccount ? QStringLiteral("/signup") : QStringLiteral("/oauth2/authorize")));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    query.addQueryItem(QStringLiteral("client_id"), clientId);
    query.addQueryItem(QStringLiteral("redirect_uri"), redirect);
    query.addQueryItem(QStringLiteral("scope"), QStringLiteral("openid email profile"));
    query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    query.addQueryItem(QStringLiteral("code_challenge"), challenge);
    query.addQueryItem(QStringLiteral("state"), pendingState_);
    if (!createAccount)
        query.addQueryItem(QStringLiteral("prompt"), QStringLiteral("login"));
    authUrl.setQuery(query);
    signInPending_ = true;
    callbackTimeout_->start(kAuthTimeoutMs);
    publish(createAccount ? QStringLiteral("Create your Pixelion account in the browser…")
                          : QStringLiteral("Sign in to Pixelion in the browser…"));
    if (!QDesktopServices::openUrl(authUrl))
        failSignIn(QStringLiteral("Could not open your browser for Pixelion sign-in."));
}

void PixelionAuthService::handleCallback()
{
    while (callbackServer_->hasPendingConnections()) {
        auto *socket = callbackServer_->nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
            const QByteArray request = socket->readAll();
            const auto requestLine = request.left(request.indexOf("\r\n"));
            const auto fields = requestLine.split(' ');
            if (fields.size() < 2 || fields.at(0) != "GET") {
                socket->write("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
                socket->disconnectFromHost();
                return;
            }
            QUrl callbackUrl(QStringLiteral("http://localhost") + QString::fromUtf8(fields.at(1)));
            const QString expectedPath = QUrl(configuredRedirectUri()).path();
            QUrlQuery query(callbackUrl);
            const QString state = query.queryItemValue(QStringLiteral("state"));
            const QString code = query.queryItemValue(QStringLiteral("code"));
            const QString error = query.queryItemValue(QStringLiteral("error"));
            QByteArray response =
                "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
                "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><meta name=\"color-scheme\" content=\"light dark\"><title>Pixelion · Sign-in complete</title>"
                "<style>:root{color-scheme:light dark;font-family:Inter,\"Segoe UI\",system-ui,-apple-system,sans-serif;background:#f4f5f8;color:#20232c}*{box-sizing:border-box}body{margin:0;min-height:100vh;display:grid;place-items:center;padding:24px}.card{width:min(100%,440px);padding:36px 34px 30px;border:1px solid #e4e6ed;border-radius:20px;background:#fff;box-shadow:0 16px 48px #20274614;text-align:center}.brand{font-size:13px;font-weight:750;letter-spacing:.13em;color:#5d6371}.logo{display:block;width:76px;height:76px;margin:24px auto 16px;object-fit:contain}h1{margin:0;font-size:24px;letter-spacing:-.025em}p{margin:12px 0 0;color:#626979;font-size:15px;line-height:1.6}.hint{display:inline-block;margin-top:25px;padding:10px 14px;border-radius:10px;background:#f3f2f7;color:#4e5361;font-size:13px;font-weight:600}.foot{margin-top:27px;color:#989daa;font-size:12px}@media(prefers-color-scheme:dark){:root{background:#171920;color:#f3f4f7}.card{background:#22252e;border-color:#343844;box-shadow:0 16px 48px #0004}.brand{color:#b6bac6}p{color:#b1b6c3}.hint{background:#30333e;color:#d5d8e0}.foot{color:#858b99}}</style></head><body><main class=\"card\"><div class=\"brand\">PIXELION CAPTIONS</div><img class=\"logo\" src=\"PIXELION_LOGO_DATA\" alt=\"Pixelion logo\"><h1>You’re signed in</h1><p>Your Pixelion account is connected. Return to OBS to finish setting up live captions.</p><div class=\"hint\">You can close this tab and switch back to OBS.</div><div class=\"foot\">Pixelion Captions</div></main></body></html>";
            QFile logoFile(QStringLiteral(":/pixelion/logo.png"));
            if (logoFile.open(QIODevice::ReadOnly))
                response.replace("PIXELION_LOGO_DATA",
                                 QByteArray("data:image/png;base64,") + logoFile.readAll().toBase64());
            socket->write(response);
            socket->disconnectFromHost();
            // State verification before any code exchange.
            if (!signInPending_ || callbackUrl.path() != expectedPath || state != pendingState_)
                return;
            callbackTimeout_->stop();
            callbackServer_->close();
            if (!error.isEmpty() || code.isEmpty()) {
                failSignIn(error == QStringLiteral("access_denied")
                               ? QStringLiteral("Sign-in was cancelled.")
                               : QStringLiteral("Pixelion sign-in was not completed."));
                return;
            }
            exchangeCode(code);
        });
    }
}

QNetworkReply *PixelionAuthService::postForm(const QUrl &url, const QList<QPair<QString, QString>> &fields)
{
    QUrlQuery query;
    for (const auto &field : fields)
        query.addQueryItem(field.first, field.second);
    QNetworkRequest request = secureRequest(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    return network_->post(request, query.query(QUrl::FullyEncoded).toUtf8());
}

void PixelionAuthService::exchangeCode(const QString &code)
{
    QUrl url(QString::fromLatin1(kCognitoOrigin) + QStringLiteral("/oauth2/token"));
    auto *reply = postForm(url, {
                                    {   QStringLiteral("grant_type"), QStringLiteral("authorization_code")},
                                    {    QStringLiteral("client_id"),                 configuredClientId()},
                                    { QStringLiteral("redirect_uri"),              configuredRedirectUri()},
                                    {         QStringLiteral("code"),                                 code},
                                    {QStringLiteral("code_verifier"),                     pendingVerifier_}
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const QByteArray body = reply->readAll().left(kMaxResponseBytes);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto networkError = reply->error();
        const QString networkErrorText = reply->errorString();
        reply->deleteLater();
        if (networkError != QNetworkReply::NoError || status != 200) {
            QJsonParseError parseError;
            const auto errorDocument = QJsonDocument::fromJson(body, &parseError);
            const QString oauthError = parseError.error == QJsonParseError::NoError && errorDocument.isObject()
                                           ? errorDocument.object().value(QStringLiteral("error")).toString()
                                           : QString();
            QString message;
            if (oauthError == QStringLiteral("invalid_grant"))
                message = QStringLiteral(
                    "Pixelion rejected the sign-in code (expired or already used). Start sign-in again.");
            else if (oauthError == QStringLiteral("invalid_client"))
                message = QStringLiteral("Pixelion rejected this app's sign-in configuration.");
            else if (oauthError == QStringLiteral("invalid_request"))
                message =
                    QStringLiteral("Pixelion rejected the sign-in request. Check the app callback configuration.");
            else if (networkError != QNetworkReply::NoError)
                message =
                    QStringLiteral("Could not reach Pixelion's token service (%1). Check your connection and retry.")
                        .arg(networkErrorText);
            else
                message =
                    QStringLiteral("Pixelion's token service returned HTTP %1. Try signing in again.").arg(status);
            failSignIn(message);
            return;
        }
        finishSignIn(body);
    });
}

void PixelionAuthService::finishSignIn(const QByteArray &body)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        failSignIn(QStringLiteral("Pixelion returned an invalid sign-in response."));
        return;
    }
    const auto object = document.object();
    QString access;
    QString refresh;
    QString id;
    if (!jsonString(object, "access_token", &access) || !jsonString(object, "refresh_token", &refresh)) {
        failSignIn(QStringLiteral("Pixelion sign-in response was incomplete."));
        return;
    }
    jsonString(object, "id_token", &id);
    const qint64 lifetime = object.value(QStringLiteral("expires_in")).toInteger();
    // Only the refresh credential is persisted (Task 3.2 Keychain); access/id
    // tokens and expiry are held in memory only.
    if (lifetime <= 0 || !saveRefreshToken(refresh)) {
        // Task 3.4: Keychain write failure on save -> report sign-in-required,
        // keep running, no persisted signed-in state. Matches the Windows
        // CredWriteW-failure branch (same guard, same user-facing message
        // adapted for macOS wording).
        failSignIn(QStringLiteral("Could not securely save your Pixelion sign-in on this Mac."));
        return;
    }
    accessToken_ = access;
    idToken_ = id;
    accessTokenExpiresAt_ = QDateTime::currentSecsSinceEpoch() + lifetime;
    signInPending_ = false;
    pendingState_.clear();
    pendingVerifier_.clear();
    loadProfile();
}

void PixelionAuthService::failSignIn(const QString &message)
{
    callbackTimeout_->stop();
    callbackServer_->close();
    signInPending_ = false;
    pendingState_.clear();
    pendingVerifier_.clear();
    publish(message);
}

void PixelionAuthService::refreshAccessToken(AccessTokenCallback callback)
{
    const QString refresh = readRefreshToken();
    if (refresh.isEmpty()) {
        callback(false, {}, QStringLiteral("Sign in to Pixelion to start a live session."));
        return;
    }
    QUrl url(QString::fromLatin1(kCognitoOrigin) + QStringLiteral("/oauth2/token"));
    auto *reply = postForm(url, {
                                    {   QStringLiteral("grant_type"), QStringLiteral("refresh_token")},
                                    {    QStringLiteral("client_id"),            configuredClientId()},
                                    {QStringLiteral("refresh_token"),                         refresh}
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback = std::move(callback)]() mutable {
        const QByteArray body = reply->readAll().left(kMaxResponseBytes);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError && status == 200;
        reply->deleteLater();
        if (!ok) {
            if (status == 400 || status == 401) {
                deleteRefreshToken();
                accessToken_.clear();
                idToken_.clear();
                email_.clear();
                plan_.clear();
                publish(QStringLiteral("Your Pixelion sign-in expired. Sign in again."));
            }
            callback(
                false, {},
                status == 0
                    ? QStringLiteral(
                          "Could not reach Pixelion. Live transcription and translation need an internet connection. Check your connection, then try Start again.")
                    : QStringLiteral("Could not refresh your Pixelion sign-in."));
            return;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(body, &parseError);
        QString access;
        QString id;
        if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
            !jsonString(document.object(), "access_token", &access)) {
            callback(false, {}, QStringLiteral("Pixelion returned an invalid refresh response."));
            return;
        }
        jsonString(document.object(), "id_token", &id);
        if (!id.isEmpty())
            idToken_ = id;
        accessToken_ = access;
        accessTokenExpiresAt_ =
            QDateTime::currentSecsSinceEpoch() + document.object().value(QStringLiteral("expires_in")).toInteger(3600);
        callback(true, accessToken_, {});
    });
}

void PixelionAuthService::withAccessToken(AccessTokenCallback callback)
{
    if (!accessToken_.isEmpty() && accessTokenExpiresAt_ > QDateTime::currentSecsSinceEpoch() + 60) {
        callback(true, accessToken_, {});
        return;
    }
    refreshAccessToken(std::move(callback));
}

void PixelionAuthService::loadProfile()
{
    // Ported verbatim from the Windows adapter
    // (src/platform/windows/auth-service.cpp): GET /me for the profile, then
    // refreshUsage() for GET /me/usage. Populate email_/userId_/plan_ with the
    // same id-token-claim fallbacks and publish the signed-in state so the
    // email and plan reach the state callback within 2 s of a valid callback.
    withAccessToken([this](bool ok, const QString &access, const QString &error) {
        if (!ok) {
            publish(error);
            return;
        }
        QUrl url(QString::fromLatin1(PIXELION_API_BASE_URL) + QStringLiteral("/me"));
        auto request = secureRequest(url);
        request.setRawHeader("Authorization", QByteArray("Bearer ") + access.toUtf8());
        auto *reply = network_->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            const QByteArray body = reply->readAll().left(kMaxResponseBytes);
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool ok = reply->error() == QNetworkReply::NoError && status == 200;
            reply->deleteLater();
            if (!ok) {
                publish(QStringLiteral("Signed in, but Pixelion account details could not be loaded."));
                return;
            }
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(body, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                publish(QStringLiteral("Pixelion returned invalid account details."));
                return;
            }
            const auto profile = document.object();
            email_ = profile.value(QStringLiteral("email")).toString();
            if (email_.isEmpty())
                email_ = idTokenClaim(idToken_, QStringLiteral("email"));
            userId_ = profile.value(QStringLiteral("id")).toString();
            if (userId_.isEmpty())
                userId_ = profile.value(QStringLiteral("user_id")).toString();
            if (userId_.isEmpty())
                userId_ = idTokenClaim(idToken_, QStringLiteral("sub"));
            plan_ = profile.value(QStringLiteral("plan")).toString(QStringLiteral("free"));
            if (email_.isEmpty()) {
                publish(QStringLiteral("Signed in, but the Pixelion account email was unavailable."));
                return;
            }
            publish(QStringLiteral("Signed in to Pixelion."));
            refreshUsage();
        });
    });
}

void PixelionAuthService::signOut()
{
    ++usageRequestGeneration_;
    deleteRefreshToken();
    accessToken_.clear();
    idToken_.clear();
    email_.clear();
    userId_.clear();
    plan_.clear();
    accessTokenExpiresAt_ = 0;
    publishUsage(UsageInfo {});
    publish(QStringLiteral("Signed out of Pixelion."));
}
