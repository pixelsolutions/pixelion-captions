#include "live-session.h"
#include "auth-service.h"
#include "plugin-debug-log.h"

#include <obs.h>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QEventLoop>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QWebSocket>

#include <array>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

// macOS adapter for PixelionLiveSession.
//
// The Qt/OBS logic is ported directly from the Windows adapter
// (src/platform/windows/live-session.cpp) so observable behavior and request
// payloads match Windows byte-for-byte (payload parity is covered by Task 8.6).
// The split follows the design (Components and Interfaces 2):
//
//   Portable (ported verbatim here):
//     * Task 4.1 - POST /realtime-token + POST /realtime-token/usage-check,
//                  with the Windows JSON payloads unchanged; abort on failure,
//                  non-success, or 10 s timeout, surfacing the error via the
//                  state callback. The client secret is held in memory only.
//     * Task 4.2 - obs_add_raw_audio_callback (mix 0), 24 kHz / s16 / mono
//                  conversion, and the fixed-capacity AudioQueue ring buffer
//                  (kSlots=64, kBytesPerSlot=4096, mutex-guarded, drop-on-full,
//                  no extra allocation on overflow).
//
//   Native:
//     * Task 4.3 (implemented) - the WSS transport. Windows uses a WinHTTP
//                  WebSocket; macOS uses QWebSocket (Qt6::WebSockets) to
//                  wss://api.openai.com. The worker_ thread runs a QEventLoop
//                  that services the QWebSocket; a QTimer drains the AudioQueue
//                  and sends input_audio_buffer.append frames, and inbound text
//                  frames are fed to the ported parseProviderMessage(). The
//                  session.update config, append event shape, and Bearer auth
//                  header match the Windows adapter byte-for-byte (parity: 8.6).
//     * Task 4.4 - error-path finalization. The connect-failure / unexpected
//                  mid-session close -> stop + surface, and the teardown
//                  sequence (obs_remove_raw_audio_callback, mark queue closed,
//                  delete audio, running_=false, final checkUsage(true), timer
//                  teardown, final state message) are implemented inline in the
//                  worker, matching the Windows worker tail so the session is
//                  stopped and surfaced on every failure without leaking.
//
// finishWorker() is declared in the shared header but is never ODR-used: the
// ported worker performs its teardown inline (exactly as the Windows adapter
// does, which defines no finishWorker() body at all). It is given an empty
// out-of-line definition here so the declaration resolves on the macOS build.

#ifndef PIXELION_API_BASE_URL
#define PIXELION_API_BASE_URL "https://9yw5ciothk.execute-api.us-east-1.amazonaws.com/prod"
#endif

namespace {
    // GA transcription sessions use intent=transcription; the model is bound to the
    // ephemeral client secret created by Pixelion's token service. Paths kept as
    // QString here (vs. the Windows wide-string literals) for the Qt-native
    // QWebSocket transport that Task 4.3 will build against wss://api.openai.com.
    constexpr char kOpenAiWssBase[] = "wss://api.openai.com";
    constexpr char kTranscribePath[] = "/v1/realtime?intent=transcription";
    constexpr char kTranslatePath[] = "/v1/realtime/translations?model=gpt-realtime-translate";

    // --- Audio ring buffer (Task 4.2) -----------------------------------------
    //
    // Carried over unchanged from the Windows adapter. Fixed-capacity, mutex-guarded
    // SPSC ring; the writer drops frames when the next write index would collide
    // with read (bounded, no additional allocation on overflow). DWORD (Windows)
    // becomes uint32_t here; the semantics are identical.
    struct AudioQueue {
        static constexpr size_t kSlots = 64;
        static constexpr size_t kBytesPerSlot = 4096;
        std::array<std::array<uint8_t, kBytesPerSlot>, kSlots> bytes {};
        std::array<uint32_t, kSlots> sizes {};
        std::mutex mutex;
        size_t read = 0;
        size_t write = 0;
        bool closed = false;
    };

