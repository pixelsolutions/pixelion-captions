#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include "live-session.h"
#include "auth-service.h"
#include "plugin-debug-log.h"

#include <obs.h>

#include <QDateTime>
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include <array>
#include <algorithm>
#include <cstring>
#include <mutex>

#ifndef PIXELION_API_BASE_URL
#define PIXELION_API_BASE_URL "https://9yw5ciothk.execute-api.us-east-1.amazonaws.com/prod"
#endif

namespace {
constexpr wchar_t kOpenAiHost[] = L"api.openai.com";
// GA transcription sessions use intent=transcription; the model is bound to
// the ephemeral client secret created by Pixelion's token service.
constexpr wchar_t kTranscribePath[] = L"/v1/realtime?intent=transcription";
constexpr wchar_t kTranslatePath[] = L"/v1/realtime/translations?model=gpt-realtime-translate";
constexpr int kClientVersionMajor = 0;

struct AudioQueue {
	static constexpr size_t kSlots = 64;
	static constexpr size_t kBytesPerSlot = 4096;
	std::array<std::array<uint8_t, kBytesPerSlot>, kSlots> bytes{};
	std::array<DWORD, kSlots> sizes{};
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
			queue->sizes[queue->write] = static_cast<DWORD>(byteCount);
			queue->write = next;
		}
	}
	queue->mutex.unlock();
}

bool popAudio(AudioQueue &queue, std::array<uint8_t, AudioQueue::kBytesPerSlot> &buffer, DWORD &size)
{
	std::lock_guard<std::mutex> lock(queue.mutex);
	if (queue.read == queue.write)
		return false;
	size = queue.sizes[queue.read];
	buffer = queue.bytes[queue.read];
	queue.read = (queue.read + 1) % AudioQueue::kSlots;
	return true;
}

QString base64(const uint8_t *data, size_t size)
{
	return QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(data), static_cast<qsizetype>(size)).toBase64());
}

bool sendJson(HINTERNET socket, const QJsonObject &message)
{
	const QByteArray json = QJsonDocument(message).toJson(QJsonDocument::Compact);
	return WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
				    const_cast<char *>(json.constData()), static_cast<DWORD>(json.size())) == NO_ERROR;
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
		const QString safeMessage = text.isEmpty() ? QStringLiteral("Realtime processing returned an error.") : text.left(240);
		PixelionDebugLog::write("ERROR", QStringLiteral("Realtime provider error%1: %2")
							.arg(code.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(code), safeMessage));
		if (providerError && providerErrorMutex) {
			std::lock_guard<std::mutex> lock(*providerErrorMutex);
			*providerError = code.isEmpty() ? safeMessage : QStringLiteral("%1 (%2)").arg(safeMessage, code);
		}
		if (providerFailed) *providerFailed = true;
		if (state)
			state(false, safeMessage);
		return;
	}
	if (type == QStringLiteral("session.updated")) {
		if (state) state(true, QStringLiteral("Connected. Listening to OBS program audio."));
		return;
	}
	if (mode == QStringLiteral("transcribe")) {
		if (type == QStringLiteral("conversation.item.input_audio_transcription.delta")) {
			const QString delta = object.value(QStringLiteral("delta")).toString();
			if (!delta.isEmpty() && caption) caption(delta, false);
		} else if (type == QStringLiteral("conversation.item.input_audio_transcription.completed")) {
			const QString text = object.value(QStringLiteral("transcript")).toString();
			if (!text.isEmpty() && caption) caption(text, true);
			if (gracefulDone) *gracefulDone = true;
		}
	} else if (type == QStringLiteral("session.output_transcript.delta")) {
		const QString delta = object.value(QStringLiteral("delta")).toString();
		if (!delta.isEmpty() && caption) caption(delta, false);
	} else if (mode == QStringLiteral("translate") && type == QStringLiteral("session.closed")) {
		if (gracefulDone) *gracefulDone = true;
	}
}

} // namespace

