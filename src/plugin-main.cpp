#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include "auth-service.h"
#include "live-session.h"
#include "platform/windows/tls-backend.h"
#include "plugin-debug-log.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QDir>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFontComboBox>
#include <QFontMetrics>
#include <QFrame>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QPen>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QPixmap>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSizePolicy>
#include <QSaveFile>
#include <QScrollArea>
#include <QStandardPaths>
#include <QSpinBox>
#include <QSignalBlocker>
#include <QStringList>
#include <QIcon>
#include <QKeyEvent>
#include <QTextLayout>
#include <QTextOption>
#include <QTableWidget>
#include <QHeaderView>
#include <QToolButton>
#include <QTimer>
#include <QVector>
#include <QVBoxLayout>
#include <QWidget>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("pixelion-captions", "en-US")

namespace {
constexpr auto kDockId = "pixelion-live-captions-control";
constexpr auto kSourceId = "pixelion_live_captions_source";
constexpr uint32_t kOverlayWidth = 1280;
constexpr uint32_t kOverlayHeight = 220;
QPointer<QWidget> dockWidget;
PixelionAuthService *authService = nullptr;
PixelionLiveSession *liveSession = nullptr;
QMutex styleMutex;
QFont captionFont(QStringLiteral("Segoe UI"));
QColor captionTextColor(Qt::white);
QColor captionBackgroundColor(Qt::black);
uint32_t captionFontSize = 32;
uint32_t captionBackgroundOpacity = 70;
uint32_t captionTextAlignment = 1;
uint32_t captionVisibleLines = 2;
uint32_t captionBackgroundMode = 0;
uint32_t captionPadding = 36;
uint64_t styleRevision = 1;
QString liveCaptionText;
QString captionCommittedDisplayText;
QString captionDraftDisplayText;
QString transcriptCommittedText;
QString transcriptDraftText;
QString transcriptFilePath;
QString transcriptMetadataPath;
QString transcriptDirectory;
QString transcriptFilenamePattern = QStringLiteral("Pixelion Live Captions {date} {time}");
bool saveTranscriptLocally = true;
bool autoSaveTranscriptToPixelion = false;
QString transcriptSessionId;
QString transcriptJobId;
QString transcriptSessionTitle;
QString transcriptLibraryState = QStringLiteral("idle");
QString transcriptLibraryUrl;
QString transcriptMode;
QString transcriptSourceLanguage;
QString transcriptTargetLanguage;
qint64 transcriptSessionStartedAt = 0;
QElapsedTimer transcriptSessionClock;
struct TranscriptCue {
	qint64 startMs = 0;
	qint64 endMs = 0;
	QString text;
};
QVector<TranscriptCue> transcriptCues;
QString pendingCueText;
qint64 pendingCueStartMs = -1;
qint64 lastCaptionDeltaMs = -1;
bool streamingActive = false;
bool recordingActive = false;
std::function<void(bool)> autoStartHandler;

class CollapsibleComboBox final : public QComboBox {
public:
	explicit CollapsibleComboBox(QWidget *parent) : QComboBox(parent) {}

	void setExpandedCallback(std::function<void(bool)> callback)
	{
		expandedCallback = std::move(callback);
	}

	void showPopup() override
	{
		setExpanded(!expanded);
	}

	void hidePopup() override
	{
		setExpanded(false);
		QComboBox::hidePopup();
	}

protected:
	void keyPressEvent(QKeyEvent *event) override
	{
		if (event->key() == Qt::Key_Escape && expanded) {
			setExpanded(false);
			event->accept();
			return;
		}
		QComboBox::keyPressEvent(event);
	}

private:
	void setExpanded(bool value)
	{
		if (expanded == value)
			return;
		expanded = value;
		if (expandedCallback)
			expandedCallback(expanded);
	}

	bool expanded = false;
	std::function<void(bool)> expandedCallback;
};

class SpinnerButton final : public QPushButton {
public:
	explicit SpinnerButton(QWidget *parent = nullptr) : QPushButton(QStringLiteral("Start"), parent)
	{
		setAccessibleName(QStringLiteral("Start live captions"));
		timer_.setInterval(80);
		QObject::connect(&timer_, &QTimer::timeout, this, [this] {
			frame_ = (frame_ + 1) % 8;
			update();
		});
	}

	void setSessionRunning(bool running)
	{
		running_ = running;
		if (!connecting_) setText(running_ ? QStringLiteral("Stop") : idleLabel_);
		setAccessibleName(running_ ? QStringLiteral("Stop live captions") : QStringLiteral("Start live captions"));
	}

	void setConnecting(bool connecting)
	{
		if (connecting_ == connecting) return;
		connecting_ = connecting;
		if (connecting_) {
			setText(QString());
			setAccessibleName(QStringLiteral("Connecting to live captions"));
			setToolTip(QStringLiteral("Connecting to Pixelion live captions…"));
			timer_.start();
		} else {
			timer_.stop();
			setSessionRunning(running_);
			setToolTip(QString());
		}
		update();
	}

	void setIdleLabel(const QString &label)
	{
		idleLabel_ = label;
		if (!running_ && !connecting_) setText(idleLabel_);
	}

protected:
	void paintEvent(QPaintEvent *event) override
	{
		QPushButton::paintEvent(event);
		if (!connecting_) return;
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.translate(width() / 2.0, height() / 2.0);
		const QColor base = palette().color(QPalette::ButtonText);
		for (int i = 0; i < 8; ++i) {
			QColor color = base;
			color.setAlpha(40 + ((i - frame_ + 8) % 8) * 25);
			painter.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap));
			painter.drawLine(QPointF(0, -3.5), QPointF(0, -7));
			painter.rotate(45.0);
		}
	}

private:
	QTimer timer_;
	int frame_ = 0;
	bool running_ = false;
	bool connecting_ = false;
	QString idleLabel_ = QStringLiteral("Start");
};

class ActiveSessionIndicator final : public QWidget {
public:
	explicit ActiveSessionIndicator(QWidget *parent = nullptr) : QWidget(parent)
	{
		setFixedSize(12, 12);
		setAccessibleName(QStringLiteral("Live caption session active"));
		setToolTip(QStringLiteral("Live caption session active"));
		setVisible(false);
		timer_.setInterval(500);
		QObject::connect(&timer_, &QTimer::timeout, this, [this] {
			bright_ = !bright_;
			update();
		});
	}

	void setActive(bool active)
	{
		if (active == active_) return;
		active_ = active;
		bright_ = true;
		setVisible(active_);
		if (active_) timer_.start();
		else timer_.stop();
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(Qt::NoPen);
		painter.setBrush(active_ && bright_ ? QColor(QStringLiteral("#35c98a")) : QColor(QStringLiteral("#1c6b4c")));
		painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 1.5, 1.5);
	}

private:
	QTimer timer_;
	bool active_ = false;
	bool bright_ = true;
};

class SessionElapsedTimer final : public QLabel {
public:
	explicit SessionElapsedTimer(QWidget *parent = nullptr) : QLabel(QStringLiteral("00:00:00"), parent)
	{
		setObjectName(QStringLiteral("mutedText"));
		setAlignment(Qt::AlignCenter);
		setAccessibleName(QStringLiteral("Live caption session elapsed time"));
		setToolTip(QStringLiteral("Elapsed time for this live caption session"));
		setVisible(false);
		clockTimer_.setInterval(250);
		QObject::connect(&clockTimer_, &QTimer::timeout, this, [this] { updateElapsedTime(); });
	}

	void setRunning(bool running)
	{
		if (running == running_) return;
		running_ = running;
		if (running_) {
			elapsed_.start();
			updateElapsedTime();
			setVisible(true);
			clockTimer_.start();
		} else {
			clockTimer_.stop();
			setVisible(false);
			setText(QStringLiteral("00:00:00"));
		}
	}

private:
	void updateElapsedTime()
	{
		const qint64 totalSeconds = elapsed_.elapsed() / 1000;
		const qint64 hours = totalSeconds / 3600;
		const qint64 minutes = (totalSeconds / 60) % 60;
		const qint64 seconds = totalSeconds % 60;
		setText(QStringLiteral("%1:%2:%3").arg(hours, 2, 10, QLatin1Char('0'))
				.arg(minutes, 2, 10, QLatin1Char('0')).arg(seconds, 2, 10, QLatin1Char('0')));
	}

	QElapsedTimer elapsed_;
	QTimer clockTimer_;
	bool running_ = false;
};

int maximumTranscriptionMinutesForPlan(const QString &plan)
{
	const QString normalizedPlan = plan.trimmed().toLower();
	return normalizedPlan == QStringLiteral("pro") || normalizedPlan == QStringLiteral("hero") ? 120 : 30;
}

bool loadAutoStartSetting()
{
	char *path = obs_module_config_path("session-settings.json");
	if (!path) return false;
	obs_data_t *settings = obs_data_create_from_json_file(path);
	bfree(path);
	if (!settings) return false;
	const bool enabled = obs_data_get_bool(settings, "auto_start");
	obs_data_release(settings);
	return enabled;
}

void migrateLegacyPluginSettings()
{
	char *configPath = obs_module_config_path(nullptr);
	if (!configPath) return;
	const QString currentDirectory = QString::fromUtf8(configPath);
	bfree(configPath);

	const QString configRoot = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
	const QString legacyDirectory = QDir(configRoot).filePath(QStringLiteral("obs-studio/plugin_config/pixelion-live-captions"));
	if (QDir::cleanPath(currentDirectory) == QDir::cleanPath(legacyDirectory) || !QDir(legacyDirectory).exists()) return;
	if (!QDir().mkpath(currentDirectory)) return;

	const QStringList settingsFiles{QStringLiteral("session-settings.json"), QStringLiteral("transcript-settings.json"),
					QStringLiteral("caption-style.json"), QStringLiteral("pixelion-live-captions.log"),
					QStringLiteral("pixelion-live-captions.log.1")};
	for (const QString &fileName : settingsFiles) {
		const QString source = QDir(legacyDirectory).filePath(fileName);
		const QString destination = QDir(currentDirectory).filePath(fileName);
		if (!QFile::exists(destination) && QFile::exists(source) && !QFile::copy(source, destination))
			blog(LOG_WARNING, "Pixelion Captions: could not migrate %s", fileName.toUtf8().constData());
	}
}

void saveAutoStartSetting(bool enabled)
{
	char *directory = obs_module_config_path(nullptr);
	if (directory) { os_mkdirs(directory); bfree(directory); }
	char *path = obs_module_config_path("session-settings.json");
	if (!path) return;
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "auto_start", enabled);
	if (!obs_data_save_json(settings, path))
		blog(LOG_WARNING, "Pixelion Live Captions: could not save session settings");
	obs_data_release(settings);
	bfree(path);
}

void loadTranscriptSettings()
{
	transcriptDirectory = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(QStringLiteral("Pixelion Live Captions"));
	char *path = obs_module_config_path("transcript-settings.json");
	if (!path) return;
	obs_data_t *settings = obs_data_create_from_json_file(path);
	bfree(path);
	if (!settings) return;
	saveTranscriptLocally = !obs_data_has_user_value(settings, "save_locally") || obs_data_get_bool(settings, "save_locally");
	autoSaveTranscriptToPixelion = obs_data_get_bool(settings, "save_to_pixelion");
	transcriptDirectory = QString::fromUtf8(obs_data_get_string(settings, "directory")).trimmed();
	transcriptFilenamePattern = QString::fromUtf8(obs_data_get_string(settings, "filename_pattern")).trimmed();
	obs_data_release(settings);
	if (transcriptDirectory.isEmpty())
		transcriptDirectory = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(QStringLiteral("Pixelion Live Captions"));
	if (transcriptFilenamePattern.isEmpty())
		transcriptFilenamePattern = QStringLiteral("Pixelion Live Captions {date} {time}");
}

void saveTranscriptSettings()
{
	char *directory = obs_module_config_path(nullptr);
	if (directory) { os_mkdirs(directory); bfree(directory); }
	char *path = obs_module_config_path("transcript-settings.json");
	if (!path) return;
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "save_locally", saveTranscriptLocally);
	obs_data_set_bool(settings, "save_to_pixelion", autoSaveTranscriptToPixelion);
	obs_data_set_string(settings, "directory", transcriptDirectory.toUtf8().constData());
	obs_data_set_string(settings, "filename_pattern", transcriptFilenamePattern.toUtf8().constData());
	if (!obs_data_save_json(settings, path))
		blog(LOG_WARNING, "Pixelion Live Captions: could not save transcript settings");
	obs_data_release(settings);
	bfree(path);
}

QString transcriptSnapshot()
{
	QMutexLocker lock(&styleMutex);
	QString text = transcriptCommittedText;
	if (!transcriptDraftText.isEmpty()) {
		if (!text.isEmpty()) text += QLatin1Char('\n');
		text += transcriptDraftText;
	}
	return text;
}

QString formatCaptionTimestamp(qint64 milliseconds, QChar separator)
{
	const qint64 value = std::max<qint64>(0, milliseconds);
	const qint64 hours = value / 3600000;
	const qint64 minutes = (value / 60000) % 60;
	const qint64 seconds = (value / 1000) % 60;
	const qint64 millis = value % 1000;
	return QStringLiteral("%1:%2:%3%4%5")
		.arg(hours, 2, 10, QLatin1Char('0')).arg(minutes, 2, 10, QLatin1Char('0'))
		.arg(seconds, 2, 10, QLatin1Char('0')).arg(separator).arg(millis, 3, 10, QLatin1Char('0'));
}

QString formatSessionDuration(const QJsonObject &metadata)
{
	qint64 durationMs = static_cast<qint64>(metadata.value(QStringLiteral("duration_ms")).toDouble(-1));
	if (durationMs < 0) {
		const QDateTime started = QDateTime::fromString(metadata.value(QStringLiteral("started_at")).toString(), Qt::ISODateWithMs);
		const QDateTime ended = QDateTime::fromString(metadata.value(QStringLiteral("ended_at")).toString(), Qt::ISODateWithMs);
		if (started.isValid() && ended.isValid()) durationMs = started.msecsTo(ended);
	}
	if (durationMs < 0) return QStringLiteral("—");
	const qint64 seconds = durationMs / 1000;
	const qint64 hours = seconds / 3600;
	const qint64 minutes = (seconds / 60) % 60;
	const qint64 remainder = seconds % 60;
	return hours > 0
		? QStringLiteral("%1:%2:%3").arg(hours, 2, 10, QLatin1Char('0')).arg(minutes, 2, 10, QLatin1Char('0')).arg(remainder, 2, 10, QLatin1Char('0'))
		: QStringLiteral("%1:%2").arg(seconds / 60, 2, 10, QLatin1Char('0')).arg(remainder, 2, 10, QLatin1Char('0'));
}

