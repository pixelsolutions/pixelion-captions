#pragma once

// Add-on credits API helpers: GET /me/entitlements parsing, amount/price
// formatting and the POST /me/addon-checkout-link request/error mapping.
// Everything here is platform-neutral; amounts are always rounded DOWN.

#include <QByteArray>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <algorithm>
#include <cmath>

namespace PixelionAddonApi {

struct Pack {
	QString id;
	double purchasedMinutes = 0.0;
	double remainingMinutes = 0.0;
	double usedMinutes = 0.0;
	qint64 purchasedAt = 0;
	qint64 expiresAt = 0;
	bool expired = false;
};

struct Entitlements {
	bool known = false;
	QString planId;
	QString tier; // guest | free | pro | hero
	QString subscriptionStatus;
	bool addonEligible = false;
	// limits.session_cap_min from GET /me/entitlements: longest single session on
	// this plan, in minutes. 0 when the server does not report it.
	int sessionCapMinutes = 0;
	// addon settings (null when the plan does not offer add-ons)
	bool hasAddonSettings = false;
	double priceCentsPerMinute = 0.0;
	int minMinutes = 0;
	// usage
	bool hasMinutesRemaining = false;
	double minutesRemaining = 0.0;
	// addons block
	bool addonsAvailable = true; // false: balance could not be read; never show zero
	double heldMinutes = 0.0;
	QList<Pack> packs;
};

// Rounds down to 2 decimals; never rounds an amount up.
inline double floor2(double value)
{
	if (!std::isfinite(value) || value <= 0.0)
		return 0.0;
	return std::floor(value * 100.0 + 1e-9) / 100.0;
}

// "125", "12.5", "0.25": at most 2 decimals, trailing zeros removed.
inline QString formatMinutes(double value)
{
	QString text = QString::number(floor2(value), 'f', 2);
	if (text.contains(QLatin1Char('.'))) {
		while (text.endsWith(QLatin1Char('0')))
			text.chop(1);
		if (text.endsWith(QLatin1Char('.')))
			text.chop(1);
	}
	return text;
}

// "from $5.00 for 125 minutes" built from the server's catalog values; empty
// when the add-on settings are missing (the caller falls back to plain copy).
inline QString priceText(const Entitlements &e)
{
	if (!e.hasAddonSettings || e.priceCentsPerMinute <= 0.0 || e.minMinutes <= 0)
		return {};
	const double dollars = std::floor(e.priceCentsPerMinute * e.minMinutes) / 100.0;
	return QStringLiteral("from $%1 for %2 minutes").arg(QString::number(dollars, 'f', 2)).arg(e.minMinutes);
}

inline bool parseEntitlements(int status, const QByteArray &body, Entitlements *out)
{
	if (status != 200 || !out)
		return false;
	QJsonParseError error;
	const auto document = QJsonDocument::fromJson(body, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject())
		return false;
	const auto root = document.object();
	Entitlements e;
	e.known = true;
	e.planId = root.value(QStringLiteral("plan_id")).toString();
	e.tier = root.value(QStringLiteral("tier")).toString();
	e.subscriptionStatus = root.value(QStringLiteral("subscription_status")).toString();
	e.addonEligible = root.value(QStringLiteral("addon_eligible")).toBool(false);
	const auto limits = root.value(QStringLiteral("limits")).toObject();
	e.sessionCapMinutes = static_cast<int>(limits.value(QStringLiteral("session_cap_min")).toDouble(0.0));
	const auto addon = root.value(QStringLiteral("addon"));
	if (addon.isObject()) {
		const auto settings = addon.toObject();
		e.hasAddonSettings = true;
		e.priceCentsPerMinute = settings.value(QStringLiteral("price_cents_per_minute")).toDouble(0.0);
		e.minMinutes = static_cast<int>(settings.value(QStringLiteral("min_minutes")).toDouble(0.0));
	}
	const auto usage = root.value(QStringLiteral("usage")).toObject();
	const auto remaining = usage.value(QStringLiteral("minutes_remaining"));
	if (remaining.isDouble()) {
		e.hasMinutesRemaining = true;
		e.minutesRemaining = remaining.toDouble();
	}
	const auto addons = root.value(QStringLiteral("addons")).toObject();
	e.addonsAvailable = addons.value(QStringLiteral("available")).toBool(true);
	e.heldMinutes = addons.value(QStringLiteral("held_minutes")).toDouble(0.0);
	const auto packs = addons.value(QStringLiteral("packs")).toArray();
	for (const auto &item : packs) {
		const auto object = item.toObject();
		Pack pack;
		pack.id = object.value(QStringLiteral("pack_id")).toString();
		pack.purchasedMinutes = object.value(QStringLiteral("purchased_minutes")).toDouble(0.0);
		pack.remainingMinutes = object.value(QStringLiteral("remaining_minutes")).toDouble(0.0);
		pack.usedMinutes = object.value(QStringLiteral("used_minutes")).toDouble(0.0);
		pack.purchasedAt = static_cast<qint64>(object.value(QStringLiteral("purchased_at")).toDouble(0.0));
		pack.expiresAt = static_cast<qint64>(object.value(QStringLiteral("expires_at")).toDouble(0.0));
		pack.expired = object.value(QStringLiteral("expired")).toBool(false);
		e.packs.append(pack);
	}
	*out = e;
	return true;
}

// One credits row: "40 of 125 used - 85 left - Expires Nov 8, 2027" or
// "Expired Nov 8, 2027, 12 left".
inline QString packText(const Pack &pack)
{
	const QString date = QDateTime::fromSecsSinceEpoch(pack.expiresAt).toLocalTime().date().toString(Qt::TextDate);
	const QString left = formatMinutes(pack.remainingMinutes);
	if (pack.expired)
		return QStringLiteral("Expired %1, %2 left").arg(date, left);
	return QStringLiteral("%1 of %2 used, %3 left. Expires %4")
		.arg(formatMinutes(pack.usedMinutes), formatMinutes(pack.purchasedMinutes), left, date);
}

// Body of POST /me/addon-checkout-link. `platform` is the value the plugin
// already sends on its other requests (the backend allow-list includes it).
inline QByteArray checkoutBody(const QString &medium)
{
	const QJsonObject body{{QStringLiteral("platform"), QStringLiteral("obs_plugin")},
			       {QStringLiteral("medium"), medium}};
	return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

enum class CheckoutAction { None, Retry, Upgrade, Account };

struct CheckoutError {
	QString message;
	CheckoutAction action = CheckoutAction::Retry;
};

// Section 3 of the add-on spec: the 403 codes map to fixed copy; any other
// failure is a short error with Retry. None of these open the website login.
inline CheckoutError checkoutError(int status, const QByteArray &body)
{
	if (status == 403) {
		const auto document = QJsonDocument::fromJson(body.left(64 * 1024));
		const QString code = document.object().value(QStringLiteral("code")).toString();
		if (code == QStringLiteral("ADDON_FREE_PLAN"))
			return {QStringLiteral("Credits are for Pixelion Pro subscribers."), CheckoutAction::Upgrade};
		if (code == QStringLiteral("ADDON_TRIAL"))
			return {QStringLiteral("Credits are not available during a trial."), CheckoutAction::None};
		if (code == QStringLiteral("ADDON_NOT_OFFERED"))
			return {QStringLiteral("Your plan does not include add-on credits."), CheckoutAction::None};
		if (code == QStringLiteral("ADDON_NO_ACTIVE_SUBSCRIPTION"))
			return {QStringLiteral("Your subscription needs attention."), CheckoutAction::Account};
	}
	return {QStringLiteral("Could not open checkout. Check your connection and try again."), CheckoutAction::Retry};
}

// Reads checkout_url from a 200 response. Only https URLs are accepted.
inline QString checkoutUrl(int status, const QByteArray &body)
{
	if (status != 200)
		return {};
	const auto document = QJsonDocument::fromJson(body.left(64 * 1024));
	const QString url = document.object().value(QStringLiteral("checkout_url")).toString();
	return url.startsWith(QStringLiteral("https://")) ? url : QString();
}

} // namespace PixelionAddonApi
