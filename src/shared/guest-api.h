#pragma once

// Guest access API helpers (no account). Shared by both platform adapters so the
// request bodies and response parsing are identical on Windows and macOS.
// Guest routes never carry an Authorization header.

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include <algorithm>
#include <cmath>

#ifndef PIXELION_API_BASE_URL
#define PIXELION_API_BASE_URL "https://api.pixelion.ai"
#endif

namespace PixelionGuestApi {

// Shown until the server answers, and whenever GET /guest/entitlements fails.
constexpr int kDefaultLifetimeSeconds = 600;

// Message the live session reports when the guest allowance is used up.
inline QString limitMessage()
{
	return QStringLiteral("Your free guest minutes are used up.");
}

// Seconds values; -1 means "not known". A missing or malformed value is never
// treated as zero used.
struct Usage {
	int lifetimeSeconds = -1;
	int usedSeconds = -1;
	int remainingSeconds = -1;
};

inline int wholeSeconds(const QJsonValue &value)
{
	if (!value.isDouble())
		return -1;
	const double number = value.toDouble();
	if (!std::isfinite(number) || number < 0.0 || number > 1.0e9)
		return -1;
	return static_cast<int>(std::floor(number));
}

// Reads guest_lifetime_seconds / guest_used_seconds / guest_remaining_seconds.
inline Usage usageFromObject(const QJsonObject &object)
{
	Usage usage;
	usage.lifetimeSeconds = wholeSeconds(object.value(QStringLiteral("guest_lifetime_seconds")));
	if (usage.lifetimeSeconds == 0)
		usage.lifetimeSeconds = -1;
	usage.usedSeconds = wholeSeconds(object.value(QStringLiteral("guest_used_seconds")));
	usage.remainingSeconds = wholeSeconds(object.value(QStringLiteral("guest_remaining_seconds")));
	return usage;
}

// Fresh values replace known ones only where the server actually sent them.
inline Usage merge(const Usage &known, const Usage &fresh)
{
	Usage out = known;
	if (fresh.lifetimeSeconds > 0)
		out.lifetimeSeconds = fresh.lifetimeSeconds;
	if (fresh.usedSeconds >= 0)
		out.usedSeconds = fresh.usedSeconds;
	if (fresh.remainingSeconds >= 0)
		out.remainingSeconds = fresh.remainingSeconds;
	// A response with remaining but no used count (the token response): keep the
	// pair consistent instead of leaving a stale used value.
	if (fresh.usedSeconds < 0 && fresh.remainingSeconds >= 0 && out.lifetimeSeconds > 0)
		out.usedSeconds = std::max(0, out.lifetimeSeconds - fresh.remainingSeconds);
	return out;
}

// GET /guest/entitlements response. Anything other than a 200 JSON object (404,
// 400 INVALID_VISITOR_ID, 500, network error status 0, malformed body) is
// "unavailable": the caller keeps its last known value.
inline bool parseEntitlements(int status, const QByteArray &body, Usage *out)
{
	if (status != 200)
		return false;
	QJsonParseError error;
	const auto document = QJsonDocument::fromJson(body, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject())
		return false;
	if (out)
		*out = usageFromObject(document.object());
	return true;
}

inline int effectiveLifetimeSeconds(const Usage &usage)
{
	return usage.lifetimeSeconds > 0 ? usage.lifetimeSeconds : kDefaultLifetimeSeconds;
}

// Seconds used, derived from remaining when only that was sent. -1 if unknown.
inline int effectiveUsedSeconds(const Usage &usage)
{
	if (usage.usedSeconds >= 0)
		return std::min(usage.usedSeconds, effectiveLifetimeSeconds(usage));
	if (usage.remainingSeconds >= 0 && usage.lifetimeSeconds > 0)
		return std::max(0, usage.lifetimeSeconds - usage.remainingSeconds);
	return -1;
}

inline bool exhausted(const Usage &usage)
{
	if (usage.remainingSeconds >= 0)
		return usage.remainingSeconds <= 0;
	const int used = effectiveUsedSeconds(usage);
	return used >= 0 && used >= effectiveLifetimeSeconds(usage);
}

// Whole minutes, rounded down (never rounds a remaining amount up); at least 1
// for the headline total so the sign-in hint never reads "0 minutes".
inline int wholeMinutes(int seconds)
{
	return std::max(0, seconds) / 60;
}

inline QString minutesText(int minutes)
{
	return minutes == 1 ? QStringLiteral("1 minute") : QStringLiteral("%1 minutes").arg(minutes);
}

inline QString offerText(const Usage &usage)
{
	const int minutes = std::max(1, wholeMinutes(effectiveLifetimeSeconds(usage)));
	return QStringLiteral("Try Pixelion Captions for %1").arg(minutesText(minutes));
}

// UUID v4 text, as the API requires.
inline bool isValidVisitorId(const QString &value)
{
	const QUuid id = QUuid::fromString(value);
	if (id.isNull() || value.size() != 36)
		return false;
	return id.version() == QUuid::Random && id.variant() == QUuid::DCE;
}

inline QString newVisitorId()
{
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// ----- Request construction (identical on both platforms) -----

inline QUrl entitlementsUrl(const QString &visitorId)
{
	QUrl url(QStringLiteral(PIXELION_API_BASE_URL) + QStringLiteral("/guest/entitlements"));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("visitor_id"), visitorId);
	url.setQuery(query);
	return url;
}

inline QNetworkRequest jsonPostRequest(const QString &path, int timeoutMs)
{
	QNetworkRequest request(QUrl(QStringLiteral(PIXELION_API_BASE_URL) + path));
	request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	request.setTransferTimeout(timeoutMs);
	return request;
}

// POST /guest/realtime-token. Guests can only transcribe.
inline QByteArray tokenBody(const QString &visitorId)
{
	const QJsonObject body{{QStringLiteral("visitor_id"), visitorId},
			       {QStringLiteral("mode"), QStringLiteral("transcribe")},
			       {QStringLiteral("client"), QStringLiteral("obs_plugin")}};
	return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

// POST /guest/realtime-token/usage-check. Whole seconds, session-relative.
inline QByteArray usageCheckBody(const QString &visitorId, const QString &sessionId, int fromSecond, int toSecond)
{
	const QJsonObject body{{QStringLiteral("visitor_id"), visitorId},
			       {QStringLiteral("session_id"), sessionId},
			       {QStringLiteral("from_second"), fromSecond},
			       {QStringLiteral("to_second"), toSecond}};
	return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

inline QString errorCode(const QByteArray &body)
{
	const auto document = QJsonDocument::fromJson(body.left(256 * 1024));
	if (!document.isObject())
		return {};
	const auto object = document.object();
	const QString code = object.value(QStringLiteral("code")).toString();
	return code.isEmpty() ? object.value(QStringLiteral("error")).toString() : code;
}

} // namespace PixelionGuestApi