void commitPendingCue(qint64 endMs)
{
	const QString text = pendingCueText.trimmed();
	if (!text.isEmpty()) {
		const qint64 start = std::max<qint64>(0, pendingCueStartMs);
		transcriptCues.append({start, std::max(start, endMs), text});
	}
	pendingCueText.clear();
	pendingCueStartMs = -1;
	lastCaptionDeltaMs = -1;
}

QJsonObject transcriptMetadataObject(bool completed)
{
	QJsonArray cues;
	for (const auto &cue : transcriptCues) {
		cues.append(QJsonObject{{QStringLiteral("start_ms"), static_cast<double>(cue.startMs)},
					{QStringLiteral("end_ms"), static_cast<double>(cue.endMs)},
					{QStringLiteral("text"), cue.text}});
	}
	if (pendingCueStartMs >= 0 && !pendingCueText.trimmed().isEmpty()) {
		const qint64 endMs = std::max(pendingCueStartMs, lastCaptionDeltaMs);
		cues.append(QJsonObject{{QStringLiteral("start_ms"), static_cast<double>(pendingCueStartMs)},
				       {QStringLiteral("end_ms"), static_cast<double>(endMs)},
				       {QStringLiteral("text"), pendingCueText.trimmed()}});
	}
	QJsonObject metadata{{QStringLiteral("version"), 1},
			      {QStringLiteral("session_id"), transcriptSessionId},
			      {QStringLiteral("job_id"), transcriptJobId},
			      {QStringLiteral("title"), transcriptSessionTitle},
			      {QStringLiteral("library_state"), transcriptLibraryState},
			      {QStringLiteral("library_url"), transcriptLibraryUrl},
			      {QStringLiteral("started_at"), QDateTime::fromMSecsSinceEpoch(transcriptSessionStartedAt).toString(Qt::ISODateWithMs)},
			      {QStringLiteral("mode"), transcriptMode},
			      {QStringLiteral("source_language"), transcriptSourceLanguage},
			      {QStringLiteral("target_language"), transcriptTargetLanguage},
			      {QStringLiteral("duration_ms"), static_cast<double>(transcriptSessionClock.isValid() ? transcriptSessionClock.elapsed() : 0)},
			      {QStringLiteral("status"), completed ? QStringLiteral("completed") : QStringLiteral("recoverable")},
			      {QStringLiteral("cues"), cues}};
	if (completed)
		metadata.insert(QStringLiteral("ended_at"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
	return metadata;
}

void writeTranscriptMetadata(bool completed = false)
{
	if (transcriptMetadataPath.isEmpty()) return;
	QSaveFile file(transcriptMetadataPath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		PixelionDebugLog::write("ERROR", QStringLiteral("Could not open transcript session metadata: %1").arg(transcriptMetadataPath));
		return;
	}
	const QByteArray json = QJsonDocument(transcriptMetadataObject(completed)).toJson(QJsonDocument::Indented);
	if (file.write(json) != json.size() || !file.commit())
		PixelionDebugLog::write("ERROR", QStringLiteral("Could not save transcript session metadata: %1").arg(transcriptMetadataPath));
}

void updateSavedTranscriptJobId(const QString &metadataPath, const QString &jobId, const QString &url)
{
	if (metadataPath.isEmpty() || jobId.isEmpty()) return;
	QFile input(metadataPath);
	if (!input.open(QIODevice::ReadOnly)) return;
	QJsonParseError error;
	QJsonDocument document = QJsonDocument::fromJson(input.readAll(), &error);
	input.close();
	if (error.error != QJsonParseError::NoError || !document.isObject()) return;
	QJsonObject object = document.object();
	object.insert(QStringLiteral("job_id"), jobId);
	if (!url.isEmpty()) {
		object.insert(QStringLiteral("library_state"), QStringLiteral("saved"));
		object.insert(QStringLiteral("library_url"), url);
	}
	QSaveFile output(metadataPath);
	if (output.open(QIODevice::WriteOnly | QIODevice::Text)) {
		const QByteArray json = QJsonDocument(object).toJson(QJsonDocument::Indented);
		if (output.write(json) == json.size()) output.commit();
	}
}

void writeTranscriptSnapshot(const QString &text)
{
	if (!saveTranscriptLocally || transcriptFilePath.isEmpty()) return;
	QSaveFile file(transcriptFilePath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		blog(LOG_WARNING, "Pixelion Live Captions: could not open local transcript checkpoint");
		PixelionDebugLog::write("ERROR", QStringLiteral("Could not open local transcript checkpoint: %1").arg(transcriptFilePath));
		return;
	}
	const QByteArray utf8 = text.toUtf8();
	if (file.write(utf8) != utf8.size() || !file.commit())
		{
			blog(LOG_WARNING, "Pixelion Live Captions: could not write local transcript checkpoint");
			PixelionDebugLog::write("ERROR", QStringLiteral("Could not write local transcript checkpoint: %1").arg(transcriptFilePath));
		}
}

void appendTranscriptDelta(const QString &delta)
{
	if (!saveTranscriptLocally || transcriptFilePath.isEmpty() || delta.isEmpty()) return;
	QFile file(transcriptFilePath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
		blog(LOG_WARNING, "Pixelion Live Captions: could not append local transcript checkpoint");
		PixelionDebugLog::write("ERROR", QStringLiteral("Could not append local transcript checkpoint: %1").arg(transcriptFilePath));
		return;
	}
	const QByteArray utf8 = delta.toUtf8();
	if (file.write(utf8) != utf8.size() || !file.flush())
		{
			blog(LOG_WARNING, "Pixelion Live Captions: could not flush local transcript checkpoint");
			PixelionDebugLog::write("ERROR", QStringLiteral("Could not flush local transcript checkpoint: %1").arg(transcriptFilePath));
		}
}

bool exportTranscriptFile(const QFileInfo &source, const QString &format, QWidget *parent)
{
	QFile input(source.absoluteFilePath());
	if (!input.open(QIODevice::ReadOnly | QIODevice::Text)) {
		QMessageBox::warning(parent, QStringLiteral("Export transcript"), QStringLiteral("Could not read the selected transcript."));
		return false;
	}
	const QString plainText = QString::fromUtf8(input.readAll());
	input.close();
	QString output;
	QString extension = format.toLower();
	if (extension == QStringLiteral("txt")) {
		output = plainText;
	} else {
		const QString metadataPath = source.absoluteFilePath() + QStringLiteral(".session.json");
		QFile metadataFile(metadataPath);
		QJsonParseError parseError;
		QJsonDocument metadata;
		if (metadataFile.open(QIODevice::ReadOnly)) {
			metadata = QJsonDocument::fromJson(metadataFile.readAll(), &parseError);
			metadataFile.close();
		}
		const QJsonArray cues = metadata.isObject() ? metadata.object().value(QStringLiteral("cues")).toArray() : QJsonArray();
		if (cues.isEmpty()) {
			QMessageBox::information(parent, QStringLiteral("Export transcript"),
						 QStringLiteral("This session has no saved caption timestamps. TXT export is available, but SRT/VTT cannot be created without timestamps."));
			return false;
		}
		if (extension == QStringLiteral("vtt")) output = QStringLiteral("WEBVTT\n\n");
		int cueNumber = 0;
		for (const auto &value : cues) {
			const QJsonObject cue = value.toObject();
			const qint64 startMs = static_cast<qint64>(cue.value(QStringLiteral("start_ms")).toDouble());
			const qint64 endMs = static_cast<qint64>(cue.value(QStringLiteral("end_ms")).toDouble());
			QString text = cue.value(QStringLiteral("text")).toString().replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(QLatin1Char('\r'), QLatin1Char('\n')).trimmed();
			text.replace(QStringLiteral("-->"), QStringLiteral("->"));
			if (text.isEmpty()) continue;
			if (extension == QStringLiteral("vtt")) {
				text.replace(QLatin1Char('&'), QStringLiteral("&amp;")).replace(QLatin1Char('<'), QStringLiteral("&lt;")).replace(QLatin1Char('>'), QStringLiteral("&gt;"));
				output += QStringLiteral("%1 --> %2\n%3\n\n").arg(formatCaptionTimestamp(startMs, QLatin1Char('.')), formatCaptionTimestamp(endMs, QLatin1Char('.')), text);
			} else {
				output += QStringLiteral("%1\n%2 --> %3\n%4\n\n")
						  .arg(++cueNumber).arg(formatCaptionTimestamp(startMs, QLatin1Char(',')), formatCaptionTimestamp(endMs, QLatin1Char(',')), text);
			}
		}
	}
	QString suggested = source.absoluteFilePath();
	if (suggested.endsWith(QStringLiteral(".partial.txt"))) suggested.chop(QStringLiteral(".partial.txt").size());
	else if (suggested.endsWith(QStringLiteral(".txt"))) suggested.chop(4);
	suggested += QLatin1Char('.') + extension;
	const QString target = QFileDialog::getSaveFileName(parent, QStringLiteral("Export transcript"), suggested,
										 QStringLiteral("%1 files (*.%2)").arg(extension.toUpper(), extension));
	if (target.isEmpty()) return false;
	QSaveFile outputFile(target);
	if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
		QMessageBox::warning(parent, QStringLiteral("Export transcript"), QStringLiteral("Could not create the export file."));
		return false;
	}
	const QByteArray bytes = output.toUtf8();
	if (outputFile.write(bytes) != bytes.size() || !outputFile.commit()) {
		QMessageBox::warning(parent, QStringLiteral("Export transcript"), QStringLiteral("Could not finish writing the export file."));
		return false;
	}
	return true;
}

bool saveSessionMetadata(const QString &path, const QJsonObject &object)
{
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
	const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
	return file.write(bytes) == bytes.size() && file.commit();
}

QJsonObject readSessionMetadata(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) return {};
	QJsonParseError error;
	const auto document = QJsonDocument::fromJson(file.readAll(), &error);
	return error.error == QJsonParseError::NoError && document.isObject() ? document.object() : QJsonObject();
}

using HistorySyncCallback = std::function<void(bool ok, const QString &message, const QString &url)>;

void syncTranscriptToPixelion(PixelionAuthService *auth, QNetworkAccessManager *network, const QFileInfo &transcript,
			      QWidget *parent, HistorySyncCallback completion)
{
	const QString metadataPath = transcript.absoluteFilePath() + QStringLiteral(".session.json");
	QJsonObject metadata = readSessionMetadata(metadataPath);
	const QString sessionId = metadata.value(QStringLiteral("session_id")).toString();
	const QJsonArray cues = metadata.value(QStringLiteral("cues")).toArray();
	if (!auth || !network || sessionId.isEmpty() || cues.isEmpty()) {
		completion(false, QStringLiteral("This transcript is missing session IDs or timestamped caption data. Only sessions created with the updated plugin can be synced."), {});
		return;
	}
	if (metadata.value(QStringLiteral("library_state")).toString() == QStringLiteral("saved")) {
		const QString savedUrl = metadata.value(QStringLiteral("library_url")).toString();
		if (!savedUrl.isEmpty()) {
			completion(true, QStringLiteral("This session is already synced to Pixelion."), savedUrl);
			return;
		}
	}
	QJsonArray segments;
	qint64 durationMs = 0;
	const QString mode = metadata.value(QStringLiteral("mode")).toString(QStringLiteral("transcribe"));
	const QString sourceLanguage = metadata.value(QStringLiteral("source_language")).toString(QStringLiteral("auto"));
	const QString targetLanguage = metadata.value(QStringLiteral("target_language")).toString();
	for (const auto &value : cues) {
		const QJsonObject cue = value.toObject();
		const qint64 startMs = std::max<qint64>(0, static_cast<qint64>(cue.value(QStringLiteral("start_ms")).toDouble()));
		const qint64 endMs = std::max(startMs, static_cast<qint64>(cue.value(QStringLiteral("end_ms")).toDouble()));
		const QString text = cue.value(QStringLiteral("text")).toString().trimmed();
		if (text.isEmpty()) continue;
		durationMs = std::max(durationMs, endMs);
		segments.append(QJsonObject{{QStringLiteral("start_ms"), static_cast<double>(startMs)},
					    {QStringLiteral("end_ms"), static_cast<double>(endMs)},
					    {QStringLiteral("text"), text},
					    {QStringLiteral("language"), mode == QStringLiteral("translate") ? targetLanguage : sourceLanguage}});
	}
	if (segments.isEmpty()) {
		completion(false, QStringLiteral("There are no captions to sync for this session."), {});
		return;
	}
	QString title = metadata.value(QStringLiteral("title")).toString();
	if (title.isEmpty()) title = transcript.completeBaseName().replace(QStringLiteral(".partial"), QString());
	QJsonObject body{{QStringLiteral("client_session_id"), sessionId},
			 {QStringLiteral("job_id"), metadata.value(QStringLiteral("job_id")).toString(sessionId)},
			 {QStringLiteral("platform"), QStringLiteral("obs")},
			 {QStringLiteral("title"), title},
			 {QStringLiteral("duration_seconds"), static_cast<double>(durationMs) / 1000.0},
			 {QStringLiteral("mode"), mode},
			 {QStringLiteral("source_language"), sourceLanguage},
			 {QStringLiteral("target_language"), mode == QStringLiteral("translate") ? QJsonValue(targetLanguage) : QJsonValue::Null},
			 {QStringLiteral("client_version"), QStringLiteral("0.1.0")},
			 {QStringLiteral("segments"), segments}};
	metadata.insert(QStringLiteral("library_state"), QStringLiteral("inflight"));
	saveSessionMetadata(metadataPath, metadata);
	QPointer<QWidget> parentGuard(parent);
	auth->withAccessToken([network, metadataPath, metadata, body, completion, parentGuard](bool ok, const QString &accessToken, const QString &error) mutable {
		if (!ok) {
			metadata.insert(QStringLiteral("library_state"), QStringLiteral("idle"));
			saveSessionMetadata(metadataPath, metadata);
			completion(false, error.isEmpty() ? QStringLiteral("Sign in to Pixelion before syncing this transcript.") : error, {});
			return;
		}
		QNetworkRequest request(QUrl(QStringLiteral(PIXELION_API_BASE_URL) + QStringLiteral("/obs/library-job")));
		request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
		request.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken.toUtf8());
		request.setTransferTimeout(30000);
		auto *reply = network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
		QObject::connect(reply, &QNetworkReply::finished, network, [reply, metadataPath, metadata, completion, parentGuard]() mutable {
			const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
			const QByteArray response = reply->readAll().left(64 * 1024);
			const QString networkError = reply->errorString();
			reply->deleteLater();
			QJsonParseError parseError;
			const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
			const QJsonObject result = document.object();
			const QString jobId = result.value(QStringLiteral("job_id")).toString();
			const QString url = result.value(QStringLiteral("url")).toString();
			if (status == 200 && parseError.error == QJsonParseError::NoError && url.startsWith(QStringLiteral("https://")) && !jobId.isEmpty()) {
				metadata.insert(QStringLiteral("job_id"), jobId);
				metadata.insert(QStringLiteral("library_state"), QStringLiteral("saved"));
				metadata.insert(QStringLiteral("library_url"), url);
				saveSessionMetadata(metadataPath, metadata);
				completion(true, QStringLiteral("Transcript synced to Pixelion."), url);
				return;
			}
			metadata.insert(QStringLiteral("library_state"), QStringLiteral("idle"));
			saveSessionMetadata(metadataPath, metadata);
			QString message = result.value(QStringLiteral("message")).toString();
			if (message.isEmpty()) message = networkError.isEmpty() ? QStringLiteral("Pixelion sync failed (HTTP %1). Check your connection and try again.").arg(status) : networkError;
			completion(false, message, {});
			if (parentGuard) PixelionDebugLog::write("ERROR", QStringLiteral("Manual Pixelion transcript sync failed: %1").arg(message));
		});
	});
}