PixelionLiveSession::PixelionLiveSession(PixelionAuthService *auth, QObject *parent)
	: QObject(parent), auth_(auth), network_(new QNetworkAccessManager(this))
{
	usageTimer_ = new QTimer(this);
	usageTimer_->setInterval(60000);
	connect(usageTimer_, &QTimer::timeout, this, [this] { checkUsage(false); });
	limitTestTimer_ = new QTimer(this);
	limitTestTimer_->setInterval(1000);
	connect(limitTestTimer_, &QTimer::timeout, this, [this] {
		if (!running_ || !sessionClock_.isValid() || durationLimitSeconds_ <= 0) return;
		const int elapsedSeconds = static_cast<int>(sessionClock_.elapsed() / 1000);
		const int secondsRemaining = durationLimitSeconds_ - elapsedSeconds;
		if (secondsRemaining > 0 && secondsRemaining <= 60) {
			durationWarningShown_ = true;
			if (stateCallback_) stateCallback_(true, QStringLiteral("Your session limit is approaching. Live captions will stop in %1 seconds.").arg(secondsRemaining));
		}
		if (secondsRemaining <= 0) {
			limitTestTimer_->stop();
			if (stateCallback_) stateCallback_(true, QStringLiteral("Your session limit has been reached."));
			stop();
		}
	});
}

PixelionLiveSession::~PixelionLiveSession()
{
	stop();
	if (worker_.joinable()) worker_.join();
}

void PixelionLiveSession::setCallbacks(StateCallback state, CaptionCallback caption)
{
	stateCallback_ = std::move(state);
	captionCallback_ = std::move(caption);
}

bool PixelionLiveSession::isRunning() const { return running_.load() || pending_.load(); }

void PixelionLiveSession::start(const QString &mode, const QString &sourceLanguage, const QString &targetLanguage)
{
	if (isRunning() || mode.isEmpty()) return;
	PixelionDebugLog::write("INFO", QStringLiteral("Requesting realtime token for %1 session (source language: %2, target language: %3)")
					.arg(mode, sourceLanguage, targetLanguage));
	mode_ = mode;
	sourceLanguage_ = sourceLanguage;
	sessionId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
	elapsedSeconds_ = 0;
	stopRequested_ = false;
	pending_ = true;
	if (stateCallback_) stateCallback_(true, QStringLiteral("Checking your Pixelion account and available minutes…"));
	requestRealtimeToken(mode, sourceLanguage, targetLanguage, sessionId_);
}

void PixelionLiveSession::requestRealtimeToken(const QString &mode, const QString &sourceLanguage,
							      const QString &targetLanguage, const QString &sessionId)
{
	auth_->withAccessToken([this, mode, sourceLanguage, targetLanguage, sessionId](bool ok, const QString &access, const QString &error) {
		if (sessionId != sessionId_ || stopRequested_) return;
		if (!ok) {
			PixelionDebugLog::write("ERROR", QStringLiteral("Could not get access token for realtime session: %1").arg(error));
			pending_ = false;
			if (stateCallback_) stateCallback_(false, error.isEmpty() ? QStringLiteral("Sign in to Pixelion before starting a session.") : error);
			return;
		}
		QJsonObject body{{QStringLiteral("mode"), mode}, {QStringLiteral("client"), QStringLiteral("obs_plugin")},
				 {QStringLiteral("client_version"), QStringLiteral("0.1.0")},
				 {QStringLiteral("job_id"), sessionId}, {QStringLiteral("source_language"), sourceLanguage}};
		if (mode == QStringLiteral("translate")) body.insert(QStringLiteral("target_language"), targetLanguage);
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
			if (sessionId != sessionId_) return;
			if (stopRequested_) { pending_ = false; if (stateCallback_) stateCallback_(false, QStringLiteral("Session start cancelled.")); return; }
			QJsonParseError parseError;
			const auto document = QJsonDocument::fromJson(payload, &parseError);
			if (status != 200 || parseError.error != QJsonParseError::NoError || !document.isObject()) {
				const auto error = document.object().value(QStringLiteral("error")).toString();
				PixelionDebugLog::write("ERROR", QStringLiteral("Realtime token request failed (HTTP %1%2): %3")
								.arg(status).arg(networkError.isEmpty() ? QString() : QStringLiteral(", %1").arg(networkError), error));
				QString message = error;
				if (status == 0 && !networkError.isEmpty()) message = QStringLiteral("Could not reach Pixelion. Live transcription and translation need an internet connection. Check your connection, then try Start again.");
				else if (message == QStringLiteral("PRO_REQUIRED") ||
					 message.compare(QStringLiteral("Translation requires a Pro plan"), Qt::CaseInsensitive) == 0)
					message = QStringLiteral("Live translation requires Pixelion Pro.");
				else if (message == QStringLiteral("MINUTES_LIMIT") || message == QStringLiteral("TRIAL_LIMIT")) message = QStringLiteral("Your Pixelion minutes are used up for this period.");
				else if (message == QStringLiteral("DURATION_LIMIT")) message = QStringLiteral("This session exceeds your plan's session limit.");
				else if (message.isEmpty()) message = QStringLiteral("Pixelion could not start this session. Check your connection and account.");
				pending_ = false;
				if (stateCallback_) stateCallback_(false, message);
				return;
			}
			const QString secret = document.object().value(QStringLiteral("client_secret")).toString();
			if (secret.isEmpty()) {
				PixelionDebugLog::write("ERROR", QStringLiteral("Pixelion realtime token response did not include a client secret"));
				pending_ = false;
				if (stateCallback_) stateCallback_(false, QStringLiteral("Pixelion returned an incomplete realtime session token."));
				return;
			}
			beginWorker(mode, sourceLanguage, targetLanguage, sessionId, secret);
		});
	});
}


