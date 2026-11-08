#include "guest-service.h"
#include "plugin-debug-log.h"

#include <obs-module.h>
#include <util/bmem.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace {
QString configFilePath()
{
	char *path = obs_module_config_path("guest.json");
	if (!path)
		return {};
	const QString result = QString::fromUtf8(path);
	bfree(path);
	return result;
}
} // namespace

PixelionGuestService::PixelionGuestService(QObject *parent) : QObject(parent), network_(new QNetworkAccessManager(this))
{
	load();
}

void PixelionGuestService::setChangedCallback(ChangedCallback callback)
{
	changed_ = std::move(callback);
}

void PixelionGuestService::notify() const
{
	if (changed_)
		changed_();
}

void PixelionGuestService::setActive(bool active)
{
	if (active_ == active)
		return;
	active_ = active;
	notify();
}

void PixelionGuestService::load()
{
	const QString path = configFilePath();
	if (!path.isEmpty()) {
		QFile file(path);
		if (file.open(QIODevice::ReadOnly)) {
			const auto document = QJsonDocument::fromJson(file.read(64 * 1024));
			if (document.isObject()) {
				const auto object = document.object();
				const QString stored = object.value(QStringLiteral("visitor_id")).toString();
				if (PixelionGuestApi::isValidVisitorId(stored))
					visitorId_ = stored;
				usage_ = PixelionGuestApi::usageFromObject(object);
			}
		}
	}
	if (visitorId_.isEmpty()) {
		visitorId_ = PixelionGuestApi::newVisitorId();
		save();
	}
}

void PixelionGuestService::save() const
{
	const QString path = configFilePath();
	if (path.isEmpty())
		return;
	QDir().mkpath(QFileInfo(path).absolutePath());
	QJsonObject object{{QStringLiteral("visitor_id"), visitorId_}};
	if (usage_.lifetimeSeconds > 0)
		object.insert(QStringLiteral("guest_lifetime_seconds"), usage_.lifetimeSeconds);
	if (usage_.usedSeconds >= 0)
		object.insert(QStringLiteral("guest_used_seconds"), usage_.usedSeconds);
	if (usage_.remainingSeconds >= 0)
		object.insert(QStringLiteral("guest_remaining_seconds"), usage_.remainingSeconds);
	QFile file(path);
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
		file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

void PixelionGuestService::applyUsage(const PixelionGuestApi::Usage &fresh)
{
	usage_ = PixelionGuestApi::merge(usage_, fresh);
	save();
	notify();
}

void PixelionGuestService::refresh()
{
	const quint64 generation = ++refreshGeneration_;
	QNetworkRequest request(PixelionGuestApi::entitlementsUrl(visitorId_));
	request.setTransferTimeout(10000);
	auto *reply = network_->get(request);
	connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
		const QByteArray body = reply->readAll().left(256 * 1024);
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		reply->deleteLater();
		if (generation != refreshGeneration_)
			return;
		PixelionGuestApi::Usage fresh;
		if (!PixelionGuestApi::parseEntitlements(status, body, &fresh)) {
			// Unavailable: keep the last known value, then the 10 minute default.
			PixelionDebugLog::write(
				"WARNING",
				QStringLiteral("Guest entitlements unavailable (HTTP %1); using last known value")
					.arg(status));
			notify();
			return;
		}
		applyUsage(fresh);
	});
}