void openTranscriptHistory(QWidget *parent, PixelionAuthService *auth, QNetworkAccessManager *network)
{
	QString directory = transcriptDirectory;
	if (directory.isEmpty())
		directory = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(QStringLiteral("Pixelion Live Captions"));
	QDialog dialog(parent);
	dialog.setWindowTitle(QStringLiteral("Transcription history"));
	dialog.resize(820, 460);
	auto *layout = new QVBoxLayout(&dialog);
	auto *table = new QTableWidget(&dialog);
	table->setColumnCount(4);
	table->setHorizontalHeaderLabels({QStringLiteral("Session"), QStringLiteral("Last saved"), QStringLiteral("Status"), QStringLiteral("Duration")});
	table->horizontalHeaderItem(3)->setToolTip(QStringLiteral("Time Pixelion Live was connected and listening."));
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setSelectionMode(QAbstractItemView::SingleSelection);
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
	const auto files = QDir(directory).entryInfoList({QStringLiteral("*.txt")}, QDir::Files, QDir::Time);
	table->setRowCount(files.size());
	for (int row = 0; row < files.size(); ++row) {
		const QFileInfo &info = files.at(row);
		QString title = info.completeBaseName();
		title.remove(QStringLiteral(".partial"));
		auto *titleItem = new QTableWidgetItem(title);
		titleItem->setData(Qt::UserRole, info.absoluteFilePath());
		table->setItem(row, 0, titleItem);
		table->setItem(row, 1, new QTableWidgetItem(info.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
		QString rowStatus = info.fileName().contains(QStringLiteral(".partial.")) ? QStringLiteral("Recoverable") : QStringLiteral("Completed");
		QString duration = QStringLiteral("—");
		QFile metadataFile(info.absoluteFilePath() + QStringLiteral(".session.json"));
		if (metadataFile.open(QIODevice::ReadOnly)) {
			const auto metadata = QJsonDocument::fromJson(metadataFile.readAll()).object();
			const QString sessionId = metadata.value(QStringLiteral("session_id")).toString();
			const QString jobId = metadata.value(QStringLiteral("job_id")).toString();
			if (metadata.value(QStringLiteral("library_state")).toString() == QStringLiteral("saved")) rowStatus = QStringLiteral("Synced");
			duration = formatSessionDuration(metadata);
			if (!sessionId.isEmpty() || !jobId.isEmpty())
				table->item(row, 0)->setToolTip(QStringLiteral("Session ID: %1\nJob ID: %2").arg(sessionId, jobId));
		}
		table->setItem(row, 2, new QTableWidgetItem(rowStatus));
		table->setItem(row, 3, new QTableWidgetItem(duration));
	}
	layout->addWidget(table);
	auto *actions = new QHBoxLayout;
	auto *format = new QComboBox(&dialog);
	format->addItems({QStringLiteral("TXT"), QStringLiteral("SRT"), QStringLiteral("VTT")});
	auto *open = new QPushButton(QStringLiteral("Open"), &dialog);
	auto *exportButton = new QPushButton(QStringLiteral("Export"), &dialog);
	auto *renameButton = new QPushButton(QStringLiteral("Rename"), &dialog);
	auto *deleteButton = new QPushButton(QStringLiteral("Delete"), &dialog);
	auto *syncButton = new QPushButton(QStringLiteral("Sync to Pixelion"), &dialog);
	auto *close = new QPushButton(QStringLiteral("Close"), &dialog);
	open->setEnabled(false);
	exportButton->setEnabled(false);
	renameButton->setEnabled(false);
	deleteButton->setEnabled(false);
	syncButton->setEnabled(false);
	actions->addWidget(format);
	actions->addWidget(open);
	actions->addWidget(exportButton);
	actions->addWidget(renameButton);
	actions->addWidget(deleteButton);
	actions->addWidget(syncButton);
	actions->addStretch(1);
	actions->addWidget(close);
	layout->addLayout(actions);
	QObject::connect(table, &QTableWidget::itemSelectionChanged, &dialog, [table, open, exportButton, renameButton, deleteButton, syncButton] {
		const bool selected = table->currentRow() >= 0;
		open->setEnabled(selected);
		exportButton->setEnabled(selected);
		const int row = table->currentRow();
		const QString path = selected ? table->item(row, 0)->data(Qt::UserRole).toString() : QString();
		const bool active = selected && path == transcriptFilePath;
		renameButton->setEnabled(selected && !active);
		deleteButton->setEnabled(selected && !active);
		const auto metadata = readSessionMetadata(path + QStringLiteral(".session.json"));
		const bool synced = metadata.value(QStringLiteral("library_state")).toString() == QStringLiteral("saved");
		syncButton->setText(synced ? QStringLiteral("Open in Pixelion") : QStringLiteral("Sync to Pixelion"));
		syncButton->setEnabled(selected && !metadata.value(QStringLiteral("session_id")).toString().isEmpty());
	});
	QObject::connect(open, &QPushButton::clicked, &dialog, [table] {
		const int row = table->currentRow();
		if (row >= 0) QDesktopServices::openUrl(QUrl::fromLocalFile(table->item(row, 0)->data(Qt::UserRole).toString()));
	});
	QObject::connect(exportButton, &QPushButton::clicked, &dialog, [table, format, &dialog] {
		const int row = table->currentRow();
		if (row >= 0 && exportTranscriptFile(QFileInfo(table->item(row, 0)->data(Qt::UserRole).toString()), format->currentText().toLower(), &dialog))
			QMessageBox::information(&dialog, QStringLiteral("Export transcript"), QStringLiteral("Transcript exported successfully."));
	});
	QObject::connect(renameButton, &QPushButton::clicked, &dialog, [table, &dialog] {
		const int row = table->currentRow();
		if (row < 0) return;
		const QString oldPath = table->item(row, 0)->data(Qt::UserRole).toString();
		if (oldPath == transcriptFilePath) { QMessageBox::information(&dialog, QStringLiteral("Rename session"), QStringLiteral("Stop this session before renaming its transcript.")); return; }
		QFileInfo oldInfo(oldPath);
		QString oldBase = oldInfo.fileName();
		if (oldBase.endsWith(QStringLiteral(".partial.txt"))) oldBase.chop(QStringLiteral(".partial.txt").size());
		else if (oldBase.endsWith(QStringLiteral(".txt"))) oldBase.chop(4);
		bool accepted = false;
		QString name = QInputDialog::getText(&dialog, QStringLiteral("Rename session"), QStringLiteral("Session name:"), QLineEdit::Normal, oldBase, &accepted).trimmed();
		if (!accepted) return;
		for (QChar &character : name)
			if (character.unicode() < 32 || QStringLiteral("<>:\"/\\|?*").contains(character)) character = QLatin1Char('-');
		while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) name.chop(1);
		if (name.isEmpty()) { QMessageBox::warning(&dialog, QStringLiteral("Rename session"), QStringLiteral("Enter a valid session name.")); return; }
		if (name.size() > 100) name = name.left(100).trimmed();
		const QString suffix = oldPath.endsWith(QStringLiteral(".partial.txt")) ? QStringLiteral(".partial.txt") : QStringLiteral(".txt");
		const QString newPath = oldInfo.dir().filePath(name + suffix);
		const QString oldMetadata = oldPath + QStringLiteral(".session.json");
		const QString newMetadata = newPath + QStringLiteral(".session.json");
		if (newPath == oldPath) return;
		if (QFileInfo::exists(newPath) || QFileInfo::exists(newMetadata)) { QMessageBox::warning(&dialog, QStringLiteral("Rename session"), QStringLiteral("A session with that name already exists.")); return; }
		if (!QFile::rename(oldPath, newPath)) { QMessageBox::warning(&dialog, QStringLiteral("Rename session"), QStringLiteral("Could not rename the transcript file.")); return; }
		if (QFileInfo::exists(oldMetadata) && !QFile::rename(oldMetadata, newMetadata)) {
			QFile::rename(newPath, oldPath);
			QMessageBox::warning(&dialog, QStringLiteral("Rename session"), QStringLiteral("Could not rename the session metadata."));
			return;
		}
		QJsonObject metadata = readSessionMetadata(newMetadata);
		if (!metadata.isEmpty()) { metadata.insert(QStringLiteral("title"), name); saveSessionMetadata(newMetadata, metadata); }
		table->item(row, 0)->setText(name);
		table->item(row, 0)->setData(Qt::UserRole, newPath);
		table->item(row, 1)->setText(QFileInfo(newPath).lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
	});
	QObject::connect(deleteButton, &QPushButton::clicked, &dialog, [table, &dialog] {
		const int row = table->currentRow();
		if (row < 0) return;
		const QString path = table->item(row, 0)->data(Qt::UserRole).toString();
		if (path == transcriptFilePath) { QMessageBox::information(&dialog, QStringLiteral("Delete session"), QStringLiteral("Stop this session before deleting its transcript.")); return; }
		const auto metadata = readSessionMetadata(path + QStringLiteral(".session.json"));
		const bool synced = metadata.value(QStringLiteral("library_state")).toString() == QStringLiteral("saved");
		const QString prompt = synced
			? QStringLiteral("Delete the local transcript and its metadata? The Pixelion Library copy will remain.")
			: QStringLiteral("Permanently delete this local transcript and its session metadata?");
		if (QMessageBox::question(&dialog, QStringLiteral("Delete session"), prompt, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
		const QString metadataPath = path + QStringLiteral(".session.json");
		if (!QFile::remove(path) && QFileInfo::exists(path)) {
			QMessageBox::warning(&dialog, QStringLiteral("Delete session"), QStringLiteral("Could not delete the transcript file."));
			return;
		}
		if (!QFile::remove(metadataPath) && QFileInfo::exists(metadataPath)) {
			QMessageBox::warning(&dialog, QStringLiteral("Delete session"), QStringLiteral("The transcript was deleted, but its session metadata could not be removed."));
		}
		table->removeRow(row);
	});
	QObject::connect(syncButton, &QPushButton::clicked, &dialog, [table, syncButton, auth, network, &dialog] {
		const int row = table->currentRow();
		if (row < 0) return;
		const QString path = table->item(row, 0)->data(Qt::UserRole).toString();
		if (path.endsWith(QStringLiteral(".partial.txt"))) { QMessageBox::information(&dialog, QStringLiteral("Sync to Pixelion"), QStringLiteral("Stop the session before syncing its transcript.")); return; }
		const auto metadata = readSessionMetadata(path + QStringLiteral(".session.json"));
		if (metadata.value(QStringLiteral("library_state")).toString() == QStringLiteral("saved")) {
			const QString url = metadata.value(QStringLiteral("library_url")).toString();
			if (url.startsWith(QStringLiteral("https://"))) QDesktopServices::openUrl(QUrl(url));
			else QMessageBox::information(&dialog, QStringLiteral("Pixelion Library"), QStringLiteral("This session is synced, but Pixelion did not return a library link."));
			return;
		}
		if (!auth || !auth->isSignedIn()) { QMessageBox::information(&dialog, QStringLiteral("Sync to Pixelion"), QStringLiteral("Sign in to Pixelion in the dock, then select this session and try again.")); return; }
		syncButton->setEnabled(false);
		syncButton->setText(QStringLiteral("Syncing…"));
		QPointer<QDialog> dialogGuard(&dialog);
		QPointer<QTableWidget> tableGuard(table);
		syncTranscriptToPixelion(auth, network, QFileInfo(path), &dialog, [dialogGuard, tableGuard, row, syncButton](bool ok, const QString &message, const QString &url) {
			if (!dialogGuard) return;
			if (tableGuard && row < tableGuard->rowCount()) {
				if (ok && !url.isEmpty()) {
					tableGuard->item(row, 2)->setText(QStringLiteral("Synced"));
				}
			}
			if (syncButton) { syncButton->setEnabled(true); syncButton->setText(ok ? QStringLiteral("Open in Pixelion") : QStringLiteral("Sync to Pixelion")); }
			QMessageBox::information(dialogGuard, ok ? QStringLiteral("Pixelion Library") : QStringLiteral("Sync to Pixelion"), message);
		});
	});
	QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
	dialog.exec();
}

void beginLocalTranscript()
{
	transcriptSessionClock.invalidate();
	{
		QMutexLocker lock(&styleMutex);
		transcriptCommittedText.clear();
		transcriptDraftText.clear();
		liveCaptionText.clear();
		captionCommittedDisplayText.clear();
		captionDraftDisplayText.clear();
		++styleRevision;
	}
	transcriptFilePath.clear();
	transcriptMetadataPath.clear();
	transcriptCues.clear();
	transcriptSessionTitle.clear();
	transcriptLibraryState = QStringLiteral("idle");
	transcriptLibraryUrl.clear();
	pendingCueText.clear();
	pendingCueStartMs = -1;
	lastCaptionDeltaMs = -1;
	if (!saveTranscriptLocally) return;
	if (transcriptDirectory.isEmpty())
		transcriptDirectory = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(QStringLiteral("Pixelion Live Captions"));
	QDir directory(transcriptDirectory);
	if (!directory.exists() && !QDir().mkpath(transcriptDirectory)) {
		blog(LOG_WARNING, "Pixelion Live Captions: could not create transcript folder");
		PixelionDebugLog::write("ERROR", QStringLiteral("Could not create transcript folder: %1").arg(transcriptDirectory));
		return;
	}
	const QDateTime now = QDateTime::currentDateTime();
	QString base = transcriptFilenamePattern;
	base.replace(QStringLiteral("{date}"), now.toString(QStringLiteral("yyyy-MM-dd")), Qt::CaseInsensitive);
	base.replace(QStringLiteral("{time}"), now.toString(QStringLiteral("HH-mm-ss")), Qt::CaseInsensitive);
	base.replace(QStringLiteral("{mode}"), transcriptMode, Qt::CaseInsensitive);
	for (QChar &character : base) {
		if (character.unicode() < 32 || QStringLiteral("<>:\"/\\|?*").contains(character))
			character = QLatin1Char('-');
	}
	base = base.trimmed();
	while (base.endsWith(QLatin1Char('.')) || base.endsWith(QLatin1Char(' '))) base.chop(1);
	if (base.isEmpty()) base = QStringLiteral("Pixelion Live Captions %1").arg(now.toString(QStringLiteral("yyyy-MM-dd HH-mm-ss")));
	if (base.size() > 100) base = base.left(100).trimmed();
	QString candidate = base;
	QString path = directory.filePath(candidate + QStringLiteral(".partial.txt"));
	int suffix = 2;
	while (QFileInfo::exists(path) || QFileInfo::exists(directory.filePath(candidate + QStringLiteral(".txt")))) {
		candidate = QStringLiteral("%1 (%2)").arg(base).arg(suffix++);
		path = directory.filePath(candidate + QStringLiteral(".partial.txt"));
	}
	transcriptSessionTitle = candidate;
	transcriptFilePath = path;
	transcriptMetadataPath = path + QStringLiteral(".session.json");
	writeTranscriptSnapshot(QString());
	writeTranscriptMetadata(false);
	PixelionDebugLog::write("INFO", QStringLiteral("Started local transcript checkpoint: %1").arg(transcriptFilePath));
}

QString finalizeLocalTranscript()
{
	if (pendingCueStartMs >= 0 && !pendingCueText.trimmed().isEmpty()) {
		const qint64 endMs = transcriptSessionClock.isValid() ? transcriptSessionClock.elapsed() : std::max<qint64>(pendingCueStartMs, 0);
		commitPendingCue(endMs);
	}
	const QString text = transcriptSnapshot();
	if (transcriptFilePath.isEmpty()) return text;
	writeTranscriptSnapshot(text);
	if (text.trimmed().isEmpty()) {
		QFile::remove(transcriptFilePath);
		QFile::remove(transcriptMetadataPath);
		transcriptFilePath.clear();
		transcriptMetadataPath.clear();
		return text;
	}
	writeTranscriptMetadata(true);
	const QString finalPath = transcriptFilePath;
	QString completedPath = finalPath;
	completedPath.replace(QStringLiteral(".partial.txt"), QStringLiteral(".txt"));
	const QString finalMetadataPath = completedPath + QStringLiteral(".session.json");
	if (!QFile::rename(finalPath, completedPath)) {
		blog(LOG_WARNING, "Pixelion Live Captions: transcript saved as a recoverable partial file");
		PixelionDebugLog::write("WARNING", QStringLiteral("Transcript could not be renamed after the session; recoverable file remains at %1").arg(finalPath));
	} else {
		QFile::rename(transcriptMetadataPath, finalMetadataPath);
		transcriptMetadataPath = finalMetadataPath;
		PixelionDebugLog::write("INFO", QStringLiteral("Saved completed local transcript: %1").arg(completedPath));
	}
	transcriptFilePath.clear();
	return text;
}

void saveTranscriptToPixelion(PixelionAuthService *auth, QNetworkAccessManager *network, QPointer<QLabel> status,
				     const QString &sessionId, const QString &mode, const QString &sourceLanguage,
				     const QString &targetLanguage, qint64 startedAtMs, const QString &text,
				     const QString &metadataPath)
{
	if (!auth || !network || sessionId.isEmpty() || text.trimmed().isEmpty()) return;
	PixelionDebugLog::write("INFO", QStringLiteral("Starting Pixelion Library upload for session %1").arg(sessionId));
	QPointer<QNetworkAccessManager> networkGuard(network);
	const qint64 durationMs = std::max<qint64>(1000, QDateTime::currentMSecsSinceEpoch() - startedAtMs);
	QJsonArray segments;
	const QJsonObject storedMetadata = readSessionMetadata(metadataPath);
	for (const auto &value : storedMetadata.value(QStringLiteral("cues")).toArray()) {
		const QJsonObject cue = value.toObject();
		const QString cueText = cue.value(QStringLiteral("text")).toString().trimmed();
		if (cueText.isEmpty()) continue;
		const qint64 startMs = std::max<qint64>(0, static_cast<qint64>(cue.value(QStringLiteral("start_ms")).toDouble()));
		const qint64 endMs = std::max(startMs, static_cast<qint64>(cue.value(QStringLiteral("end_ms")).toDouble()));
		segments.append(QJsonObject{{QStringLiteral("start_ms"), static_cast<double>(startMs)},
					    {QStringLiteral("end_ms"), static_cast<double>(endMs)},
					    {QStringLiteral("text"), cueText},
					    {QStringLiteral("language"), mode == QStringLiteral("translate") ? targetLanguage : sourceLanguage}});
	}
	// Older local records may have no cue metadata. Keep them uploadable as one
	// untimed body segment instead of inventing evenly spaced timestamps.
	if (segments.isEmpty()) {
		segments.append(QJsonObject{{QStringLiteral("start_ms"), 0},
					    {QStringLiteral("end_ms"), static_cast<double>(durationMs)},
					    {QStringLiteral("text"), text.trimmed()},
					    {QStringLiteral("language"), mode == QStringLiteral("translate") ? targetLanguage : sourceLanguage}});
	}
	if (segments.isEmpty()) return;
	const QString now = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
	const QString title = storedMetadata.value(QStringLiteral("title")).toString(QStringLiteral("Pixelion Live Captions %1").arg(now));
	QJsonObject body{{QStringLiteral("client_session_id"), sessionId},
			 {QStringLiteral("job_id"), sessionId},
			 {QStringLiteral("platform"), QStringLiteral("obs")},
			 {QStringLiteral("title"), title},
			 {QStringLiteral("duration_seconds"), static_cast<double>(durationMs / 1000)},
			 {QStringLiteral("mode"), mode},
			 {QStringLiteral("source_language"), sourceLanguage.isEmpty() ? QStringLiteral("auto") : sourceLanguage},
			 {QStringLiteral("target_language"), mode == QStringLiteral("translate") ? QJsonValue(targetLanguage) : QJsonValue::Null},
			 {QStringLiteral("client_version"), QStringLiteral("0.1.0")},
			 {QStringLiteral("segments"), segments}};
	auth->withAccessToken([networkGuard, status, body, metadataPath, sessionId](bool ok, const QString &accessToken, const QString &error) {
		if (!ok) {
			PixelionDebugLog::write("ERROR", QStringLiteral("Pixelion Library upload could not get an access token: %1").arg(error));
			if (status) status->setText(QStringLiteral("Transcript saved locally. Pixelion Library save failed: %1").arg(error));
			return;
		}
		if (!networkGuard) return;
		QNetworkRequest request(QUrl(QStringLiteral(PIXELION_API_BASE_URL) + QStringLiteral("/obs/library-job")));
		request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
		request.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken.toUtf8());
		request.setTransferTimeout(30000);
		auto *reply = networkGuard->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
		QObject::connect(reply, &QNetworkReply::finished, networkGuard, [reply, status, metadataPath, sessionId] {
			const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
			const QByteArray responseBody = reply->readAll().left(64 * 1024);
			const QString networkError = reply->errorString();
			reply->deleteLater();
			if (statusCode == 200) {
				QJsonParseError parsedError;
				const QJsonDocument saved = QJsonDocument::fromJson(responseBody, &parsedError);
				const QJsonObject savedObject = saved.object();
				const QString returnedJobId = parsedError.error == QJsonParseError::NoError && saved.isObject()
					? savedObject.value(QStringLiteral("job_id")).toString() : QString();
				const QString savedUrl = savedObject.value(QStringLiteral("url")).toString();
				if (!returnedJobId.isEmpty()) {
					updateSavedTranscriptJobId(metadataPath, returnedJobId, savedUrl);
					if (transcriptSessionId == sessionId) transcriptJobId = returnedJobId;
					PixelionDebugLog::write("INFO", QStringLiteral("Pixelion Library job linked to session %1 as job %2").arg(sessionId, returnedJobId));
				}
				PixelionDebugLog::write("INFO", QStringLiteral("Pixelion Library upload completed successfully (HTTP 200)"));
				if (status) status->setText(QStringLiteral("Transcript saved locally and to your Pixelion Library."));
			} else {
				blog(LOG_WARNING, "Pixelion Live Captions: Pixelion Library save failed (HTTP %d)", statusCode);
				QJsonParseError parseError;
				const QJsonDocument parsed = QJsonDocument::fromJson(responseBody, &parseError);
				const QString detail = parseError.error == QJsonParseError::NoError
					? parsed.object().value(QStringLiteral("message")).toString() : networkError;
				PixelionDebugLog::write("ERROR", QStringLiteral("Pixelion Library upload failed (HTTP %1): %2")
								.arg(statusCode).arg(detail.isEmpty() ? networkError : detail));
				if (status) status->setText(detail.isEmpty()
					? QStringLiteral("Transcript saved locally. Automatic Pixelion Library upload did not complete.")
					: QStringLiteral("Transcript saved locally. Pixelion Library save failed: %1").arg(detail));
			}
		});
	});
}

struct CaptionSource {
	gs_texture_t *texture = nullptr;
	uint64_t textureRevision = 0;
};

struct StyleSnapshot {
	QFont font;
	QColor textColor;
	QColor backgroundColor;
	uint32_t fontSize;
	uint32_t backgroundOpacity;
	uint32_t textAlignment;
	uint32_t visibleLines;
	uint32_t backgroundMode;
	uint32_t padding;
	QString captionText;
	uint64_t revision;
};

StyleSnapshot currentStyle()
{
	QMutexLocker lock(&styleMutex);
	return {captionFont, captionTextColor, captionBackgroundColor, captionFontSize,
		captionBackgroundOpacity, captionTextAlignment, captionVisibleLines, captionBackgroundMode, captionPadding, liveCaptionText, styleRevision};
}

QString visibleCaptionTail(const QString &text)
{
	if (text.isEmpty()) return {};
	QFont font = captionFont;
	font.setPixelSize(static_cast<int>(captionFontSize));
	QStringList wrappedLines;
	const QStringList paragraphs = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
	for (const QString &paragraph : paragraphs) {
		if (paragraph.isEmpty()) {
			wrappedLines.append(QString());
			continue;
		}
		QTextLayout layout(paragraph, font);
		QTextOption option;
		option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
		layout.setTextOption(option);
		layout.beginLayout();
		while (true) {
			QTextLine line = layout.createLine();
			if (!line.isValid()) break;
			line.setLineWidth(static_cast<qreal>(kOverlayWidth - 2 * captionPadding));
			wrappedLines.append(paragraph.mid(line.textStart(), line.textLength()));
		}
		layout.endLayout();
	}
	const int visible = static_cast<int>(captionVisibleLines);
	if (wrappedLines.size() > visible)
		wrappedLines = wrappedLines.mid(wrappedLines.size() - visible);
	return wrappedLines.join(QLatin1Char('\n'));
}

void rebuildCaptionDisplay()
{
	QString text = captionCommittedDisplayText;
	if (!captionDraftDisplayText.isEmpty()) {
		if (!text.isEmpty()) text += QLatin1Char('\n');
		text += captionDraftDisplayText;
	}
	liveCaptionText = visibleCaptionTail(text);
}

void updateCaptionText(const QString &text, bool final)
{
	const qint64 nowMs = transcriptSessionClock.isValid() ? transcriptSessionClock.elapsed() : 0;
	if (final) {
		if (!text.trimmed().isEmpty()) {
			if (pendingCueStartMs < 0) pendingCueStartMs = nowMs;
			// Provider final text is the corrected full utterance, so replace its deltas.
			pendingCueText = text.trimmed();
			commitPendingCue(nowMs);
		}
	} else if (!text.isEmpty()) {
		if (pendingCueStartMs >= 0 && lastCaptionDeltaMs >= 0 && nowMs - lastCaptionDeltaMs >= 2000)
			commitPendingCue(lastCaptionDeltaMs);
		if (pendingCueStartMs < 0) pendingCueStartMs = nowMs;
		pendingCueText += text;
		lastCaptionDeltaMs = nowMs;
	}
	QString snapshot;
	{
		QMutexLocker lock(&styleMutex);
		if (final) {
			if (!text.trimmed().isEmpty()) {
				if (!captionCommittedDisplayText.isEmpty()) captionCommittedDisplayText += QLatin1Char('\n');
				captionCommittedDisplayText += text.trimmed();
			}
			captionDraftDisplayText.clear();
			if (!text.trimmed().isEmpty()) {
				if (!transcriptCommittedText.isEmpty()) transcriptCommittedText += QLatin1Char('\n');
				transcriptCommittedText += text.trimmed();
			}
			transcriptDraftText.clear();
		} else {
			captionDraftDisplayText += text;
			transcriptDraftText += text;
		}
		rebuildCaptionDisplay();
		snapshot = transcriptCommittedText;
		if (!transcriptDraftText.isEmpty()) {
			if (!snapshot.isEmpty()) snapshot += QLatin1Char('\n');
			snapshot += transcriptDraftText;
		}
		++styleRevision;
	}
	// Append streaming deltas cheaply; atomically reconcile provider corrections on final revisions.
	if (final) writeTranscriptSnapshot(snapshot);
	else appendTranscriptDelta(text);
	writeTranscriptMetadata(false);
}

void saveStyle()
{
	const auto style = currentStyle();
	char *directory = obs_module_config_path(nullptr);
	if (directory) {
		os_mkdirs(directory);
		bfree(directory);
	}
	char *path = obs_module_config_path("caption-style.json");
	if (!path)
		return;
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "font_family", style.font.family().toUtf8().constData());
	obs_data_set_string(settings, "text_color", style.textColor.name(QColor::HexRgb).toUtf8().constData());
	obs_data_set_string(settings, "background_color", style.backgroundColor.name(QColor::HexRgb).toUtf8().constData());
	obs_data_set_int(settings, "font_size", style.fontSize);
	obs_data_set_int(settings, "background_opacity", style.backgroundOpacity);
	obs_data_set_int(settings, "text_alignment", style.textAlignment);
	obs_data_set_int(settings, "visible_lines", style.visibleLines);
	obs_data_set_int(settings, "background_mode", style.backgroundMode);
	obs_data_set_int(settings, "padding", style.padding);
	if (!obs_data_save_json(settings, path))
		blog(LOG_WARNING, "Pixelion Live Captions: could not save caption style settings");
	obs_data_release(settings);
	bfree(path);
}

void loadStyle()
{
	char *path = obs_module_config_path("caption-style.json");
	if (!path)
		return;
	obs_data_t *settings = obs_data_create_from_json_file(path);
	bfree(path);
	if (!settings)
		return;
	if (obs_data_has_user_value(settings, "font_family")) {
		const QString family = QString::fromUtf8(obs_data_get_string(settings, "font_family"));
		if (!family.isEmpty())
			captionFont = QFont(family);
	}
	if (obs_data_has_user_value(settings, "text_color")) {
		const QColor color(QString::fromUtf8(obs_data_get_string(settings, "text_color")));
		if (color.isValid()) captionTextColor = color;
	}
	if (obs_data_has_user_value(settings, "background_color")) {
		const QColor color(QString::fromUtf8(obs_data_get_string(settings, "background_color")));
		if (color.isValid()) captionBackgroundColor = color;
	}
	if (obs_data_has_user_value(settings, "font_size"))
		captionFontSize = std::clamp<int64_t>(obs_data_get_int(settings, "font_size"), 8, 120);
	if (obs_data_has_user_value(settings, "background_opacity"))
		captionBackgroundOpacity = std::clamp<int64_t>(obs_data_get_int(settings, "background_opacity"), 0, 100);
	if (obs_data_has_user_value(settings, "text_alignment"))
		captionTextAlignment = std::clamp<int64_t>(obs_data_get_int(settings, "text_alignment"), 0, 2);
	if (obs_data_has_user_value(settings, "visible_lines"))
		captionVisibleLines = std::clamp<int64_t>(obs_data_get_int(settings, "visible_lines"), 1, 4);
	if (obs_data_has_user_value(settings, "background_mode"))
		captionBackgroundMode = std::clamp<int64_t>(obs_data_get_int(settings, "background_mode"), 0, 1);
	if (obs_data_has_user_value(settings, "padding"))
		captionPadding = std::clamp<int64_t>(obs_data_get_int(settings, "padding"), 0, 200);
	obs_data_release(settings);
}

void setCaptionFont(const QFont &font)
{
	{
		QMutexLocker lock(&styleMutex);
		captionFont = font;
		rebuildCaptionDisplay();
		++styleRevision;
	}
	saveStyle();
}

void setCaptionFontSize(int size)
{
	{
		QMutexLocker lock(&styleMutex);
		captionFontSize = static_cast<uint32_t>(size);
		rebuildCaptionDisplay();
		++styleRevision;
	}
	saveStyle();
}

enum class ColorTarget { Text, Background };

void setCaptionColor(ColorTarget target, const QColor &color)
{
	{
		QMutexLocker lock(&styleMutex);
		switch (target) {
		case ColorTarget::Text: captionTextColor = color; break;
		case ColorTarget::Background: captionBackgroundColor = color; break;
		}
		++styleRevision;
	}
	saveStyle();
}

void setCaptionOpacity(int opacity)
{
	{
		QMutexLocker lock(&styleMutex);
		captionBackgroundOpacity = static_cast<uint32_t>(opacity);
		++styleRevision;
	}
	saveStyle();
}

void setCaptionAlignment(int alignment)
{
	{
		QMutexLocker lock(&styleMutex);
		captionTextAlignment = static_cast<uint32_t>(alignment);
		++styleRevision;
	}
	saveStyle();
}

void setCaptionVisibleLines(int lines)
{
	{
		QMutexLocker lock(&styleMutex);
		captionVisibleLines = static_cast<uint32_t>(lines);
		rebuildCaptionDisplay();
		++styleRevision;
	}
	saveStyle();
}

void setCaptionBackgroundMode(int mode)
{
	{
		QMutexLocker lock(&styleMutex);
		captionBackgroundMode = static_cast<uint32_t>(mode);
		++styleRevision;
	}
	saveStyle();
}

void setCaptionPadding(int padding)
{
	{
		QMutexLocker lock(&styleMutex);
		captionPadding = static_cast<uint32_t>(padding);
		rebuildCaptionDisplay();
		++styleRevision;
	}
	saveStyle();
}

void resetCaptionStyle()
{
	{
		QMutexLocker lock(&styleMutex);
		captionFont = QFont(QStringLiteral("Segoe UI"));
		captionTextColor = QColor(Qt::white);
		captionBackgroundColor = QColor(Qt::black);
		captionFontSize = 32;
		captionBackgroundOpacity = 70;
		captionTextAlignment = 1;
		captionVisibleLines = 2;
		captionBackgroundMode = 0;
		captionPadding = 36;
		rebuildCaptionDisplay();
		++styleRevision;
	}
	saveStyle();
}

void styleButton(QPushButton *button, const QColor &color)
{
	button->setText(QString());
	const auto foreground = color.lightness() > 140 ? QStringLiteral("#17191f") : QStringLiteral("#f3f4f6");
	button->setStyleSheet(QStringLiteral(R"(
		QPushButton { background-color: %1; color: %2; }
		QPushButton:hover { background-color: %3; }
		QPushButton:pressed { background-color: %4; }
	)").arg(color.name(QColor::HexArgb), foreground, color.lighter(118).name(QColor::HexArgb), color.darker(118).name(QColor::HexArgb)));
}

QPushButton *makeColorControl(const QString &title, ColorTarget target, QWidget *parent)
{
	auto *button = new QPushButton(parent);
	button->setMinimumHeight(30);
	button->setAccessibleName(title);
	const auto initial = currentStyle();
	QColor initialColor = initial.textColor;
	if (target == ColorTarget::Background)
		initialColor = initial.backgroundColor;
	styleButton(button, initialColor);
	QObject::connect(button, &QPushButton::clicked, button, [button, title, target] {
		const auto style = currentStyle();
		QColor initialColor = style.textColor;
		if (target == ColorTarget::Background)
			initialColor = style.backgroundColor;
		const auto chosen = QColorDialog::getColor(initialColor, button, title);
		if (!chosen.isValid())
			return;
		setCaptionColor(target, chosen);
		styleButton(button, chosen);
	});
	return button;
}

const char *captionSourceName(void *)
{
	return "Pixelion Captions";
}

void *captionSourceCreate(obs_data_t *, obs_source_t *)
{
	return new CaptionSource;
}

void captionSourceDestroy(void *data)
{
	auto *source = static_cast<CaptionSource *>(data);
	obs_enter_graphics();
	if (source->texture)
		gs_texture_destroy(source->texture);
	obs_leave_graphics();
	delete source;
}

uint32_t captionSourceWidth(void *)
{
	return kOverlayWidth;
}

uint32_t captionSourceHeight(void *)
{
	return kOverlayHeight;
}

void captionSourceRender(void *data, gs_effect_t *effect)
{
	auto *source = static_cast<CaptionSource *>(data);
	const auto style = currentStyle();
	if (style.revision != source->textureRevision || !source->texture) {
		QImage canvas(static_cast<int>(kOverlayWidth), static_cast<int>(kOverlayHeight), QImage::Format_ARGB32_Premultiplied);
		canvas.fill(Qt::transparent);
		if (style.backgroundMode == 0) {
			QColor background = style.backgroundColor;
			background.setAlpha(static_cast<int>(style.backgroundOpacity * 255 / 100));
			canvas.fill(background);
		}

		QPainter painter(&canvas);
		painter.setRenderHint(QPainter::TextAntialiasing);
		QFont font = style.font;
		font.setPixelSize(static_cast<int>(style.fontSize));
		painter.setFont(font);
		painter.setPen(style.textColor);
		const QFontMetrics metrics(font);
		const int lineHeight = metrics.lineSpacing();
		const int textHeight = std::min(static_cast<int>(kOverlayHeight) - 2 * static_cast<int>(style.padding), lineHeight * static_cast<int>(style.visibleLines));
		const QRect textRect(static_cast<int>(style.padding), (static_cast<int>(kOverlayHeight) - textHeight) / 2,
				     static_cast<int>(kOverlayWidth) - 2 * static_cast<int>(style.padding), textHeight);
		int alignment = static_cast<int>(Qt::AlignVCenter) | static_cast<int>(Qt::TextWordWrap);
		if (style.textAlignment == 0)
			alignment |= static_cast<int>(Qt::AlignLeft);
		else if (style.textAlignment == 2)
			alignment |= static_cast<int>(Qt::AlignRight);
		else
			alignment |= static_cast<int>(Qt::AlignHCenter);
		const QString previewText = style.captionText.isEmpty() ? QStringLiteral("Your captions will appear here") : style.captionText;
		painter.drawText(textRect, alignment, previewText);
		painter.end();

		if (source->texture)
			gs_texture_destroy(source->texture);
		const uint8_t *pixels = canvas.constBits();
		source->texture = gs_texture_create(kOverlayWidth, kOverlayHeight, GS_BGRA, 1, &pixels, GS_DYNAMIC);
		source->textureRevision = style.revision;
	}

	if (!source->texture)
		return;
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture_srgb(image, source->texture);
	gs_draw_sprite(source->texture, 0, kOverlayWidth, kOverlayHeight);
}

void captionSourceDefaults(obs_data_t *settings)
{
	obs_data_set_int(settings, "width", kOverlayWidth);
	obs_data_set_int(settings, "height", kOverlayHeight);
}

struct obs_source_info captionSourceInfo = {};

struct SceneItemSearch {
	obs_sceneitem_t *match = nullptr;
	obs_sceneitem_t *selected = nullptr;
};

vec2 defaultCaptionPosition()
{
	vec2 position = {};
	obs_video_info videoInfo = {};
	if (obs_get_video_info(&videoInfo)) {
		position.x = videoInfo.base_width > kOverlayWidth ? (videoInfo.base_width - kOverlayWidth) / 2.0f : 0.0f;
		position.y = videoInfo.base_height > kOverlayHeight + 80 ? videoInfo.base_height - kOverlayHeight - 80.0f : 0.0f;
	}
	return position;
}

bool findCaptionSceneItem(obs_scene_t *, obs_sceneitem_t *item, void *data)
{
	auto *search = static_cast<SceneItemSearch *>(data);
	auto *source = obs_sceneitem_get_source(item);
	if (source && strcmp(obs_source_get_id(source), kSourceId) == 0) {
		search->match = item;
		obs_sceneitem_addref(item);
	}
	return true;
}

bool deselectSceneItems(obs_scene_t *, obs_sceneitem_t *item, void *)
{
	if (obs_sceneitem_selected(item))
		obs_sceneitem_select(item, false);
	return true;
}

void addOrSelectCaptionArea(QLabel *status)
{
	obs_source_t *sceneSource = obs_frontend_preview_program_mode_active()
				    ? obs_frontend_get_current_preview_scene()
				    : obs_frontend_get_current_scene();
	if (!sceneSource) {
		status->setText(QStringLiteral("Select a scene before adding the caption area."));
		return;
	}

	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene) {
		obs_source_release(sceneSource);
		status->setText(QStringLiteral("The selected item is not an editable scene."));
		return;
	}

	SceneItemSearch search;
	obs_scene_enum_items(scene, findCaptionSceneItem, &search);
	obs_sceneitem_t *item = search.match;
	if (!item) {
		obs_source_t *captionSource = obs_source_create(kSourceId, "Pixelion Captions", nullptr, nullptr);
		if (captionSource) {
			item = obs_scene_add(scene, captionSource);
			obs_source_release(captionSource);
			if (item) {
				obs_sceneitem_addref(item);
				const vec2 position = defaultCaptionPosition();
				obs_sceneitem_set_pos(item, &position);
			}
		}
	}

	if (item) {
		obs_scene_enum_items(scene, deselectSceneItems, nullptr);
		obs_sceneitem_set_locked(item, false);
		obs_sceneitem_set_visible(item, true);
		obs_sceneitem_select(item, true);
		status->setText(QStringLiteral("Caption area selected. Drag it in the preview or use its handles to resize."));
		obs_sceneitem_release(item);
	} else {
		status->setText(QStringLiteral("Could not add the caption area to this scene."));
	}
	obs_source_release(sceneSource);
}

bool resetCaptionArea(QLabel *status)
{
	obs_source_t *sceneSource = obs_frontend_preview_program_mode_active()
				    ? obs_frontend_get_current_preview_scene()
				    : obs_frontend_get_current_scene();
	if (!sceneSource) {
		status->setText(QStringLiteral("Select a scene before resetting the caption area."));
		return false;
	}

	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene) {
		obs_source_release(sceneSource);
		status->setText(QStringLiteral("The selected item is not an editable scene."));
		return false;
	}

	SceneItemSearch search;
	obs_scene_enum_items(scene, findCaptionSceneItem, &search);
	auto *item = search.match;
	if (!item) {
		status->setText(QStringLiteral("Add the caption area to this scene before resetting it."));
		obs_source_release(sceneSource);
		return false;
	}

	obs_scene_enum_items(scene, deselectSceneItems, nullptr);
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_NONE);
	obs_sceneitem_set_scale_filter(item, OBS_SCALE_DISABLE);
	vec2 scale = {1.0f, 1.0f};
	obs_sceneitem_set_scale(item, &scale);
	obs_sceneitem_set_rot(item, 0.0f);
	obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
	const obs_sceneitem_crop crop = {};
	obs_sceneitem_set_crop(item, &crop);
	const vec2 position = defaultCaptionPosition();
	obs_sceneitem_set_pos(item, &position);
	obs_sceneitem_set_locked(item, false);
	obs_sceneitem_set_visible(item, true);
	obs_sceneitem_select(item, true);
	status->setText(QStringLiteral("Caption area restored to its default size and bottom-center position."));
	obs_sceneitem_release(item);
	obs_source_release(sceneSource);
	return true;
}