    void audioCallback(void *param, size_t, audio_data *data)
    {
        auto *queue = static_cast<AudioQueue *>(param);
        if (!queue || !data || !data->data[0] || data->frames == 0)
            return;
        const size_t byteCount = static_cast<size_t>(data->frames) * sizeof(int16_t);
        if (byteCount > AudioQueue::kBytesPerSlot || !queue->mutex.try_lock())
            return;
        if (!queue->closed) {
            const size_t next = (queue->write + 1) % AudioQueue::kSlots;
            if (next != queue->read) {
                std::memcpy(queue->bytes[queue->write].data(), data->data[0], byteCount);
                queue->sizes[queue->write] = static_cast<uint32_t>(byteCount);
                queue->write = next;
            }
        }
        queue->mutex.unlock();
    }

    bool popAudio(AudioQueue &queue, std::array<uint8_t, AudioQueue::kBytesPerSlot> &buffer, uint32_t &size)
    {
        std::lock_guard<std::mutex> lock(queue.mutex);
        if (queue.read == queue.write)
            return false;
        size = queue.sizes[queue.read];
        buffer = queue.bytes[queue.read];
        queue.read = (queue.read + 1) % AudioQueue::kSlots;
        return true;
    }

    // --- Shared transport helpers ---------------------------------------------
    //
    // Ported verbatim from the Windows adapter so the Task 4.3 QWebSocket transport
    // base64-encodes audio frames and parses provider messages identically to
    // Windows (caption deltas/finals and error handling match; see Task 8.6).
    QString base64(const uint8_t *data, size_t size)
    {
        return QString::fromLatin1(
            QByteArray(reinterpret_cast<const char *>(data), static_cast<qsizetype>(size)).toBase64());
    }

    // Serialize + send one JSON event as a WebSocket text frame. Mirrors the
    // Windows adapter's sendJson(): compact JSON, UTF-8 text message. QWebSocket
    // buffers the frame internally and reports write failures via its error
    // signal, so this returns whether the socket was in a connected/valid state at
    // send time (the receive/error signals drive the actual failure path).
    bool sendJson(QWebSocket &socket, const QJsonObject &message)
    {
        if (socket.state() != QAbstractSocket::ConnectedState)
            return false;
        const QByteArray json = QJsonDocument(message).toJson(QJsonDocument::Compact);
        socket.sendTextMessage(QString::fromUtf8(json));
        return true;
    }

    void parseProviderMessage(const QByteArray &message, const QString &mode,
                              const PixelionLiveSession::CaptionCallback &caption,
                              const PixelionLiveSession::StateCallback &state, std::atomic<bool> *gracefulDone,
                              QString *providerError, std::mutex *providerErrorMutex, std::atomic<bool> *providerFailed)
    {
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(message, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
            return;
        const auto object = document.object();
        const QString type = object.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("error")) {
            const auto error = object.value(QStringLiteral("error")).toObject();
            const QString code = error.value(QStringLiteral("code")).toString();
            const QString text = error.value(QStringLiteral("message")).toString();
            const QString safeMessage = text.isEmpty() ? QStringLiteral("Realtime processing returned an error.")
                                                       : text.left(240);
            PixelionDebugLog::write("ERROR", QStringLiteral("Realtime provider error%1: %2")
                                                 .arg(code.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(code),
                                                      safeMessage));
            if (providerError && providerErrorMutex) {
                std::lock_guard<std::mutex> lock(*providerErrorMutex);
                *providerError = code.isEmpty() ? safeMessage : QStringLiteral("%1 (%2)").arg(safeMessage, code);
            }
            if (providerFailed)
                *providerFailed = true;
            if (state)
                state(false, safeMessage);
            return;
        }
        if (type == QStringLiteral("session.updated")) {
            if (state)
                state(true, QStringLiteral("Connected. Listening to OBS program audio."));
            return;
        }
        if (mode == QStringLiteral("transcribe")) {
            if (type == QStringLiteral("conversation.item.input_audio_transcription.delta")) {
                const QString delta = object.value(QStringLiteral("delta")).toString();
                if (!delta.isEmpty() && caption)
                    caption(delta, false);
            } else if (type == QStringLiteral("conversation.item.input_audio_transcription.completed")) {
                const QString text = object.value(QStringLiteral("transcript")).toString();
                if (!text.isEmpty() && caption)
                    caption(text, true);
                if (gracefulDone)
                    *gracefulDone = true;
            }
        } else if (type == QStringLiteral("session.output_transcript.delta")) {
            const QString delta = object.value(QStringLiteral("delta")).toString();
            if (!delta.isEmpty() && caption)
                caption(delta, false);
        } else if (mode == QStringLiteral("translate") && type == QStringLiteral("session.closed")) {
            if (gracefulDone)
                *gracefulDone = true;
        }
    }

}  // namespace

