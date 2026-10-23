#include "plugin-debug-log.h"

#include <obs-module.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>

namespace PixelionDebugLog {
namespace {
QMutex logMutex;
constexpr qint64 kMaxLogBytes = 2 * 1024 * 1024;

QString configuredPath()
{
	char *path = obs_module_config_path("pixelion-live-captions.log");
	if (!path)
		return {};
	const QString result = QString::fromUtf8(path);
	bfree(path);
	return result;
}

void rotateIfNeeded(const QString &path)
{
	QFileInfo info(path);
	if (!info.exists() || info.size() < kMaxLogBytes)
		return;
	const QString previousPath = path + QStringLiteral(".1");
	QFile::remove(previousPath);
	QFile::rename(path, previousPath);
}
} // namespace

QString filePath()
{
	return configuredPath();
}

void write(const char *level, const QString &message)
{
	QMutexLocker lock(&logMutex);
	const QString path = configuredPath();
	if (path.isEmpty()) {
		blog(LOG_WARNING, "Pixelion Live Captions: could not resolve debug log path");
		return;
	}
	const QFileInfo info(path);
	if (!QDir().mkpath(info.absolutePath())) {
		blog(LOG_WARNING, "Pixelion Live Captions: could not create debug log directory");
		return;
	}
	rotateIfNeeded(path);
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
		blog(LOG_WARNING, "Pixelion Live Captions: could not open debug log file");
		return;
	}
	const QString line = QStringLiteral("%1 [%2] %3\r\n")
			     .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
				  QString::fromLatin1(level ? level : "INFO"), message);
	const QByteArray utf8 = line.toUtf8();
	if (file.write(utf8) != utf8.size() || !file.flush())
		blog(LOG_WARNING, "Pixelion Live Captions: could not flush debug log file");
}

} // namespace PixelionDebugLog