QWidget *createDockContents(PixelionAuthService *auth, PixelionLiveSession *session)
{
	auto *root = new QWidget;
	auto *libraryNetwork = new QNetworkAccessManager(root);
	root->setObjectName(QStringLiteral("pixelionLiveCaptionsDock"));
	root->setAttribute(Qt::WA_TranslucentBackground);
	root->setAttribute(Qt::WA_NoSystemBackground);
	auto *outer = new QVBoxLayout(root);
	outer->setContentsMargins(12, 10, 12, 12);
	auto *scroll = new QScrollArea(root);
	scroll->setWidgetResizable(true);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setBackgroundRole(QPalette::Window);
	scroll->setAutoFillBackground(true);
	scroll->viewport()->setBackgroundRole(QPalette::Window);
	scroll->viewport()->setAutoFillBackground(true);
	auto *content = new QWidget(scroll);
	content->setMinimumWidth(0);
	content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	content->setBackgroundRole(QPalette::Window);
	content->setAutoFillBackground(true);
	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(12);

	auto *brandRow = new QWidget(content);
	auto *brandLayout = new QHBoxLayout(brandRow);
	brandLayout->setContentsMargins(0, 0, 0, 0);
	brandLayout->setSpacing(10);
	auto *logo = new QLabel(brandRow);
	logo->setFixedSize(40, 40);
	logo->setAlignment(Qt::AlignCenter);
	logo->setPixmap(QPixmap(QStringLiteral(":/pixelion/logo.png")).scaled(40, 40, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	logo->setAccessibleName(QStringLiteral("Pixelion logo"));
	brandLayout->addWidget(logo, 0, Qt::AlignTop);
	auto *brandCopy = new QWidget(brandRow);
	auto *brandCopyLayout = new QVBoxLayout(brandCopy);
	brandCopyLayout->setContentsMargins(0, 0, 0, 0);
	brandCopyLayout->setSpacing(1);
	auto *brand = new QLabel(QStringLiteral("Pixelion Captions"), brandCopy);
	QFont brandFont = brand->font();
	brandFont.setPointSize(15);
	brandFont.setBold(true);
	brand->setFont(brandFont);
	brandCopyLayout->addWidget(brand);
	auto *tagline = new QLabel(QStringLiteral("Live captions · translation"), brandCopy);
	QFont taglineFont = tagline->font();
	taglineFont.setPointSize(9);
	tagline->setFont(taglineFont);
	QPalette taglinePalette = tagline->palette();
	taglinePalette.setColor(QPalette::WindowText, taglinePalette.color(QPalette::Mid));
	tagline->setPalette(taglinePalette);
	brandCopyLayout->addWidget(tagline);
	brandLayout->addWidget(brandCopy, 1);
	auto *settingsButton = new QPushButton(brandRow);
	settingsButton->setIcon(QIcon(QStringLiteral(":/settings/images/settings/general.svg")));
	settingsButton->setProperty("class", QStringLiteral("icon-gear"));
	settingsButton->setFixedSize(32, 32);
	settingsButton->setIconSize(QSize(16, 16));
	settingsButton->setText(QString());
	settingsButton->setToolTip(QStringLiteral("Transcript settings"));
	settingsButton->setAccessibleName(QStringLiteral("Transcript settings"));
	brandLayout->addWidget(settingsButton, 0, Qt::AlignTop);
	layout->addWidget(brandRow);
	auto *brandAccent = new QFrame(content);
	brandAccent->setFixedHeight(3);
	brandAccent->setStyleSheet(QStringLiteral("border: 0; border-radius: 1px; background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #7828a8, stop:0.55 #e5007d, stop:1 #ffca05);"));
	layout->addWidget(brandAccent);

	auto *statusRow = new QWidget(content);
	auto *statusLayout = new QHBoxLayout(statusRow);
	statusLayout->setContentsMargins(0, 0, 0, 0);
	statusLayout->setSpacing(7);
	auto *statusDot = new QLabel(statusRow);
	statusDot->setFixedSize(9, 9);
	statusDot->setStyleSheet(QStringLiteral("background-color: #78808e; border-radius: 4px;"));
	statusLayout->addWidget(statusDot, 0, Qt::AlignVCenter);
	auto *status = new QLabel(QStringLiteral("Ready to set up live captions"), statusRow);
	status->setObjectName(QStringLiteral("mutedText"));
	status->setWordWrap(true);
	statusLayout->addWidget(status, 1, Qt::AlignVCenter);
	layout->addWidget(statusRow);
	auto readiness = std::make_shared<std::function<void()>>();

	auto *accountHeader = new QWidget(content);
	auto *accountHeaderLayout = new QHBoxLayout(accountHeader);
	accountHeaderLayout->setContentsMargins(0, 0, 0, 0);
	accountHeaderLayout->setSpacing(6);
	auto *accountHeading = new QLabel(QStringLiteral("Account"), accountHeader);
	QFont accountHeadingFont = accountHeading->font();
	accountHeadingFont.setBold(true);
	accountHeading->setFont(accountHeadingFont);
	accountHeaderLayout->addWidget(accountHeading);
	accountHeaderLayout->addStretch(1);
	layout->addWidget(accountHeader);
	auto *accountMessage = new QLabel(QStringLiteral("Sign in to Pixelion to start a live session."), content);
	accountMessage->setObjectName(QStringLiteral("mutedText"));
	accountMessage->setWordWrap(true);
	layout->addWidget(accountMessage);
	auto *signIn = new QPushButton(QStringLiteral("Sign in"), content);
	signIn->setEnabled(true);
	layout->addWidget(signIn);
	auto *createAccount = new QPushButton(QStringLiteral("Create account"), content);
	createAccount->setEnabled(true);
	layout->addWidget(createAccount);
	auto *usageSummary = new QLabel(content);
	usageSummary->setObjectName(QStringLiteral("mutedText"));
	usageSummary->setWordWrap(true);
	usageSummary->setVisible(false);
	layout->addWidget(usageSummary);
	auto *usageBar = new QProgressBar(content);
	usageBar->setRange(0, 100);
	usageBar->setValue(0);
	usageBar->setTextVisible(false);
	usageBar->setFixedHeight(8);
	usageBar->setAccessibleName(QStringLiteral("Live caption minutes used"));
	usageBar->setVisible(false);
	layout->addWidget(usageBar);
	auto *historyButton = new QPushButton(QStringLiteral("Transcription history"), content);
	layout->addWidget(historyButton);
	QObject::connect(historyButton, &QPushButton::clicked, root, [root, auth, libraryNetwork] { openTranscriptHistory(root, auth, libraryNetwork); });
	QObject::connect(settingsButton, &QPushButton::clicked, root, [root, auth, session] {
		QDialog dialog(root);
		dialog.setWindowTitle(QStringLiteral("Transcript settings"));
		dialog.setModal(true);
		auto *dialogLayout = new QVBoxLayout(&dialog);
		auto *storageGroup = new QGroupBox(QStringLiteral("Local transcript storage"), &dialog);
		auto *storageLayout = new QVBoxLayout(storageGroup);
		auto *saveLocally = new QCheckBox(QStringLiteral("Save transcripts to this computer"), storageGroup);
		saveLocally->setChecked(saveTranscriptLocally);
		saveLocally->setToolTip(QStringLiteral("A recoverable transcript file is updated as captions arrive."));
		storageLayout->addWidget(saveLocally);
		auto *saveToPixelion = new QCheckBox(QStringLiteral("Save completed transcripts to Pixelion Library"), storageGroup);
		saveToPixelion->setChecked(autoSaveTranscriptToPixelion);
		saveToPixelion->setToolTip(QStringLiteral("Uploads the transcript after a session ends. Local checkpoints are written first."));
		storageLayout->addWidget(saveToPixelion);
		auto *folderRow = new QWidget(storageGroup);
		auto *folderLayout = new QHBoxLayout(folderRow);
		folderLayout->setContentsMargins(0, 0, 0, 0);
		auto *folder = new QLineEdit(transcriptDirectory, folderRow);
		folder->setReadOnly(true);
		folder->setToolTip(QStringLiteral("Folder used for completed and recoverable transcripts."));
		auto *browse = new QPushButton(QStringLiteral("Browse…"), folderRow);
		folderLayout->addWidget(folder, 1);
		folderLayout->addWidget(browse);
		storageLayout->addWidget(folderRow);
		auto *openTranscripts = new QPushButton(QStringLiteral("Open Transcriptions folder"), storageGroup);
		storageLayout->addWidget(openTranscripts);
		QObject::connect(openTranscripts, &QPushButton::clicked, &dialog, [&dialog, folder] {
			const QString path = folder->text().trimmed();
			if (path.isEmpty() || !QDir().mkpath(path) || !QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
				QMessageBox::warning(&dialog, QStringLiteral("Transcript folder"), QStringLiteral("Could not open the transcript folder. Check that the location is available."));
		});
		auto *patternLabel = new QLabel(QStringLiteral("Transcript filename pattern"), storageGroup);
		storageLayout->addWidget(patternLabel);
		auto *filenamePattern = new QLineEdit(transcriptFilenamePattern, storageGroup);
		filenamePattern->setMaxLength(120);
		filenamePattern->setPlaceholderText(QStringLiteral("Pixelion Live Captions {date} {time}"));
		storageLayout->addWidget(filenamePattern);
		auto *patternHint = new QLabel(QStringLiteral("Available placeholders: {date}, {time}, {mode}. Invalid filename characters are replaced automatically."), storageGroup);
		patternHint->setWordWrap(true);
		patternHint->setObjectName(QStringLiteral("mutedText"));
		storageLayout->addWidget(patternHint);
		auto *recoveryHint = new QLabel(QStringLiteral("While a session is active, its .partial.txt file is checkpointed after every caption update and renamed to .txt when it ends. Pixelion Library saving is optional and happens after the local transcript is complete."), storageGroup);
		recoveryHint->setWordWrap(true);
		recoveryHint->setObjectName(QStringLiteral("mutedText"));
		storageLayout->addWidget(recoveryHint);
		dialogLayout->addWidget(storageGroup);
		QObject::connect(browse, &QPushButton::clicked, &dialog, [&dialog, folder] {
			const QString selected = QFileDialog::getExistingDirectory(&dialog, QStringLiteral("Choose transcript folder"), folder->text());
			if (!selected.isEmpty()) folder->setText(selected);
		});

		auto *diagnosticsGroup = new QGroupBox(QStringLiteral("Diagnostics"), &dialog);
		auto *diagnosticsLayout = new QVBoxLayout(diagnosticsGroup);
		auto *logPath = new QLineEdit(PixelionDebugLog::filePath(), diagnosticsGroup);
		logPath->setReadOnly(true);
		logPath->setToolTip(QStringLiteral("Share this file to help diagnose plugin errors. It does not contain transcript text or authentication tokens."));
		diagnosticsLayout->addWidget(logPath);
		auto *openLogFolder = new QPushButton(QStringLiteral("Open log folder"), diagnosticsGroup);
		diagnosticsLayout->addWidget(openLogFolder);
		QObject::connect(openLogFolder, &QPushButton::clicked, &dialog, [logPath] {
			QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(logPath->text()).absolutePath()));
		});
		dialogLayout->addWidget(diagnosticsGroup);
		bool developerDiagnostics = qEnvironmentVariableIntValue("PIXELION_OBS_DIAGNOSTICS") == 1;
#ifndef NDEBUG
		developerDiagnostics = true;
#endif
		auto *showDiagnostics = new QCheckBox(QStringLiteral("Show diagnostics"), &dialog);
		showDiagnostics->setVisible(developerDiagnostics);
		diagnosticsGroup->setVisible(false);
		dialogLayout->insertWidget(dialogLayout->indexOf(diagnosticsGroup), showDiagnostics);
		QObject::connect(showDiagnostics, &QCheckBox::toggled, diagnosticsGroup, &QWidget::setVisible);

		auto *accountGroup = new QGroupBox(QStringLiteral("Pixelion account"), &dialog);
		auto *accountLayout = new QVBoxLayout(accountGroup);
		const QString planName = auth->plan().isEmpty() ? QStringLiteral("Free") : auth->plan();
		auto *identity = new QLabel(auth->email().isEmpty()
						? QStringLiteral("Not signed in")
						: QStringLiteral("%1\n%2 plan · Pixelion account").arg(auth->email(), planName), accountGroup);
		identity->setWordWrap(true);
		accountLayout->addWidget(identity);
		auto *signOut = new QPushButton(QStringLiteral("Sign out"), accountGroup);
		signOut->setEnabled(auth->isSignedIn());
		accountLayout->addWidget(signOut);
		dialogLayout->addWidget(accountGroup);
	QObject::connect(signOut, &QPushButton::clicked, &dialog, [auth, &dialog] {
		PixelionDebugLog::write("INFO", QStringLiteral("User signed out of Pixelion"));
			auth->signOut();
			dialog.accept();
		});

		const bool canEdit = !session->isRunning();
		saveLocally->setEnabled(canEdit);
		saveToPixelion->setEnabled(canEdit);
		browse->setEnabled(canEdit);
		filenamePattern->setEnabled(canEdit);
		if (!canEdit) {
			auto *activeHint = new QLabel(QStringLiteral("Stop the current session before changing transcript storage settings."), &dialog);
			activeHint->setWordWrap(true);
			activeHint->setObjectName(QStringLiteral("mutedText"));
			dialogLayout->insertWidget(1, activeHint);
		}
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
		dialogLayout->addWidget(buttons);
		QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
		QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
		if (dialog.exec() == QDialog::Accepted && canEdit) {
			saveTranscriptLocally = saveLocally->isChecked();
			autoSaveTranscriptToPixelion = saveToPixelion->isChecked();
			transcriptDirectory = folder->text().trimmed();
			transcriptFilenamePattern = filenamePattern->text().trimmed();
			if (transcriptFilenamePattern.isEmpty())
				transcriptFilenamePattern = QStringLiteral("Pixelion Live Captions {date} {time}");
			saveTranscriptSettings();
		}
	});
	QObject::connect(signIn, &QPushButton::clicked, root, [auth] { auth->signIn(false); });
	QObject::connect(createAccount, &QPushButton::clicked, root, [auth] { auth->signIn(true); });
	QPointer<QLabel> accountLabel = accountMessage;
	QPointer<QPushButton> signInButton = signIn;
	QPointer<QPushButton> createAccountButton = createAccount;
	auth->setStateCallback([accountLabel, signInButton, createAccountButton, readiness](const QString &email, const QString &plan, const QString &message) {
		const QString normalized = message.toLower();
		if (normalized.contains(QStringLiteral("could not")) || normalized.contains(QStringLiteral("failed")) ||
		    normalized.contains(QStringLiteral("error")) || normalized.contains(QStringLiteral("rejected")) ||
		    normalized.contains(QStringLiteral("timed out")))
			PixelionDebugLog::write("ERROR", QStringLiteral("Account: %1").arg(message));
		else if (!email.isEmpty() && message == QStringLiteral("Signed in to Pixelion."))
			PixelionDebugLog::write("INFO", QStringLiteral("Pixelion sign-in succeeded"));
		if (accountLabel) {
			const QString planName = plan.isEmpty() ? QStringLiteral("Free") : plan;
			accountLabel->setText(email.isEmpty() ? message : QStringLiteral("%1\n%2 plan · Pixelion account\nMaximum transcription length: up to %3 minutes")
										   .arg(email, planName).arg(maximumTranscriptionMinutesForPlan(planName)));
		}
		const bool signedIn = !email.isEmpty();
		if (signInButton) signInButton->setVisible(!signedIn);
		if (createAccountButton) createAccountButton->setVisible(!signedIn);
		if (readiness && *readiness) (*readiness)();
		if (signedIn && autoStartHandler) autoStartHandler(streamingActive || recordingActive);
	});
	auth->setUsageCallback([usageSummaryPtr = QPointer<QLabel>(usageSummary), usageBarPtr = QPointer<QProgressBar>(usageBar)](const PixelionAuthService::UsageInfo &usage) {
		if (!usageSummaryPtr || !usageBarPtr) return;
		if (usage.loading) {
			usageSummaryPtr->setText(QStringLiteral("Loading available minutes…"));
			usageSummaryPtr->setVisible(true);
			usageBarPtr->setVisible(false);
		} else if (!usage.known) {
			usageSummaryPtr->setVisible(false);
			usageBarPtr->setVisible(false);
		} else if (usage.unlimited) {
			usageSummaryPtr->setText(QStringLiteral("Unlimited minutes"));
			usageSummaryPtr->setVisible(true);
			usageBarPtr->setVisible(false);
		} else {
			const int remaining = std::max(0, usage.limitMinutes - usage.usedMinutes);
			const QString period = usage.periodKind == QStringLiteral("trial") ? QStringLiteral("in your trial") : QStringLiteral("this month");
			usageSummaryPtr->setText(QStringLiteral("%1 of %2 minutes remaining %3").arg(remaining).arg(usage.limitMinutes).arg(period));
			usageSummaryPtr->setVisible(true);
			usageBarPtr->setRange(0, usage.limitMinutes);
			usageBarPtr->setValue(usage.usedMinutes);
			usageBarPtr->setVisible(true);
		}
	});

	auto *sessionHeading = new QLabel(QStringLiteral("Start a session"), content);
	QFont sessionHeadingFont = sessionHeading->font();
	sessionHeadingFont.setBold(true);
	sessionHeading->setFont(sessionHeadingFont);
	layout->addWidget(sessionHeading);
	auto *mode = new QComboBox(content);
	mode->addItem(QStringLiteral("Transcribe"));
	mode->addItem(QStringLiteral("Translate"));
	mode->setEnabled(true);
	mode->setToolTip(QStringLiteral("Choose whether the session will transcribe or translate speech."));
	layout->addWidget(mode);
	auto *sourceLanguage = new QComboBox(content);
	sourceLanguage->addItem(QStringLiteral("Auto detect"), QStringLiteral("auto"));
	sourceLanguage->addItems({QStringLiteral("Afrikaans"), QStringLiteral("Arabic"), QStringLiteral("Chinese"), QStringLiteral("Dutch"), QStringLiteral("English"), QStringLiteral("French"), QStringLiteral("German"), QStringLiteral("Hindi"), QStringLiteral("Italian"), QStringLiteral("Japanese"), QStringLiteral("Korean"), QStringLiteral("Portuguese"), QStringLiteral("Russian"), QStringLiteral("Spanish")});
	const QStringList sourceCodes{QStringLiteral("af"), QStringLiteral("ar"), QStringLiteral("zh"), QStringLiteral("nl"), QStringLiteral("en"), QStringLiteral("fr"), QStringLiteral("de"), QStringLiteral("hi"), QStringLiteral("it"), QStringLiteral("ja"), QStringLiteral("ko"), QStringLiteral("pt"), QStringLiteral("ru"), QStringLiteral("es")};
	for (int i = 0; i < sourceCodes.size(); ++i) sourceLanguage->setItemData(i + 1, sourceCodes.at(i));
	layout->addWidget(new QLabel(QStringLiteral("Speech language"), content));
	layout->addWidget(sourceLanguage);
	auto *targetLanguage = new QComboBox(content);
	const QStringList targetNames{QStringLiteral("Chinese"), QStringLiteral("Dutch"), QStringLiteral("English"), QStringLiteral("French"), QStringLiteral("German"), QStringLiteral("Hindi"), QStringLiteral("Italian"), QStringLiteral("Japanese"), QStringLiteral("Korean"), QStringLiteral("Polish"), QStringLiteral("Portuguese"), QStringLiteral("Russian"), QStringLiteral("Spanish")};
	const QStringList targetCodes{QStringLiteral("zh"), QStringLiteral("nl"), QStringLiteral("en"), QStringLiteral("fr"), QStringLiteral("de"), QStringLiteral("hi"), QStringLiteral("it"), QStringLiteral("ja"), QStringLiteral("ko"), QStringLiteral("pl"), QStringLiteral("pt"), QStringLiteral("ru"), QStringLiteral("es")};
	for (int i = 0; i < targetNames.size(); ++i) targetLanguage->addItem(targetNames.at(i), targetCodes.at(i));
	auto *targetLabel = new QLabel(QStringLiteral("Translate to"), content);
	layout->addWidget(targetLabel);
	layout->addWidget(targetLanguage);
	targetLanguage->setVisible(false);
	targetLabel->setVisible(false);
	QObject::connect(mode, qOverload<int>(&QComboBox::currentIndexChanged), targetLanguage, [targetLanguage](int index) { targetLanguage->setVisible(index == 1); });
	QObject::connect(mode, qOverload<int>(&QComboBox::currentIndexChanged), targetLabel, [targetLabel](int index) { targetLabel->setVisible(index == 1); });
	auto *audio = new QLabel(QStringLiteral("Audio input: OBS program mix (default)"), content);
	audio->setWordWrap(true);
	audio->setObjectName(QStringLiteral("mutedText"));
	layout->addWidget(audio);
	auto *consent = new QCheckBox(QStringLiteral("I agree: send OBS audio to Pixelion"), content);
	consent->setToolTip(QStringLiteral("OBS program audio is sent to Pixelion for live transcription or translation."));
	layout->addWidget(consent);
	*readiness = [auth, consent, session, status, statusDot] {
		if (session->isRunning()) return;
		const bool consented = auth->isSignedIn() && consent->isChecked();
		status->setText(consented ? QStringLiteral("Ready to start live captions")
				      : (auth->isSignedIn() ? QStringLiteral("Agree to audio processing before starting.")
							   : QStringLiteral("Sign in to Pixelion to set up live captions.")));
		statusDot->setStyleSheet(consented ? QStringLiteral("background-color: #f0a23a; border-radius: 4px;")
							: QStringLiteral("background-color: #78808e; border-radius: 4px;"));
	};
	(*readiness)();
	QObject::connect(consent, &QCheckBox::toggled, root, [readiness](bool) {
		if (readiness && *readiness) (*readiness)();
		if (autoStartHandler) autoStartHandler(streamingActive || recordingActive);
	});
	auto *autoStart = new QCheckBox(QStringLiteral("Start/stop with streaming or recording"), content);
	autoStart->setChecked(loadAutoStartSetting());
	autoStart->setToolTip(QStringLiteral("Automatically start captions when OBS starts streaming or recording. Requires audio consent."));
	layout->addWidget(autoStart);
	auto *start = new SpinnerButton(content);
	const auto updateManualStartAvailability = [autoStart, start] {
		const bool automatic = autoStart->isChecked();
		start->setEnabled(!automatic);
		start->setToolTip(automatic
				  ? QStringLiteral("Manual start is disabled while automatic start/stop is enabled.")
				  : QStringLiteral("Start or stop a live caption session."));
	};
	updateManualStartAvailability();
	QObject::connect(autoStart, &QCheckBox::toggled, root, [start, updateManualStartAvailability](bool enabled) {
		saveAutoStartSetting(enabled);
		updateManualStartAvailability();
		if (autoStartHandler) autoStartHandler(streamingActive || recordingActive);
	});
	auto *sessionControls = new QWidget(content);
	auto *sessionControlsLayout = new QVBoxLayout(sessionControls);
	sessionControlsLayout->setContentsMargins(0, 0, 0, 0);
	sessionControlsLayout->setSpacing(4);
	auto *sessionStatusRow = new QWidget(sessionControls);
	auto *sessionStatusLayout = new QHBoxLayout(sessionStatusRow);
	sessionStatusLayout->setContentsMargins(0, 0, 0, 0);
	sessionStatusLayout->addStretch(1);
	auto *activeSessionIndicator = new ActiveSessionIndicator(sessionStatusRow);
	sessionStatusLayout->addWidget(activeSessionIndicator, 0, Qt::AlignVCenter);
	auto *sessionTimer = new SessionElapsedTimer(sessionStatusRow);
	sessionStatusLayout->addWidget(sessionTimer, 0, Qt::AlignVCenter);
	sessionStatusLayout->addStretch(1);
	sessionControlsLayout->addWidget(sessionStatusRow);
	sessionControlsLayout->addWidget(start);
	layout->addWidget(sessionControls);
	auto limitActionState = std::make_shared<QString>();
	auto beginSession = [session, auth, mode, sourceLanguage, targetLanguage, consent, status, root, start, limitActionState](bool automatic) {
		if (session->isRunning()) {
			return false;
		}
		if (!auth->isSignedIn()) {
			const QString message = automatic ? QStringLiteral("Sign in to Pixelion before OBS can auto-start captions.") : QStringLiteral("Sign in to Pixelion before starting a session.");
			status->setText(message);
			PixelionDebugLog::write("ERROR", message);
			if (!automatic) QMessageBox::warning(root, QStringLiteral("Pixelion Captions"), message);
			return false;
		}
		if (!consent->isChecked()) {
			const QString message = QStringLiteral("Agree to audio processing before starting.");
			status->setText(message);
			PixelionDebugLog::write("WARNING", message);
			if (!automatic) QMessageBox::warning(root, QStringLiteral("Pixelion Captions"), message);
			return false;
		}
		const QString liveMode = mode->currentIndex() == 1 ? QStringLiteral("translate") : QStringLiteral("transcribe");
		PixelionDebugLog::write("INFO", QStringLiteral("Starting %1 session (speech language: %2, target language: %3)")
							.arg(liveMode, sourceLanguage->currentData().toString(), targetLanguage->currentData().toString()));
		transcriptMode = liveMode;
		limitActionState->clear();
		start->setIdleLabel(QStringLiteral("Start"));
		transcriptSourceLanguage = sourceLanguage->currentData().toString();
		transcriptTargetLanguage = targetLanguage->currentData().toString();
		transcriptSessionStartedAt = QDateTime::currentMSecsSinceEpoch();
		beginLocalTranscript();
		session->start(liveMode, sourceLanguage->currentData().toString(), targetLanguage->currentData().toString());
		transcriptSessionId = session->sessionId();
		transcriptJobId = transcriptSessionId;
		writeTranscriptMetadata(false);
		return session->isRunning();
	};
	QObject::connect(start, &QPushButton::clicked, root, [session, auth, beginSession, limitActionState] {
		if (!limitActionState->isEmpty()) {
			const QString plan = auth->plan().trimmed().toLower();
			if (*limitActionState == QStringLiteral("duration") && !plan.isEmpty() && plan != QStringLiteral("free"))
				beginSession(false);
			else
				QDesktopServices::openUrl(QUrl(QStringLiteral("https://pixelion.ai/pricing")));
			return;
		}
		if (session->isRunning()) {
			session->stop();
			return;
		}
		beginSession(false);
	});
	auto autoStarted = std::make_shared<bool>(false);
	autoStartHandler = [session, autoStartPtr = QPointer<QCheckBox>(autoStart), beginSession, autoStarted](bool obsActive) {
		if (!autoStartPtr || !autoStartPtr->isChecked()) {
			*autoStarted = false;
			return;
		}
		if (!obsActive && *autoStarted) {
			*autoStarted = false;
			if (session->isRunning()) session->stop();
		} else if (obsActive) {
			*autoStarted = session->isRunning() || beginSession(true);
		}
	};
	session->setCallbacks([statusPtr = QPointer<QLabel>(status), statusDotPtr = QPointer<QLabel>(statusDot), startPtr = QPointer<SpinnerButton>(start), activePtr = QPointer<ActiveSessionIndicator>(activeSessionIndicator), timerPtr = QPointer<SessionElapsedTimer>(sessionTimer), readiness, auth, libraryNetwork, limitActionState](bool running, const QString &message) {
		const QString normalized = message.toLower();
		const bool hasError = normalized.contains(QStringLiteral("error")) || normalized.contains(QStringLiteral("failed")) ||
			normalized.contains(QStringLiteral("connection was interrupted")) ||
			normalized.contains(QStringLiteral("could not")) || normalized.contains(QStringLiteral("rejected")) ||
			normalized.contains(QStringLiteral("requires pixelion pro")) ||
			normalized.contains(QStringLiteral("requires a pro plan")) ||
			normalized.contains(QStringLiteral("used up")) || normalized.contains(QStringLiteral("limit has been reached"));
		if (hasError)
			PixelionDebugLog::write("ERROR", QStringLiteral("Session: %1").arg(message));
		else
			PixelionDebugLog::write("INFO", QStringLiteral("Session: %1").arg(message));
		const bool limitReached = message == QStringLiteral("Your session limit has been reached.");
		const bool allowanceReached = message == QStringLiteral("Your available Pixelion minutes are used up.");
		const bool limitWarning = message.startsWith(QStringLiteral("Your session limit is approaching."));
		const bool connected = running && (message == QStringLiteral("Connected. Listening to OBS program audio.") || limitWarning);
		if (limitWarning) limitActionState->clear();
		if (limitReached || allowanceReached) {
			const QString plan = auth->plan().trimmed().toLower();
			const bool freeTier = plan.isEmpty() || plan == QStringLiteral("free");
			*limitActionState = allowanceReached ? QStringLiteral("allowance") : QStringLiteral("duration");
			if (startPtr) startPtr->setIdleLabel(allowanceReached
				? (freeTier ? QStringLiteral("Upgrade plan") : QStringLiteral("View plan"))
				: (freeTier ? QStringLiteral("Upgrade plan") : QStringLiteral("Start new session")));
		}
		if (connected) limitActionState->clear();
		if (connected && !transcriptSessionClock.isValid()) transcriptSessionClock.start();
		if (startPtr) {
			startPtr->setSessionRunning(!limitActionState->isEmpty() ? false : running);
			startPtr->setConnecting(running && !connected && !hasError && !limitReached && !allowanceReached && limitActionState->isEmpty());
		}
		if (activePtr) activePtr->setActive(connected);
		if (timerPtr) timerPtr->setRunning(connected);
		if (statusDotPtr) {
			const QString color = hasError ? QStringLiteral("#e56b6f")
						 : connected ? QStringLiteral("#35c98a")
							     : (running ? QStringLiteral("#f0a23a") : QStringLiteral("#78808e"));
			statusDotPtr->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 4px;").arg(color));
		}
		if (!running && !hasError) {
			if (readiness && *readiness) (*readiness)();
		} else if (statusPtr && !( !limitActionState->isEmpty() && message == QStringLiteral("Session ended."))) {
			statusPtr->setText(message);
		}
		if (!running) {
			const QString transcript = finalizeLocalTranscript();
			if (autoSaveTranscriptToPixelion && !transcript.trimmed().isEmpty())
				saveTranscriptToPixelion(auth, libraryNetwork, hasError ? QPointer<QLabel>() : statusPtr, transcriptSessionId, transcriptMode,
							transcriptSourceLanguage, transcriptTargetLanguage, transcriptSessionStartedAt, transcript,
							transcriptMetadataPath);
		}
		if (running && statusPtr) statusPtr->setText(message);
	}, [](const QString &text, bool final) { updateCaptionText(text, final); });

	auto *appearanceHeading = new QLabel(QStringLiteral("Caption overlay"), content);
	QFont headingFont = appearanceHeading->font();
	headingFont.setBold(true);
	appearanceHeading->setFont(headingFont);
	layout->addWidget(appearanceHeading);
	auto *placementHint = new QLabel(QStringLiteral("Add the caption area to your scene, then position and resize it in the preview."), content);
	placementHint->setWordWrap(true);
	placementHint->setObjectName(QStringLiteral("mutedText"));
	layout->addWidget(placementHint);
	auto *placeCaption = new QPushButton(QStringLiteral("Add / select caption area"), content);
	layout->addWidget(placeCaption);
	QObject::connect(placeCaption, &QPushButton::clicked, root, [status] { addOrSelectCaptionArea(status); });
	auto *styleToggle = new CollapsibleComboBox(content);
	styleToggle->setObjectName(QStringLiteral("captionStyleToggle"));
	styleToggle->addItem(QStringLiteral("Caption style"));
	styleToggle->setCurrentIndex(0);
	styleToggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	layout->addWidget(styleToggle);
	auto *styleContents = new QWidget(content);
	styleContents->setVisible(false);
	auto *styleLayout = new QGridLayout(styleContents);
	styleLayout->setContentsMargins(0, 0, 0, 0);
	styleLayout->setHorizontalSpacing(10);
	styleLayout->setVerticalSpacing(8);
	styleToggle->setExpandedCallback([styleContents](bool expanded) {
		styleContents->setVisible(expanded);
	});
	styleLayout->setColumnStretch(0, 1);
	styleLayout->setColumnStretch(1, 1);
	auto *fontLabel = new QLabel(QStringLiteral("Font family"), styleContents);
	styleLayout->addWidget(fontLabel, 0, 0);
	auto *sizeLabel = new QLabel(QStringLiteral("Font size (px)"), styleContents);
	styleLayout->addWidget(sizeLabel, 0, 1);
	auto *font = new QFontComboBox(styleContents);
	font->setCurrentFont(currentStyle().font);
	QObject::connect(font, &QFontComboBox::currentFontChanged, root, &setCaptionFont);
	styleLayout->addWidget(font, 1, 0);
	auto *size = new QSpinBox(styleContents);
	size->setRange(8, 120);
	size->setValue(static_cast<int>(currentStyle().fontSize));
	QObject::connect(size, qOverload<int>(&QSpinBox::valueChanged), root, &setCaptionFontSize);
	styleLayout->addWidget(size, 1, 1);
	styleLayout->addWidget(new QLabel(QStringLiteral("Text color"), styleContents), 2, 0);
	styleLayout->addWidget(new QLabel(QStringLiteral("Background color"), styleContents), 2, 1);
	auto *textColor = makeColorControl(QStringLiteral("Text color"), ColorTarget::Text, styleContents);
	styleLayout->addWidget(textColor, 3, 0);
	auto *backgroundColor = makeColorControl(QStringLiteral("Background color"), ColorTarget::Background, styleContents);
	styleLayout->addWidget(backgroundColor, 3, 1);
	auto *opacityLabel = new QLabel(QStringLiteral("Background opacity (%)"), styleContents);
	opacityLabel->setWordWrap(true);
	styleLayout->addWidget(opacityLabel, 4, 0);
	styleLayout->addWidget(new QLabel(QStringLiteral("Text alignment"), styleContents), 4, 1);
	auto *opacity = new QSpinBox(styleContents);
	opacity->setRange(0, 100);
	opacity->setSuffix(QStringLiteral("%"));
	opacity->setValue(static_cast<int>(currentStyle().backgroundOpacity));
	QObject::connect(opacity, qOverload<int>(&QSpinBox::valueChanged), root, &setCaptionOpacity);
	styleLayout->addWidget(opacity, 5, 0);
	auto *alignment = new QComboBox(styleContents);
	alignment->addItems({QStringLiteral("Left"), QStringLiteral("Center"), QStringLiteral("Right")});
	alignment->setCurrentIndex(static_cast<int>(currentStyle().textAlignment));
	QObject::connect(alignment, qOverload<int>(&QComboBox::currentIndexChanged), root, &setCaptionAlignment);
	styleLayout->addWidget(alignment, 5, 1);
	styleLayout->addWidget(new QLabel(QStringLiteral("Visible lines"), styleContents), 6, 0);
	auto *backgroundModeLabel = new QLabel(QStringLiteral("Caption window background"), styleContents);
	backgroundModeLabel->setWordWrap(true);
	styleLayout->addWidget(backgroundModeLabel, 6, 1);
	auto *visibleLines = new QSpinBox(styleContents);
	visibleLines->setRange(1, 4);
	visibleLines->setValue(static_cast<int>(currentStyle().visibleLines));
	QObject::connect(visibleLines, qOverload<int>(&QSpinBox::valueChanged), root, &setCaptionVisibleLines);
	styleLayout->addWidget(visibleLines, 7, 0);
	auto *backgroundMode = new QComboBox(styleContents);
	backgroundMode->addItems({QStringLiteral("Solid"), QStringLiteral("Transparent")});
	backgroundMode->setCurrentIndex(static_cast<int>(currentStyle().backgroundMode));
	QObject::connect(backgroundMode, qOverload<int>(&QComboBox::currentIndexChanged), root, &setCaptionBackgroundMode);
	QObject::connect(backgroundMode, qOverload<int>(&QComboBox::currentIndexChanged), opacity, [opacity](int mode) { opacity->setEnabled(mode == 0); });
	styleLayout->addWidget(backgroundMode, 7, 1);
	opacity->setEnabled(backgroundMode->currentIndex() == 0);
	styleLayout->addWidget(new QLabel(QStringLiteral("Overlay padding (px)"), styleContents), 8, 0);
	auto *padding = new QSpinBox(styleContents);
	padding->setRange(0, 200);
	padding->setSuffix(QStringLiteral(" px"));
	padding->setValue(static_cast<int>(currentStyle().padding));
	padding->setToolTip(QStringLiteral("Space between the overlay edges and its caption text."));
	QObject::connect(padding, qOverload<int>(&QSpinBox::valueChanged), root, &setCaptionPadding);
	styleLayout->addWidget(padding, 9, 0);
	auto *resetCaption = new QPushButton(QStringLiteral("Reset to default"), styleContents);
	resetCaption->setToolTip(QStringLiteral("Restore the caption box and all caption style settings to their defaults."));
	styleLayout->addWidget(resetCaption, 10, 0, 1, 2);
	layout->addWidget(styleContents);
	QObject::connect(resetCaption, &QPushButton::clicked, root,
			 [status, font, size, textColor, backgroundColor, opacity, alignment, visibleLines, backgroundMode, padding] {
				 resetCaptionStyle();
				 const QSignalBlocker fontBlocker(font);
				 const QSignalBlocker sizeBlocker(size);
				 const QSignalBlocker opacityBlocker(opacity);
				 const QSignalBlocker alignmentBlocker(alignment);
				 const QSignalBlocker linesBlocker(visibleLines);
				 const QSignalBlocker modeBlocker(backgroundMode);
					 const QSignalBlocker paddingBlocker(padding);
				 font->setCurrentFont(QFont(QStringLiteral("Segoe UI")));
				 size->setValue(32);
				 styleButton(textColor, QColor(Qt::white));
				 styleButton(backgroundColor, QColor(Qt::black));
				 opacity->setValue(70);
				 alignment->setCurrentIndex(1);
				 visibleLines->setValue(2);
				 backgroundMode->setCurrentIndex(0);
					 padding->setValue(36);
				 opacity->setEnabled(true);
				 const bool boxReset = resetCaptionArea(status);
				 if (boxReset)
					 status->setText(QStringLiteral("Caption box and style restored to defaults."));
				 else
					 status->setText(QStringLiteral("Caption style restored. %1").arg(status->text()));
			 });

	auto *shortcutInfo = new QLabel(QStringLiteral("Shortcut: Not assigned. OBS hotkey support will be available with session controls."), content);
	shortcutInfo->setWordWrap(true);
	shortcutInfo->setObjectName(QStringLiteral("mutedText"));
	layout->addWidget(shortcutInfo);
	auto *privacyLink = new QLabel(QStringLiteral("<a href=\"https://pixelion.ai/privacy\">Privacy policy</a>"), content);
	privacyLink->setOpenExternalLinks(true);
	privacyLink->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
	layout->addWidget(privacyLink);

	layout->addStretch(1);
	scroll->setWidget(content);
	outer->addWidget(scroll);
	for (QWidget *control : content->findChildren<QWidget *>()) {
		if (qobject_cast<QComboBox *>(control) || qobject_cast<QSpinBox *>(control) ||
		    qobject_cast<QPushButton *>(control) || qobject_cast<QToolButton *>(control) ||
		    qobject_cast<QProgressBar *>(control)) {
			control->setMinimumWidth(0);
			control->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
		}
	}
	if (auto *fontControl = content->findChild<QFontComboBox *>()) {
		fontControl->setMinimumContentsLength(6);
		fontControl->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	}
	root->setMinimumWidth(280);
	return root;
}

void onFrontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED) streamingActive = true;
	else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED) streamingActive = false;
	else if (event == OBS_FRONTEND_EVENT_RECORDING_STARTED) recordingActive = true;
	else if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPED) recordingActive = false;
	if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED || event == OBS_FRONTEND_EVENT_STREAMING_STOPPED ||
	    event == OBS_FRONTEND_EVENT_RECORDING_STARTED || event == OBS_FRONTEND_EVENT_RECORDING_STOPPED) {
		if (autoStartHandler) autoStartHandler(streamingActive || recordingActive);
	}
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
		obs_frontend_remove_dock(kDockId);
		dockWidget = nullptr;
		autoStartHandler = {};
	}
}

} // namespace