PixelionLiveSession::PixelionLiveSession(PixelionAuthService *auth, QObject *parent) :
    QObject(parent), auth_(auth), network_(new QNetworkAccessManager(this))
{
    usageTimer_ = new QTimer(this);
    usageTimer_->setInterval(60000);
    connect(usageTimer_, &QTimer::timeout, this, [this] {
        checkUsage(false);
    });
    limitTestTimer_ = new QTimer(this);
    limitTestTimer_->setInterval(1000);
    connect(limitTestTimer_, &QTimer::timeout, this, [this] {
        if (!running_ || !sessionClock_.isValid() || durationLimitSeconds_ <= 0)
            return;
        const int elapsedSeconds = static_cast<int>(sessionClock_.elapsed() / 1000);
        const int secondsRemaining = durationLimitSeconds_ - elapsedSeconds;
        if (secondsRemaining > 0 && secondsRemaining <= 60) {
            durationWarningShown_ = true;
            if (stateCallback_)
                stateCallback_(true, QStringLiteral("Your session limit is approaching. Live captions will "
                                                    "stop in %1 seconds.")
                                         .arg(secondsRemaining));
        }
        if (secondsRemaining <= 0) {
            limitTestTimer_->stop();
            if (stateCallback_)
                stateCallback_(true, QStringLiteral("Your session limit has been reached."));
            stop();
        }
    });
}

PixelionLiveSession::~PixelionLiveSession()
{
    stop();
    if (worker_.joinable())
        worker_.join();
}

void PixelionLiveSession::setCallbacks(StateCallback state, CaptionCallback caption)
{
    stateCallback_ = std::move(state);
    captionCallback_ = std::move(caption);
}

bool PixelionLiveSession::isRunning() const
{
    return running_.load() || pending_.load();
}

void PixelionLiveSession::start(const QString &mode, const QString &sourceLanguage, const QString &targetLanguage)
{
    if (isRunning() || mode.isEmpty())
        return;
    PixelionDebugLog::write("INFO", QStringLiteral("Requesting realtime token for %1 session (source language: %2, "
                                                   "target language: %3)")
                                        .arg(mode, sourceLanguage, targetLanguage));
    mode_ = mode;
    sourceLanguage_ = sourceLanguage;
    sessionId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    elapsedSeconds_ = 0;
    stopRequested_ = false;
    pending_ = true;
    if (stateCallback_)
        stateCallback_(true, QStringLiteral("Checking your Pixelion account and available minutes…"));
    requestRealtimeToken(mode, sourceLanguage, targetLanguage, sessionId_);
}

