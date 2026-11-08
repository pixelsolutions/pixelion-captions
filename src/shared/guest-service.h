#pragma once

#include "guest-api.h"

#include <QObject>
#include <QString>

#include <functional>

class QNetworkAccessManager;

// Guest (no-account) mode state: the per-install visitor id, the last known
// server-reported allowance, and the GET /guest/entitlements refresh. The id and
// last known usage persist in the plugin config folder across OBS restarts.
class PixelionGuestService final : public QObject {
public:
	using ChangedCallback = std::function<void()>;

	explicit PixelionGuestService(QObject *parent = nullptr);

	void setChangedCallback(ChangedCallback callback);

	const QString &visitorId() const { return visitorId_; }
	bool active() const { return active_; }
	void setActive(bool active);

	const PixelionGuestApi::Usage &usage() const { return usage_; }
	// Allowance shown to the user: server value, else the last known, else 10 minutes.
	int lifetimeSeconds() const { return PixelionGuestApi::effectiveLifetimeSeconds(usage_); }
	bool exhausted() const { return PixelionGuestApi::exhausted(usage_); }

	// GET /guest/entitlements?visitor_id=...  A failure keeps the last known value.
	void refresh();
	// Applies a usage-check / token response (values the server did not send are kept).
	void applyUsage(const PixelionGuestApi::Usage &fresh);

private:
	void load();
	void save() const;
	void notify() const;

	QNetworkAccessManager *network_ = nullptr;
	ChangedCallback changed_;
	QString visitorId_;
	PixelionGuestApi::Usage usage_;
	bool active_ = false;
	quint64 refreshGeneration_ = 0;
};
