#pragma once

// Billing portal link for the signed-in account (POST /billing/portal). The
// server returns a Stripe Customer Portal URL for the caller's own customer, and
// the plugin opens it in the system browser. Pure helpers, covered by tests.

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QUrl>

namespace PixelionBillingPortal {

// The portal URL from a 200 response, or an empty string. Only https URLs on
// billing.stripe.com are accepted.
inline QString portalUrlFromBody(int status, const QByteArray &body)
{
	if (status != 200)
		return {};
	const auto document = QJsonDocument::fromJson(body.left(64 * 1024));
	const QString url = document.object().value(QStringLiteral("portal_url")).toString();
	const QUrl parsed(url);
	if (parsed.scheme() != QStringLiteral("https") || parsed.host() != QStringLiteral("billing.stripe.com"))
		return {};
	return url;
}

// Copy for a failed request. 409 (NO_BILLING_ACCOUNT) means the account has never
// subscribed, so there is no subscription to manage yet.
inline QString errorText(int status)
{
	if (status == 409)
		return QStringLiteral("There is no subscription to manage on this account yet.");
	if (status == 401)
		return QStringLiteral("Sign in again to manage your subscription.");
	return QStringLiteral("Could not open the billing page. Check your connection and try again.");
}

} // namespace PixelionBillingPortal
