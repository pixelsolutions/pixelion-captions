#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <QElapsedTimer>
#include <memory>
#include <thread>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class PixelionAuthService;
class PixelionGuestService;
class QJsonObject;

class PixelionLiveSession final : public QObject {
public:
	using StateCallback = std::function<void(bool running, const QString &message)>;
	using CaptionCallback = std::function<void(const QString &text, bool final)>;

	explicit PixelionLiveSession(PixelionAuthService *auth, QObject *parent = nullptr);
	~PixelionLiveSession() override;

	void setCallbacks(StateCallback state, CaptionCallback caption);
	void start(const QString &mode, const QString &sourceLanguage, const QString &targetLanguage);
	void stop();
	bool isRunning() const;
	QString sessionId() const { return sessionId_; }

	// Guest (no-account) mode. When a guest service is set and active while the
	// user is signed out, the session uses the /guest/* routes (transcribe only).
	void setGuestService(PixelionGuestService *guest) { guest_ = guest; }
	bool isGuestSession() const { return guestSession_; }

	// Details of the most recent metering denial, so the dock can choose between
	// "Buy add-on credits" and "Upgrade" (MINUTES_LIMIT carries can_buy_addons).
	struct LimitInfo {
		QString code;
		bool canBuyAddons = false;
		bool hasCanBuyAddons = false;
		double addonRemainingMinutes = 0.0;
	};
	const LimitInfo &lastLimit() const { return lastLimit_; }

private:
	void requestRealtimeToken(const QString &mode, const QString &sourceLanguage, const QString &targetLanguage,
				  const QString &sessionId);
	void beginWorker(const QString &mode, const QString &sourceLanguage, const QString &targetLanguage,
			 const QString &sessionId, const QString &clientSecret);
	void finishWorker();
	void requestGuestToken(const QString &sessionId);
	void checkGuestUsage(bool final);
	void recordLimit(const QJsonObject &response);

	PixelionAuthService *auth_ = nullptr;
	PixelionGuestService *guest_ = nullptr;
	bool guestSession_ = false;
	LimitInfo lastLimit_;
	QNetworkAccessManager *network_ = nullptr;
	StateCallback stateCallback_;
	CaptionCallback captionCallback_;
	std::thread worker_;
	std::atomic<bool> running_{false};
	std::atomic<bool> pending_{false};
	std::atomic<bool> stopRequested_{false};
	QString sessionId_;
	QString mode_;
	QString sourceLanguage_;
	int elapsedSeconds_ = 0;
	int reportedSeconds_ = 0;
	int durationLimitSeconds_ = 0;
	bool durationWarningShown_ = false;
	QTimer *usageTimer_ = nullptr;
	QTimer *limitTestTimer_ = nullptr;
	QElapsedTimer sessionClock_;
	void checkUsage(bool final);
};