void PixelionLiveSession::beginWorker(const QString &mode, const QString &sourceLanguage, const QString &targetLanguage,
					      const QString &sessionId, const QString &clientSecret)
{
	if (worker_.joinable()) worker_.join();
	pending_ = false;
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
	if (stateCallback_) stateCallback_(true, QStringLiteral("Connecting to Pixelion live %1…").arg(mode));
		PixelionDebugLog::write("INFO", QStringLiteral("Opening realtime WebSocket for %1 session").arg(mode));
	const auto rawStateCb = stateCallback_;
	const auto rawCaptionCb = captionCallback_;
	const auto stateCb = [this, rawStateCb](bool running, const QString &message) {
		QMetaObject::invokeMethod(this, [rawStateCb, running, message] { if (rawStateCb) rawStateCb(running, message); }, Qt::QueuedConnection);
	};
	const auto captionCb = [this, rawCaptionCb](const QString &text, bool final) {
		QMetaObject::invokeMethod(this, [rawCaptionCb, text, final] { if (rawCaptionCb) rawCaptionCb(text, final); }, Qt::QueuedConnection);
	};
	worker_ = std::thread([this, audio, conversion, mode, sourceLanguage, targetLanguage, sessionId, clientSecret, stateCb, captionCb]() mutable {
		HINTERNET session = WinHttpOpen(L"Pixelion OBS/0.1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
						WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (session) WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);
		HINTERNET connection = session ? WinHttpConnect(session, kOpenAiHost, INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
		const wchar_t *path = mode == QStringLiteral("translate") ? kTranslatePath : kTranscribePath;
		HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path, nullptr,
										 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
										 WINHTTP_FLAG_SECURE) : nullptr;
		if (request) WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
		DWORD receiveTimeoutMs = 100;
		if (request) WinHttpSetOption(request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &receiveTimeoutMs, sizeof(receiveTimeoutMs));
		// Pixelion returns GA client secrets. The retired Realtime Beta header
		// makes OpenAI reject the first session.update with beta_api_shape_disabled.
		const QString authHeader = QStringLiteral("Authorization: Bearer %1\r\n").arg(clientSecret);
		const auto authWide = authHeader.toStdWString();
		DWORD transportError = ERROR_SUCCESS;
		DWORD httpStatus = 0;
		bool connected = request != nullptr;
		if (!connected) transportError = GetLastError();
		if (connected && !WinHttpAddRequestHeaders(request, authWide.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD)) {
			connected = false; transportError = GetLastError();
		}
		if (connected && !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
			connected = false; transportError = GetLastError();
		}
		if (connected && !WinHttpReceiveResponse(request, nullptr)) {
			connected = false; transportError = GetLastError();
		}
		if (connected) {
			DWORD statusSize = sizeof(httpStatus);
			WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
					    WINHTTP_HEADER_NAME_BY_INDEX, &httpStatus, &statusSize, WINHTTP_NO_HEADER_INDEX);
		}
		HINTERNET socket = connected ? WinHttpWebSocketCompleteUpgrade(request, 0) : nullptr;
		if (connected && !socket) transportError = GetLastError();
		if (request) { WinHttpCloseHandle(request); request = nullptr; }
		if (!socket) connected = false;
		if (!connected) {
			if (connection) WinHttpCloseHandle(connection);
			if (session) WinHttpCloseHandle(session);
			delete audio;
			running_ = false;
			QString failure;
			if (httpStatus && httpStatus != HTTP_STATUS_SWITCH_PROTOCOLS)
				failure = QStringLiteral("Realtime handshake rejected by OpenAI (HTTP %1). Check the OBS log for details.").arg(httpStatus);
			else
				failure = QStringLiteral("Could not connect to live captions. Check your internet connection, then try Start again. Live transcription and translation require internet access.");
			blog(LOG_WARNING, "Pixelion Live Captions: realtime WebSocket handshake failed (HTTP %lu, WinHTTP %lu)", httpStatus, transportError);
			PixelionDebugLog::write("ERROR", QStringLiteral("Realtime WebSocket handshake failed (HTTP %1, WinHTTP %2)").arg(httpStatus).arg(transportError));
			QMetaObject::invokeMethod(this, [this, stateCb, failure] {
				usageTimer_->stop();
				stateCb(false, failure);
			}, Qt::QueuedConnection);
			return;
		}
		PixelionDebugLog::write("INFO", QStringLiteral("Realtime WebSocket connected"));
		DWORD keepAliveMs = 15000;
		WinHttpSetOption(socket, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL, &keepAliveMs, sizeof(keepAliveMs));
		QJsonObject sessionConfig;
		if (mode == QStringLiteral("translate")) {
			sessionConfig = {{QStringLiteral("type"), QStringLiteral("session.update")},
					 {QStringLiteral("session"), QJsonObject{
						 {QStringLiteral("audio"), QJsonObject{
							 {QStringLiteral("input"), QJsonObject{{QStringLiteral("transcription"), QJsonObject{{QStringLiteral("model"), QStringLiteral("gpt-4o-mini-transcribe")}}}}},
						 {QStringLiteral("output"), QJsonObject{{QStringLiteral("language"), targetLanguage}}}}}}}};
		} else {
			QJsonObject transcription{{QStringLiteral("model"), QStringLiteral("gpt-live-transcribe")}, {QStringLiteral("delay"), QStringLiteral("low")}};
			if (!sourceLanguage.isEmpty() && sourceLanguage != QStringLiteral("auto")) transcription.insert(QStringLiteral("languages"), QJsonArray{sourceLanguage});
			sessionConfig = {{QStringLiteral("type"), QStringLiteral("session.update")},
					 {QStringLiteral("session"), QJsonObject{{QStringLiteral("type"), QStringLiteral("transcription")},
						 {QStringLiteral("audio"), QJsonObject{{QStringLiteral("input"), QJsonObject{
							 {QStringLiteral("format"), QJsonObject{{QStringLiteral("type"), QStringLiteral("audio/pcm")}, {QStringLiteral("rate"), 24000}}},
							 {QStringLiteral("transcription"), transcription}, {QStringLiteral("turn_detection"), QJsonValue::Null}}}}}}}};
		}
		if (!sendJson(socket, sessionConfig)) {
			const DWORD sendError = GetLastError();
			blog(LOG_WARNING, "Pixelion Live Captions: could not configure realtime session (WinHTTP %lu)", sendError);
			PixelionDebugLog::write("ERROR", QStringLiteral("Could not configure realtime session (WinHTTP %1)").arg(sendError));
			WinHttpWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
			WinHttpCloseHandle(socket);
			WinHttpCloseHandle(connection);
			WinHttpCloseHandle(session);
			delete audio;
			running_ = false;
			QMetaObject::invokeMethod(this, [this, stateCb, sendError] {
				usageTimer_->stop();
				stateCb(false, QStringLiteral("Could not start live captions because the connection was interrupted. Check your internet connection, then try Start again."));
			}, Qt::QueuedConnection);
			return;
		}
		obs_add_raw_audio_callback(0, &conversion, audioCallback, audio);
		std::array<uint8_t, AudioQueue::kBytesPerSlot> audioBuffer{};
		std::array<char, 8192> receiveBuffer{};
		DWORD audioSize = 0;
		std::atomic<bool> receiveFailed{false};
		std::atomic<DWORD> receiveError{ERROR_SUCCESS};
		std::atomic<bool> receiveDone{false};
		std::atomic<bool> gracefulDone{false};
		std::atomic<bool> providerFailed{false};
		QString providerError;
		std::mutex providerErrorMutex;
		std::thread receiver([socket, mode, captionCb, stateCb, &receiveFailed, &receiveDone, &gracefulDone,
				      &providerError, &providerErrorMutex, &providerFailed, &receiveError] {
			std::array<char, 8192> receiveBuffer{};
			while (!receiveDone) {
				DWORD received = 0;
				WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
				const DWORD result = WinHttpWebSocketReceive(socket, receiveBuffer.data(), static_cast<DWORD>(receiveBuffer.size() - 1), &received, &bufferType);
				if (result == NO_ERROR && bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
					if (!receiveDone && !gracefulDone) receiveFailed = true;
					break;
				}
				if (result == NO_ERROR && received > 0) {
					receiveBuffer[received] = '\0';
					parseProviderMessage(QByteArray(receiveBuffer.data(), received), mode, captionCb, stateCb, &gracefulDone,
							    &providerError, &providerErrorMutex, &providerFailed);
					if (providerFailed) break;
				} else if (result != NO_ERROR && result != ERROR_WINHTTP_TIMEOUT && !receiveDone) {
					receiveError = result;
					receiveFailed = true;
					break;
				}
			}
		});
		bool failed = false;
		while (!stopRequested_ && !failed && !receiveFailed && !providerFailed) {
			if (popAudio(*audio, audioBuffer, audioSize) && audioSize > 0) {
				const QString eventType = mode == QStringLiteral("translate") ? QStringLiteral("session.input_audio_buffer.append") : QStringLiteral("input_audio_buffer.append");
				if (!sendJson(socket, QJsonObject{{QStringLiteral("type"), eventType}, {QStringLiteral("audio"), base64(audioBuffer.data(), audioSize)}})) failed = true;
			}
			if (!audioSize) Sleep(10);
			audioSize = 0;
		}
		if (mode == QStringLiteral("transcribe")) sendJson(socket, QJsonObject{{QStringLiteral("type"), QStringLiteral("input_audio_buffer.commit")}});
		else sendJson(socket, QJsonObject{{QStringLiteral("type"), QStringLiteral("session.close")}});
		const DWORD drainStarted = GetTickCount();
		while (!failed && !receiveFailed && !providerFailed && !gracefulDone && GetTickCount() - drainStarted < 2500) Sleep(10);
		receiveDone = true;
		WinHttpWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
		if (receiver.joinable()) receiver.join();
		failed = failed || receiveFailed || providerFailed;
		if (receiveFailed)
			PixelionDebugLog::write("ERROR", QStringLiteral("Realtime WebSocket receive failed (WinHTTP %1)").arg(receiveError.load()));
		else if (failed && !providerFailed)
			PixelionDebugLog::write("ERROR", QStringLiteral("Realtime WebSocket audio send failed (WinHTTP %1)").arg(GetLastError()));
		else if (!failed)
			PixelionDebugLog::write("INFO", QStringLiteral("Realtime session stopped cleanly"));
		WinHttpCloseHandle(socket);
		WinHttpCloseHandle(connection);
		WinHttpCloseHandle(session);
		obs_remove_raw_audio_callback(0, audioCallback, audio);
		{
			std::lock_guard<std::mutex> lock(audio->mutex);
			audio->closed = true;
		}
		delete audio;
		running_ = false;
		if (!stopRequested_) QMetaObject::invokeMethod(this, [this] { checkUsage(true); }, Qt::QueuedConnection);
		QMetaObject::invokeMethod(this, [this] { usageTimer_->stop(); limitTestTimer_->stop(); }, Qt::QueuedConnection);
		QString finalMessage = failed ? QStringLiteral("Live captions stopped because the connection was interrupted. Check your internet connection, then try Start again. Text already received is kept; audio during the interruption cannot be transcribed.") : QStringLiteral("Session ended.");
		{
			std::lock_guard<std::mutex> lock(providerErrorMutex);
			if (!providerError.isEmpty()) finalMessage = QStringLiteral("Realtime service error: %1").arg(providerError);
		}
		QMetaObject::invokeMethod(this, [this, stateCb, finalMessage] {
			stateCb(false, finalMessage);
		}, Qt::QueuedConnection);
	});
}