bool obs_module_load(void)
{
	PixelionPlatform::registerBundledQtTlsBackend();
	migrateLegacyPluginSettings();
	PixelionDebugLog::write("INFO", QStringLiteral("Plugin loading; debug log: %1").arg(PixelionDebugLog::filePath()));
	blog(LOG_INFO, "Pixelion Live Captions: debug log at %s", PixelionDebugLog::filePath().toUtf8().constData());
	loadStyle();
	loadTranscriptSettings();
	captionSourceInfo.id = kSourceId;
	captionSourceInfo.type = OBS_SOURCE_TYPE_INPUT;
	captionSourceInfo.output_flags = OBS_SOURCE_VIDEO;
	captionSourceInfo.get_name = captionSourceName;
	captionSourceInfo.create = captionSourceCreate;
	captionSourceInfo.destroy = captionSourceDestroy;
	captionSourceInfo.get_width = captionSourceWidth;
	captionSourceInfo.get_height = captionSourceHeight;
	captionSourceInfo.video_render = captionSourceRender;
	captionSourceInfo.get_defaults = captionSourceDefaults;
	captionSourceInfo.icon_type = OBS_ICON_TYPE_TEXT;
	obs_register_source(&captionSourceInfo);
	authService = new PixelionAuthService;
	liveSession = new PixelionLiveSession(authService);
	auto *widget = createDockContents(authService, liveSession);
	dockWidget = widget;
	if (!obs_frontend_add_dock_by_id(kDockId, "Pixelion Captions", widget)) {
		delete widget;
		dockWidget = nullptr;
		blog(LOG_ERROR, "Pixelion Live Captions: OBS dock registration failed");
		PixelionDebugLog::write("ERROR", QStringLiteral("OBS dock registration failed"));
		return false;
	}
	streamingActive = obs_frontend_streaming_active();
	recordingActive = obs_frontend_recording_active();
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
	authService->restore();
	if (autoStartHandler) autoStartHandler(streamingActive || recordingActive);
	blog(LOG_INFO, "Pixelion Live Captions %s loaded", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	PixelionDebugLog::write("INFO", QStringLiteral("Plugin unloading"));
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	autoStartHandler = {};
	obs_frontend_remove_dock(kDockId);
	dockWidget = nullptr;
	if (liveSession) {
		liveSession->stop();
		delete liveSession;
		liveSession = nullptr;
	}
	finalizeLocalTranscript();
	delete authService;
	authService = nullptr;
	blog(LOG_INFO, "Pixelion Live Captions unloaded");
}
