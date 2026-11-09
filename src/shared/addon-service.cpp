#include "addon-service.h"
#include "billing-portal.h"
#include "auth-service.h"
#include "plugin-debug-log.h"

#include <QDesktopServices>
#include <QGuiApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#ifndef PIXELION_API_BASE_URL
#define PIXELION_API_BASE_URL "https://api.pixelion.ai"
#endif

namespace {
constexpr int kPollIntervalMs = 5000;
constexpr int kPollAttempts = 12; // 12 x 5 s = 60 s
} // namespace

PixelionAddonService::PixelionAddonService(PixelionAuthService *auth, QObject *parent)
	: QObject(parent),
	  auth_(auth),
	  network_(new QNetworkAccessManager(this)),
	  pollTimer_(new QTimer(this))
{
	pollTimer_->setInterval(kPollIntervalMs);
	connect(pollTimer_, &QTimer::timeout, this, [this] { pollOnce(); });
	// The app is not told when the browser checkout finishes. Start polling once
	// the user comes back to OBS after opening a checkout link.
	connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
		if (state == Qt::ApplicationActive && awaitingReturn_) {
			awaitingReturn_ = false;
			startPollingAfterReturn();
		}
	});
}

void PixelionAddonService::setChangedCallback(ChangedCallback callback)
{
	changed_ = std::move(callback);
}

void PixelionAddonService::notify() const
{
	if (changed_)
		changed_();
}

void PixelionAddonService::clear()
{
	++refreshGeneration_;
	pollTimer_->stop();
	awaitingReturn_ = false;
	entitlements_ = PixelionAddonApi::Entitlements();
	notify();
}

void PixelionAddonService::refresh()
{
	if (!auth_ || !auth_->isSignedIn())
		return;
	const quint64 generation = ++refreshGeneration_;
	auth_->withAccessToken([this, generation](bool ok, const QString &access, const QString &) {
		if (!ok || generation != refreshGeneration_)
			return;
		QNetworkRequest request(
			QUrl(QStringLiteral(PIXELION_API_BASE_URL) + QStringLiteral("/me/entitlements")));
		request.setRawHeader("Authorization", QByteArray("Bearer ") + access.toUtf8());
		request.setTransferTimeout(10000);
		auto *reply = network_->get(request);
		connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
			const QByteArray body = reply->readAll().left(1024 * 1024);
			const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
			reply->deleteLater();
			if (generation != refreshGeneration_)
				return;
			PixelionAddonApi::Entitlements fresh;
			if (!PixelionAddonApi::parseEntitlements(status, body, &fresh)) {
				PixelionDebugLog::write(
					"WARNING", QStringLiteral("Entitlements unavailable (HTTP %1)").arg(status));
				return; // keep the last known balance
			}
			entitlements_ = fresh;
			notify();
		});
	});
}

void PixelionAddonService::openCheckout(const QString &medium, CheckoutCallback callback)
{
	if (!auth_ || !auth_->isSignedIn()) {
		if (callback)
			callback(false, {QStringLiteral("Sign in to Pixelion to add credits."),
					 PixelionAddonApi::CheckoutAction::None});
		return;
	}
	// A fresh link every press: links expire after 10 minutes and are never cached.
	auth_->withAccessToken([this, medium, callback](bool ok, const QString &access, const QString &) {
		if (!ok) {
			if (callback)
				callback(false, PixelionAddonApi::checkoutError(0, QByteArray()));
			return;
		}
		QNetworkRequest request(
			QUrl(QStringLiteral(PIXELION_API_BASE_URL) + QStringLiteral("/me/addon-checkout-link")));
		request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
		request.setRawHeader("Authorization", QByteArray("Bearer ") + access.toUtf8());
		request.setTransferTimeout(15000);
		auto *reply = network_->post(request, PixelionAddonApi::checkoutBody(medium));
		connect(reply, &QNetworkReply::finished, this, [this, reply, callback] {
			const QByteArray body = reply->readAll().left(64 * 1024);
			const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
			reply->deleteLater();
			const QString url = PixelionAddonApi::checkoutUrl(status, body);
			if (url.isEmpty() || !QDesktopServices::openUrl(QUrl(url))) {
				PixelionDebugLog::write(
					"ERROR", QStringLiteral("Add-on checkout link failed (HTTP %1)").arg(status));
				if (callback)
					callback(false, PixelionAddonApi::checkoutError(status, body));
				return;
			}
			awaitingReturn_ = true;
			pollBaselineHeld_ = entitlements_.heldMinutes;
			pollBaselinePacks_ = static_cast<int>(entitlements_.packs.size());
			if (callback)
				callback(true, {});
		});
	});
}

void PixelionAddonService::openBillingPortal(PortalCallback callback)
{
	if (!auth_ || !auth_->isSignedIn()) {
		if (callback)
			callback(false, QStringLiteral("Sign in to Pixelion to manage your subscription."));
		return;
	}
	auth_->withAccessToken([this, callback](bool ok, const QString &access, const QString &) {
		if (!ok) {
			if (callback)
				callback(false, PixelionBillingPortal::errorText(0));
			return;
		}
		QNetworkRequest request(QUrl(QStringLiteral(PIXELION_API_BASE_URL) + QStringLiteral("/billing/portal")));
		request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
		request.setRawHeader("Authorization", QByteArray("Bearer ") + access.toUtf8());
		request.setTransferTimeout(15000);
		auto *reply = network_->post(request, QByteArray("{}"));
		connect(reply, &QNetworkReply::finished, this, [reply, callback] {
			const QByteArray body = reply->readAll().left(64 * 1024);
			const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
			reply->deleteLater();
			const QString url = PixelionBillingPortal::portalUrlFromBody(status, body);
			if (url.isEmpty() || !QDesktopServices::openUrl(QUrl(url))) {
				PixelionDebugLog::write("ERROR", QStringLiteral("Billing portal link failed (HTTP %1)").arg(status));
				if (callback)
					callback(false, PixelionBillingPortal::errorText(status));
				return;
			}
			if (callback)
				callback(true, {});
		});
	});
}

void PixelionAddonService::startPollingAfterReturn()
{
	pollsLeft_ = kPollAttempts;
	pollTimer_->start();
	pollOnce();
}

void PixelionAddonService::pollOnce()
{
	if (pollsLeft_ <= 0 || !auth_ || !auth_->isSignedIn()) {
		pollTimer_->stop();
		return;
	}
	--pollsLeft_;
	const auto before = entitlements_;
	refresh();
	// Stop once credits appear (checked against the baseline from before checkout).
	if (before.known && (static_cast<int>(before.packs.size()) > pollBaselinePacks_ ||
			     before.heldMinutes > pollBaselineHeld_ + 1e-9))
		pollTimer_->stop();
	if (pollsLeft_ <= 0)
		pollTimer_->stop();
}