void PixelionLiveSession::stop()
{
	if (pending_) {
		stopRequested_ = true;
		pending_ = false;
		if (stateCallback_) stateCallback_(false, QStringLiteral("Session start cancelled."));
		return;
	}
	if (!running_) return;
	stopRequested_ = true;
	if (limitTestTimer_) limitTestTimer_->stop();
	checkUsage(true);
}

void PixelionLiveSession::checkUsage(bool final)
{
	if (!auth_) return;
	if (sessionId_.isEmpty() || !sessionClock_.isValid()) {
		if (final) auth_->refreshUsage();
		return;
	}
	const int toSecond = static_cast<int>(sessionClock_.elapsed() / 1000);
	const int fromSecond = reportedSeconds_;
	if (toSecond <= fromSecond) {
		if (final) auth_->refreshUsage();
		return;
	}
	const QString sessionId = sessionId_;
	const QString mode = mode_;
	const QString sourceLanguage = sourceLanguage_;
	auth_->withAccessToken([this, sessionId, mode, sourceLanguage, final, fromSecond, toSecond](bool ok, const QString &access, const QString &) {
		if (!ok) {
			if (final) auth_->refreshUsage();
			return;
		}
		QJsonObject body{{QStringLiteral("elapsed_seconds"), toSecond - fromSecond},
				 {QStringLiteral("session_id"), sessionId}, {QStringLiteral("job_id"), sessionId},
				 {QStringLiteral("from_second"), fromSecond}, {QStringLiteral("to_second"), toSecond},
				 {QStringLiteral("final"), final}, {QStringLiteral("client"), QStringLiteral("obs_plugin")},
				 {QStringLiteral("client_version"), QStringLiteral("0.1.0")},
				 {QStringLiteral("mode"), mode}, {QStringLiteral("source_language"), sourceLanguage}};
		QNetworkRequest request(QUrl(QStringLiteral(PIXELION_API_BASE_URL) + QStringLiteral("/realtime-token/usage-check")));
		request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
		request.setRawHeader("Authorization", QByteArray("Bearer ") + access.toUtf8());
		request.setTransferTimeout(10000);
		auto *reply = network_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
		connect(reply, &QNetworkReply::finished, this, [this, reply, toSecond, final] {
			const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
			const QByteArray payload = reply->readAll().left(256 * 1024);
			reply->deleteLater();
			if (status != 200) {
				if (final) auth_->refreshUsage();
				return;
			}
			QJsonParseError parseError;
			const auto document = QJsonDocument::fromJson(payload, &parseError);
			if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
				if (final) auth_->refreshUsage();
				return;
			}
			reportedSeconds_ = std::max(reportedSeconds_, toSecond);
			const auto result = document.object();
			const int durationLimit = result.value(QStringLiteral("duration_limit_seconds")).toInt();
		if (durationLimit > 0) durationLimitSeconds_ = durationLimit;
			const auto remaining = result.value(QStringLiteral("remaining_minutes"));
			if (remaining.isDouble()) auth_->updateUsageRemainingMinutes(static_cast<int>(remaining.toDouble()));
			if (final) auth_->refreshUsage();
			if (!result.value(QStringLiteral("allowed")).toBool(true) || (durationLimitSeconds_ > 0 && toSecond >= durationLimitSeconds_)) {
				const QString code = result.value(QStringLiteral("error")).toString();
				if (stateCallback_) stateCallback_(true, code == QStringLiteral("DURATION_LIMIT") || durationLimitSeconds_ > 0
					? QStringLiteral("Your session limit has been reached.")
					: QStringLiteral("Your available Pixelion minutes are used up."));
				stop();
			}
		});
	});
}
