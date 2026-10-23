#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QList>
#include <QPair>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QTcpServer;
class QTimer;

class PixelionAuthService final : public QObject {
public:
	using StateCallback = std::function<void(const QString &email, const QString &plan, const QString &message)>;
	using AccessTokenCallback = std::function<void(bool ok, const QString &token, const QString &error)>;
	struct UsageInfo {
		bool loading = false;
		bool known = false;
		bool unlimited = false;
		int usedMinutes = 0;
		int limitMinutes = 0;
		QString periodKind;
	};
	using UsageCallback = std::function<void(const UsageInfo &usage)>;

	explicit PixelionAuthService(QObject *parent = nullptr);
	~PixelionAuthService() override;

	void setStateCallback(StateCallback callback);
	void setUsageCallback(UsageCallback callback);
	void refreshUsage();
	void updateUsageRemainingMinutes(int remainingMinutes);
	void restore();
	void signIn(bool createAccount);
	void signOut();
	void withAccessToken(AccessTokenCallback callback);
	bool isSignedIn() const;
	QString email() const;
	QString userId() const;
	QString plan() const;

private:
	void handleCallback();
	void exchangeCode(const QString &code);
	void refreshAccessToken(AccessTokenCallback callback);
	void loadProfile();
	void publish(const QString &message);
	void publishUsage(const UsageInfo &usage);
	void finishSignIn(const QByteArray &body);
	void failSignIn(const QString &message);
	QNetworkReply *postForm(const QUrl &url, const QList<QPair<QString, QString>> &fields);

	QNetworkAccessManager *network_ = nullptr;
	QTcpServer *callbackServer_ = nullptr;
	QTimer *callbackTimeout_ = nullptr;
	StateCallback stateCallback_;
	UsageCallback usageCallback_;
	UsageInfo usageInfo_;
	quint64 usageRequestGeneration_ = 0;
	QString accessToken_;
	QString idToken_;
	QString email_;
	QString userId_;
	QString plan_;
	qint64 accessTokenExpiresAt_ = 0;
	QString pendingState_;
	QString pendingVerifier_;
	bool signInPending_ = false;
};
