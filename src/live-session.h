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

private:
	void requestRealtimeToken(const QString &mode, const QString &sourceLanguage, const QString &targetLanguage,
			  const QString &sessionId);
	void beginWorker(const QString &mode, const QString &sourceLanguage, const QString &targetLanguage, const QString &sessionId,
			 const QString &clientSecret);
	void finishWorker();

	PixelionAuthService *auth_ = nullptr;
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
