// Platform-neutral parts of PixelionLiveSession: the guest (no-account) metering
// path and metering-denial bookkeeping. The platform adapters call into these
// from requestRealtimeToken() / checkUsage(), so the guest request bodies and
// handling are identical on Windows and macOS.

#include "live-session.h"
#include "guest-api.h"
#include "guest-service.h"
#include "plugin-debug-log.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <algorithm>

void PixelionLiveSession::recordLimit(const QJsonObject &response)
{
	LimitInfo info;
	info.code = response.value(QStringLiteral("error")).toString();
	if (info.code.isEmpty())
		info.code = response.value(QStringLiteral("code")).toString();
	const auto canBuy = response.value(QStringLiteral("can_buy_addons"));
	info.hasCanBuyAddons = canBuy.isBool();
	info.canBuyAddons = canBuy.toBool(false);
	info.addonRemainingMinutes = response.value(QStringLiteral("addon_remaining_minutes")).toDouble(0.0);
	lastLimit_ = info;
}

void PixelionLiveSession::requestGuestToken(const QString &sessionId)
{
	if (mode_ != QStringLiteral("transcribe")) {
		// Guests can only transcribe; the dock also blocks this before Start.
		pending_ = false;
		if (stateCallback_)
			stateCallback_(false, QStringLiteral("Live translation needs a free Pixelion account."));
		return;
	}
	const QString sourceLanguage = sourceLanguage_;
	auto *reply = network_->post(PixelionGuestApi::jsonPostRequest(QStringLiteral("/guest/realtime-token"), 15000),
				     PixelionGuestApi::tokenBody(guest_->visitorId()));
	connect(reply, &QNetworkReply::finished, this, [this, reply, sessionId, sourceLanguage] {
		const QByteArray payload = reply->readAll().left(1024 * 1024);
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QString networkError = reply->error() == QNetworkReply::NoError ? QString()
										      : reply->errorString();
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
			const QString code = PixelionGuestApi::errorCode(payload);
			PixelionDebugLog::write(
				"ERROR", QStringLiteral("Guest realtime token request failed (HTTP %1%2): %3")
						 .arg(status)
						 .arg(networkError.isEmpty() ? QString()
									     : QStringLiteral(", %1").arg(networkError),
						      code));
			QString message;
			if (status == 0 && !networkError.isEmpty()) {
				message = QStringLiteral(
					"Could not reach Pixelion. Live transcription needs an internet connection. Check your connection, then try Start again.");
			} else if (code == QStringLiteral("GUEST_LIMIT_REACHED")) {
				if (document.isObject())
					guest_->applyUsage(PixelionGuestApi::usageFromObject(document.object()));
				PixelionGuestApi::Usage spent;
				spent.remainingSeconds = 0;
				guest_->applyUsage(spent);
				message = PixelionGuestApi::limitMessage();
			} else if (code == QStringLiteral("MODE_NOT_ALLOWED")) {
				message = QStringLiteral("Live translation needs a free Pixelion account.");
			} else {
				message = QStringLiteral(
					"Pixelion could not start this session. Check your connection and try again.");
			}
			pending_ = false;
			if (stateCallback_)
				stateCallback_(false, message);
			return;
		}
		const auto object = document.object();
		const QString secret = object.value(QStringLiteral("client_secret")).toString();
		if (secret.isEmpty()) {
			PixelionDebugLog::write(
				"ERROR",
				QStringLiteral(
					"Pixelion guest realtime token response did not include a client secret"));
			pending_ = false;
			if (stateCallback_)
				stateCallback_(
					false,
					QStringLiteral("Pixelion returned an incomplete realtime session token."));
			return;
		}
		guest_->applyUsage(PixelionGuestApi::usageFromObject(object));
		const int remaining =
			PixelionGuestApi::wholeSeconds(object.value(QStringLiteral("guest_remaining_seconds")));
		const int cap = PixelionGuestApi::wholeSeconds(object.value(QStringLiteral("duration_limit_seconds")));
		beginWorker(QStringLiteral("transcribe"), sourceLanguage, QString(), sessionId, secret);
		// beginWorker() resets the limit; set it after so the dock warns and stops at the allowance.
		const int limit = (remaining >= 0 && cap > 0) ? std::min(remaining, cap) : std::max(remaining, cap);
		if (limit > 0)
			durationLimitSeconds_ = limit;
	});
}

void PixelionLiveSession::checkGuestUsage(bool final)
{
	if (!guest_)
		return;
	if (sessionId_.isEmpty() || !sessionClock_.isValid()) {
		if (final)
			guest_->refresh();
		return;
	}
	const int toSecond = static_cast<int>(sessionClock_.elapsed() / 1000);
	const int fromSecond = reportedSeconds_;
	if (toSecond <= fromSecond) {
		if (final)
			guest_->refresh();
		return;
	}
	auto *reply = network_->post(
		PixelionGuestApi::jsonPostRequest(QStringLiteral("/guest/realtime-token/usage-check"), 10000),
		PixelionGuestApi::usageCheckBody(guest_->visitorId(), sessionId_, fromSecond, toSecond));
	connect(reply, &QNetworkReply::finished, this, [this, reply, toSecond, final] {
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QByteArray payload = reply->readAll().left(256 * 1024);
		reply->deleteLater();
		QJsonParseError parseError;
		const auto document = QJsonDocument::fromJson(payload, &parseError);
		const bool parsed = parseError.error == QJsonParseError::NoError && document.isObject();
		const QJsonObject object = parsed ? document.object() : QJsonObject();
		bool limitReached = false;
		if (status == 200 && parsed) {
			const int watermark =
				PixelionGuestApi::wholeSeconds(object.value(QStringLiteral("watermark_second")));
			reportedSeconds_ = std::max(reportedSeconds_, watermark >= 0 ? watermark : toSecond);
			guest_->applyUsage(PixelionGuestApi::usageFromObject(object));
			limitReached = guest_->exhausted();
		} else if (status == 403 &&
			   PixelionGuestApi::errorCode(payload) == QStringLiteral("GUEST_LIMIT_REACHED")) {
			const int watermark =
				PixelionGuestApi::wholeSeconds(object.value(QStringLiteral("watermark_second")));
			reportedSeconds_ = std::max(reportedSeconds_, watermark >= 0 ? watermark : toSecond);
			guest_->applyUsage(PixelionGuestApi::usageFromObject(object));
			limitReached = true;
		}
		// Anything else (409 RETRY, network error, malformed body) leaves
		// reportedSeconds_ alone so the next interval re-sends the same range.
		if (limitReached && !stopRequested_) {
			if (stateCallback_)
				stateCallback_(true, PixelionGuestApi::limitMessage());
			stop();
		}
		if (final)
			guest_->refresh();
	});
}