// --- Task 4.1: realtime-token request --------------------------------------
//
// Ported verbatim from the Windows adapter: same payload (mode, client,
// client_version, job_id, source_language[, target_language]), same error
// mapping, same 15 s transfer timeout, and the client secret handed to
// beginWorker() in memory only (never persisted to disk).
void PixelionLiveSession::requestRealtimeToken(const QString &mode, const QString &sourceLanguage,
                                               const QString &targetLanguage, const QString &sessionId)
{
    auth_->withAccessToken([this, mode, sourceLanguage, targetLanguage, sessionId](bool ok, const QString &access,
                                                                                   const QString &error) {
        if (sessionId != sessionId_ || stopRequested_)
            return;
        if (!ok) {
            PixelionDebugLog::write("ERROR",
                                    QStringLiteral("Could not get access token for realtime session: %1").arg(error));
            pending_ = false;
            if (stateCallback_)
                stateCallback_(false, error.isEmpty() ? QStringLiteral("Sign in to Pixelion before starting a "
                                                                       "session.")
                                                      : error);
            return;
        }
        QJsonObject body {
            {           QStringLiteral("mode"),                         mode},
            {         QStringLiteral("client"), QStringLiteral("obs_plugin")},
            { QStringLiteral("client_version"),      QStringLiteral("0.1.0")},
            {         QStringLiteral("job_id"),                    sessionId},
            {QStringLiteral("source_language"),               sourceLanguage}
        };
        if (mode == QStringLiteral("translate"))
            body.insert(QStringLiteral("target_language"), targetLanguage);
        QNetworkRequest request(QUrl(QStringLiteral(PIXELION_API_BASE_URL) + QStringLiteral("/realtime-token")));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setRawHeader("Authorization", QByteArray("Bearer ") + access.toUtf8());
        request.setTransferTimeout(15000);
        auto *reply = network_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, mode, sourceLanguage, targetLanguage, sessionId] {
            const QByteArray payload = reply->readAll().left(1024 * 1024);
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString networkError = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
            reply->deleteLater();
            if (sessionId != sessionId_)
                return;
            if (stopRequested_) {
                pending_ = false;
                if (stateCallback_)
                    stateCallback_(false, QStringLiteral("Session start cancelled."));
                return;
            }
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(payload, &parseError);
            if (status != 200 || parseError.error != QJsonParseError::NoError || !document.isObject()) {
                const auto error = document.object().value(QStringLiteral("error")).toString();
                PixelionDebugLog::write(
                    "ERROR",
                    QStringLiteral("Realtime token request failed (HTTP %1%2): %3")
                        .arg(status)
                        .arg(networkError.isEmpty() ? QString() : QStringLiteral(", %1").arg(networkError), error));
                QString message = error;
                if (status == 0 && !networkError.isEmpty())
                    message = QStringLiteral("Could not reach Pixelion. Live transcription and translation "
                                             "need an internet connection. Check your connection, then try "
                                             "Start again.");
                else if (message == QStringLiteral("PRO_REQUIRED") ||
                         message.compare(QStringLiteral("Translation requires a Pro plan"), Qt::CaseInsensitive) == 0)
                    message = QStringLiteral("Live translation requires Pixelion Pro.");
                else if (message == QStringLiteral("MINUTES_LIMIT") || message == QStringLiteral("TRIAL_LIMIT"))
                    message = QStringLiteral("Your Pixelion minutes are used up for this period.");
                else if (message == QStringLiteral("DURATION_LIMIT"))
                    message = QStringLiteral("This session exceeds your plan's session limit.");
                else if (message.isEmpty())
                    message = QStringLiteral("Pixelion could not start this session. Check your connection "
                                             "and account.");
                pending_ = false;
                if (stateCallback_)
                    stateCallback_(false, message);
                return;
            }
            const QString secret = document.object().value(QStringLiteral("client_secret")).toString();
            if (secret.isEmpty()) {
                PixelionDebugLog::write("ERROR", QStringLiteral("Pixelion realtime token response did "
                                                                "not include a client secret"));
                pending_ = false;
                if (stateCallback_)
                    stateCallback_(false, QStringLiteral("Pixelion returned an incomplete "
                                                         "realtime session token."));
                return;
            }
            beginWorker(mode, sourceLanguage, targetLanguage, sessionId, secret);
        });
    });
}

