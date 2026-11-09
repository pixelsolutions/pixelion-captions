#pragma once

#include "addon-api.h"

#include <QObject>
#include <QString>

#include <functional>

class PixelionAuthService;
class QNetworkAccessManager;
class QTimer;

// Add-on credits for signed-in Pro users: GET /me/entitlements balance and
// POST /me/addon-checkout-link (opened in the system browser, never a webview).
class PixelionAddonService final : public QObject {
public:
	using ChangedCallback = std::function<void()>;
	// ok: the link was opened. Otherwise `error` has the copy and next action.
	using CheckoutCallback = std::function<void(bool ok, const PixelionAddonApi::CheckoutError &error)>;

	explicit PixelionAddonService(PixelionAuthService *auth, QObject *parent = nullptr);

	void setChangedCallback(ChangedCallback callback);
	const PixelionAddonApi::Entitlements &entitlements() const { return entitlements_; }
	// True while the balance could not be loaded (show "temporarily unavailable", never zero).
	bool balanceUnavailable() const { return entitlements_.known && !entitlements_.addonsAvailable; }

	void refresh();
	void clear();
	// medium: "limit_popup" (from a limit message) or "account_page" (usage screen).
	void openCheckout(const QString &medium, CheckoutCallback callback);
	// Opens the Stripe billing portal for the signed-in account (POST /billing/portal).
	// callback(false, message) shows the failure copy; the portal opens on success.
	using PortalCallback = std::function<void(bool ok, const QString &message)>;
	void openBillingPortal(PortalCallback callback);

private:
	void notify() const;
	void startPollingAfterReturn();
	void pollOnce();

	PixelionAuthService *auth_ = nullptr;
	QNetworkAccessManager *network_ = nullptr;
	QTimer *pollTimer_ = nullptr;
	ChangedCallback changed_;
	PixelionAddonApi::Entitlements entitlements_;
	quint64 refreshGeneration_ = 0;
	bool awaitingReturn_ = false;
	int pollsLeft_ = 0;
	double pollBaselineHeld_ = 0.0;
	int pollBaselinePacks_ = 0;
};