void PixelionLiveSession::beginWorker(const QString &mode, const QString &sourceLanguage, const QString &targetLanguage,
                                      const QString &sessionId, const QString &clientSecret)
{
    if (worker_.joinable())
        worker_.join();
    pending_ = false;
    // Task 4.2: the OBS program-mix conversion target (24 kHz / s16 / mono).
    // The AudioQueue is owned by the worker and fed by audioCallback; the
    // callback is registered in the worker once the WSS session is configured
    // (Task 4.3) and removed on teardown (Task 4.4), exactly as on Windows.
    auto *audio = new AudioQueue;
    audio_convert_info conversion = {};
    conversion.samples_per_sec = 24000;
    conversion.format = AUDIO_FORMAT_16BIT;
    conversion.speakers = SPEAKERS_MONO;
    running_ = true;
    pending_ = false;
    reportedSeconds_ = 0;
    durationLimitSeconds_ = 0;
    durationWarningShown_ = false;
    sessionClock_.start();
    usageTimer_->start();
    limitTestTimer_->start();
    if (stateCallback_)
        stateCallback_(true, QStringLiteral("Connecting to Pixelion live %1…").arg(mode));
    PixelionDebugLog::write("INFO", QStringLiteral("Opening realtime WebSocket for %1 session").arg(mode));

    // Marshal provider callbacks back onto the object's thread, matching the
    // Windows adapter. Task 4.3's QWebSocket transport uses these when wiring
    // parseProviderMessage.
    const auto rawStateCb = stateCallback_;
    const auto rawCaptionCb = captionCallback_;
    const auto stateCb = [this, rawStateCb](bool running, const QString &message) {
        QMetaObject::invokeMethod(
            this, [rawStateCb, running, message] {
                if (rawStateCb)
                    rawStateCb(running, message);
            }, Qt::QueuedConnection);
    };
    const auto captionCb = [this, rawCaptionCb](const QString &text, bool final) {
        QMetaObject::invokeMethod(
            this, [rawCaptionCb, text, final] {
                if (rawCaptionCb)
                    rawCaptionCb(text, final);
            }, Qt::QueuedConnection);
    };

    // ===================================================================
    // Task 4.3: QWebSocket (Qt6::WebSockets) WSS transport.
    // -------------------------------------------------------------------
    // Mirrors the Windows WinHTTP WebSocket worker (src/platform/windows/
    // live-session.cpp). The Windows adapter ran a dedicated std::thread with
    // a blocking send loop plus a receive thread; QWebSocket is signal-driven
    // and requires an event loop, so the worker_ thread runs a QEventLoop and
    // the QWebSocket lives on and is serviced by that thread. A QTimer on the
    // same thread drains the AudioQueue and sends input_audio_buffer.append
    // frames, replacing the Windows poll/Sleep(10) send loop; inbound text
    // frames are fed to the ported parseProviderMessage() exactly as Windows
    // feeds its receive thread. Payloads (session.update shape, append event
    // type/shape, auth header) are byte-identical to Windows (parity: 8.6).
    //
    // Error-path finalization (Task 4.4): connect failure / unexpected close ->
    // stop and surface via stateCb; final checkUsage + timer teardown + final
    // state message. Implemented in the worker's teardown/connectFailure lambdas
    // below, mirroring the Windows worker tail so every failure is surfaced and
    // nothing leaks.
    worker_ = std::thread([this, audio, conversion, mode, sourceLanguage, targetLanguage, sessionId, clientSecret,
                           stateCb, captionCb]() mutable {
        (void) sessionId;
        QEventLoop loop;
        QWebSocket socket(QString(), QWebSocketProtocol::VersionLatest);

        // Shared teardown, mirroring the Windows worker's tail. Runs exactly
        // once; `audioAdded` tracks whether the OBS callback was registered so
        // we only remove what we added. `failed` selects the interrupted vs.
        // clean final message, and a provider error (captured by
        // parseProviderMessage) overrides it, matching Windows.
        std::atomic<bool> gracefulDone {false};
        std::atomic<bool> providerFailed {false};
        QString providerError;
        std::mutex providerErrorMutex;
        bool audioAdded = false;
        bool teardownDone = false;
        bool failed = false;

        auto teardown = [&](const QString &logInfo) {
            if (teardownDone)
                return;
            teardownDone = true;
            if (socket.state() != QAbstractSocket::UnconnectedState) {
                socket.abort();
            }
            if (audioAdded) {
                obs_remove_raw_audio_callback(0, audioCallback, audio);
                audioAdded = false;
            }
            {
                std::lock_guard<std::mutex> lock(audio->mutex);
                audio->closed = true;
            }
            delete audio;
            running_ = false;
            if (!logInfo.isEmpty())
                PixelionDebugLog::write("INFO", logInfo);
            // Task 4.4: final usage check + timer teardown sequence,
            // mirroring the Windows worker tail so the session does not leak.
            if (!stopRequested_)
                QMetaObject::invokeMethod(
                    this, [this] {
                        checkUsage(true);
                    }, Qt::QueuedConnection);
            QMetaObject::invokeMethod(
                this, [this] {
                    usageTimer_->stop();
                    limitTestTimer_->stop();
                }, Qt::QueuedConnection);
            QString finalMessage = failed ? QStringLiteral("Live captions stopped because the connection was "
                                                           "interrupted. Check your internet connection, then try "
                                                           "Start again. Text already received is kept; audio during "
                                                           "the interruption cannot be transcribed.")
                                          : QStringLiteral("Session ended.");
            {
                std::lock_guard<std::mutex> lock(providerErrorMutex);
                if (!providerError.isEmpty())
                    finalMessage = QStringLiteral("Realtime service error: %1").arg(providerError);
            }
            QMetaObject::invokeMethod(
                this, [this, stateCb, finalMessage] {
                    stateCb(false, finalMessage);
                }, Qt::QueuedConnection);
            loop.quit();
        };

        // Connect-failure / unexpected-close path. Mirrors the Windows
        // "could not connect" early-out: stop the session, surface the error
        // via stateCb, and stop usageTimer_. Distinguished from a graceful
        // close so a clean session ending does not report as a failure.
        auto connectFailure = [&](const QString &userMessage, const QString &logMessage) {
            if (teardownDone)
                return;
            failed = true;
            if (!logMessage.isEmpty())
                PixelionDebugLog::write("ERROR", logMessage);
            if (audioAdded) {
                obs_remove_raw_audio_callback(0, audioCallback, audio);
                audioAdded = false;
            }
            {
                std::lock_guard<std::mutex> lock(audio->mutex);
                audio->closed = true;
            }
            if (socket.state() != QAbstractSocket::UnconnectedState)
                socket.abort();
            delete audio;
            teardownDone = true;
            running_ = false;
            // Task 4.4: connect-failure / unexpected-close teardown. Mirrors
            // the Windows "could not connect" early-out: stop usage timers,
            // surface the error via stateCb, and exit the worker loop.
            QMetaObject::invokeMethod(
                this, [this, stateCb, userMessage] {
                    usageTimer_->stop();
                    limitTestTimer_->stop();
                    stateCb(false, userMessage);
                }, Qt::QueuedConnection);
            loop.quit();
        };

        // --- connected: send session.update, register OBS audio, start drain
        QObject::connect(&socket, &QWebSocket::connected, &socket, [&]() {
            PixelionDebugLog::write("INFO", QStringLiteral("Realtime WebSocket connected"));
            QJsonObject sessionConfig;
            if (mode == QStringLiteral("translate")) {
                sessionConfig = {
                    {   QStringLiteral("type"),QStringLiteral("session.update")                            },
                    {QStringLiteral("session"),
                     QJsonObject {
                     {QStringLiteral("audio"),
                     QJsonObject {{QStringLiteral("input"),
                     QJsonObject {{QStringLiteral("transcription"),
                     QJsonObject {{QStringLiteral("model"),
                     QStringLiteral("gpt-4o-mini-transcribe")}}}}},
                     {QStringLiteral("output"),
                     QJsonObject {{QStringLiteral("language"), targetLanguage}}}}}}}
                };
            } else {
                QJsonObject transcription {
                    {QStringLiteral("model"), QStringLiteral("gpt-live-transcribe")},
                    {QStringLiteral("delay"),                 QStringLiteral("low")}
                };
                if (!sourceLanguage.isEmpty() && sourceLanguage != QStringLiteral("auto"))
                    transcription.insert(QStringLiteral("languages"), QJsonArray {sourceLanguage});
                sessionConfig = {
                    {   QStringLiteral("type"),QStringLiteral("session.update")               },
                    {QStringLiteral("session"),
                     QJsonObject {
                     {QStringLiteral("type"), QStringLiteral("transcription")},
                     {QStringLiteral("audio"),
                     QJsonObject {{QStringLiteral("input"),
                     QJsonObject {{QStringLiteral("format"),
                     QJsonObject {{QStringLiteral("type"), QStringLiteral("audio/pcm")},
                     {QStringLiteral("rate"), 24000}}},
                     {QStringLiteral("transcription"), transcription},
                     {QStringLiteral("turn_detection"), QJsonValue::Null}}}}}}}
                };
            }
            if (!sendJson(socket, sessionConfig)) {
                connectFailure(QStringLiteral("Could not start live captions because the connection was "
                                              "interrupted. Check your internet connection, then try Start "
                                              "again."),
                               QStringLiteral("Could not configure realtime session"));
                return;
            }
            obs_add_raw_audio_callback(0, &conversion, audioCallback, audio);
            audioAdded = true;
        });

        // --- inbound text frames -> parseProviderMessage (identical to Windows)
        QObject::connect(&socket, &QWebSocket::textMessageReceived, &socket, [&](const QString &message) {
            const QByteArray payload = message.toUtf8();
            parseProviderMessage(payload, mode, captionCb, stateCb, &gracefulDone, &providerError, &providerErrorMutex,
                                 &providerFailed);
            if (providerFailed) {
                failed = true;
                teardown(QString());
            }
        });

        // --- transport error -> connect/unexpected-close failure path
        QObject::connect(&socket, &QWebSocket::errorOccurred, &socket, [&](QAbstractSocket::SocketError) {
            if (teardownDone)
                return;
            connectFailure(QStringLiteral("Could not connect to live captions. Check your internet "
                                          "connection, then try Start again. Live transcription and "
                                          "translation require internet access."),
                           QStringLiteral("Realtime WebSocket error: %1").arg(socket.errorString()));
        });

        // --- unexpected close mid-session -> failure; graceful close -> clean
        QObject::connect(&socket, &QWebSocket::disconnected, &socket, [&]() {
            if (teardownDone)
                return;
            if (gracefulDone || stopRequested_) {
                teardown(QStringLiteral("Realtime session stopped cleanly"));
            } else {
                failed = true;
                teardown(QStringLiteral("Realtime WebSocket closed unexpectedly"));
            }
        });

        // --- audio drain timer (replaces the Windows poll/Sleep(10) loop) ---
        std::array<uint8_t, AudioQueue::kBytesPerSlot> audioBuffer {};
        QTimer drainTimer;
        drainTimer.setInterval(10);
        QObject::connect(&drainTimer, &QTimer::timeout, &socket, [&]() {
            if (teardownDone)
                return;
            if (stopRequested_ || providerFailed) {
                // Graceful stop, matching the Windows worker: commit
                // (transcribe) or session.close (translate), then let the
                // provider's completion/close drive teardown. A short grace
                // period is enforced by the fallback timer below.
                drainTimer.stop();
                if (socket.state() == QAbstractSocket::ConnectedState) {
                    if (mode == QStringLiteral("transcribe"))
                        sendJson(socket, QJsonObject {
                                             {QStringLiteral("type"), QStringLiteral("input_audio_buffer.commit")}
                        });
                    else
                        sendJson(socket, QJsonObject {
                                             {QStringLiteral("type"), QStringLiteral("session.close")}
                        });
                }
                // Task 4.4: match the Windows worker's 2.5 s drain window.
                // A single-shot fallback guarantees teardown even if no
                // graceful close/completion arrives within the window.
                QTimer::singleShot(2500, &socket, [&]() {
                    if (!teardownDone) {
                        if (!gracefulDone && !stopRequested_)
                            failed = true;
                        teardown(QStringLiteral("Realtime session stopped"));
                    }
                });
                return;
            }
            uint32_t audioSize = 0;
            while (popAudio(*audio, audioBuffer, audioSize) && audioSize > 0) {
                const QString eventType = mode == QStringLiteral("translate")
                                              ? QStringLiteral("session.input_audio_buffer.append")
                                              : QStringLiteral("input_audio_buffer.append");
                if (!sendJson(socket, QJsonObject {
                                          {QStringLiteral("type"), eventType},
                                          {QStringLiteral("audio"), base64(audioBuffer.data(), audioSize)}
                })) {
                    connectFailure(QStringLiteral("Live captions stopped because the connection was "
                                                  "interrupted. Check your internet connection, then try "
                                                  "Start again."),
                                   QStringLiteral("Realtime WebSocket audio send failed"));
                    return;
                }
                audioSize = 0;
            }
        });
        drainTimer.start();

        // --- open the WSS connection with the Bearer auth header ------------
        // Pixelion returns GA client secrets. The retired Realtime Beta header
        // (OpenAI-Beta: realtime=v1) makes OpenAI reject the first
        // session.update with beta_api_shape_disabled, so the Windows adapter
        // sends only Authorization; we match it and send no OpenAI-Beta header.
        // The client secret is held in memory only; never written to disk.
        const QString path = mode == QStringLiteral("translate") ? QString::fromLatin1(kTranslatePath)
                                                                 : QString::fromLatin1(kTranscribePath);
        QNetworkRequest request(QUrl(QString::fromLatin1(kOpenAiWssBase) + path));
        request.setRawHeader("Authorization", QByteArray("Bearer ") + clientSecret.toUtf8());
        socket.open(request);

        loop.exec();
    });
}

// finishWorker() is declared in the shared header but not ODR-used: the worker
// performs OBS callback removal, queue close, final usage check, timer teardown,
// and the final state message inline (see beginWorker's teardown/connectFailure
// lambdas), matching the Windows adapter which keeps the same steps inline. This
// empty definition exists only so the header declaration resolves on macOS.
void PixelionLiveSession::finishWorker() {}

void PixelionLiveSession::stop()
{
    if (pending_) {
        stopRequested_ = true;
        pending_ = false;
        if (stateCallback_)
            stateCallback_(false, QStringLiteral("Session start cancelled."));
        return;
    }
    if (!running_)
        return;
    stopRequested_ = true;
    if (limitTestTimer_)
        limitTestTimer_->stop();
    checkUsage(true);
}

// --- Task 4.1: usage-check request -----------------------------------------
//
// Ported verbatim from the Windows adapter: same payload (elapsed_seconds,
// session_id/job_id, from_second/to_second, final, client, client_version,
// mode, source_language), same 10 s transfer timeout, and the same
// duration-limit / remaining-minutes handling that drives stop().
void PixelionLiveSession::checkUsage(bool final)
{
    if (!auth_)
        return;
    if (sessionId_.isEmpty() || !sessionClock_.isValid()) {
        if (final)
            auth_->refreshUsage();
        return;
    }
    const int toSecond = static_cast<int>(sessionClock_.elapsed() / 1000);
    const int fromSecond = reportedSeconds_;
    if (toSecond <= fromSecond) {
        if (final)
            auth_->refreshUsage();
        return;
    }
    const QString sessionId = sessionId_;
    const QString mode = mode_;
    const QString sourceLanguage = sourceLanguage_;
    auth_->withAccessToken([this, sessionId, mode, sourceLanguage, final, fromSecond,
                            toSecond](bool ok, const QString &access, const QString &) {
        if (!ok) {
            if (final)
                auth_->refreshUsage();
            return;
        }
        QJsonObject body {
            {QStringLiteral("elapsed_seconds"),        toSecond - fromSecond},
            {     QStringLiteral("session_id"),                    sessionId},
            {         QStringLiteral("job_id"),                    sessionId},
            {    QStringLiteral("from_second"),                   fromSecond},
            {      QStringLiteral("to_second"),                     toSecond},
            {          QStringLiteral("final"),                        final},
            {         QStringLiteral("client"), QStringLiteral("obs_plugin")},
            { QStringLiteral("client_version"),      QStringLiteral("0.1.0")},
            {           QStringLiteral("mode"),                         mode},
            {QStringLiteral("source_language"),               sourceLanguage}
        };
        QNetworkRequest request(
            QUrl(QStringLiteral(PIXELION_API_BASE_URL) + QStringLiteral("/realtime-token/usage-check")));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setRawHeader("Authorization", QByteArray("Bearer ") + access.toUtf8());
        request.setTransferTimeout(10000);
        auto *reply = network_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, toSecond, final] {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray payload = reply->readAll().left(256 * 1024);
            reply->deleteLater();
            if (status != 200) {
                if (final)
                    auth_->refreshUsage();
                return;
            }
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(payload, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                if (final)
                    auth_->refreshUsage();
                return;
            }
            reportedSeconds_ = std::max(reportedSeconds_, toSecond);
            const auto result = document.object();
            const int durationLimit = result.value(QStringLiteral("duration_limit_seconds")).toInt();
            if (durationLimit > 0)
                durationLimitSeconds_ = durationLimit;
            const auto remaining = result.value(QStringLiteral("remaining_minutes"));
            if (remaining.isDouble())
                auth_->updateUsageRemainingMinutes(static_cast<int>(remaining.toDouble()));
            if (final)
                auth_->refreshUsage();
            if (!result.value(QStringLiteral("allowed")).toBool(true) ||
                (durationLimitSeconds_ > 0 && toSecond >= durationLimitSeconds_)) {
                const QString code = result.value(QStringLiteral("error")).toString();
                if (stateCallback_)
                    stateCallback_(true, code == QStringLiteral("DURATION_LIMIT") || durationLimitSeconds_ > 0
                                             ? QStringLiteral("Your session limit has been "
                                                              "reached.")
                                             : QStringLiteral("Your available Pixelion minutes "
                                                              "are used up."));
                stop();
            }
        });
    });
}
