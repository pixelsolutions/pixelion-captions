#include "feedback-dialog.h"
#include <plugin-support.h>

#include <QButtonGroup>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

void showFeedbackDialog(QWidget *parent)
{
	QDialog dialog(parent);
	dialog.setWindowTitle(QStringLiteral("Send Feedback"));
	dialog.resize(500, 580);
	auto *layout = new QVBoxLayout(&dialog);
	layout->setContentsMargins(24, 24, 24, 24);
	layout->setSpacing(12);
	auto *title = new QLabel(QStringLiteral("Send Feedback"), &dialog);
	QFont titleFont = title->font();
	titleFont.setPointSize(14);
	titleFont.setBold(true);
	title->setFont(titleFont);
	layout->addWidget(title);
	auto *typeLabel = new QLabel(QStringLiteral("Type *"), &dialog);
	layout->addWidget(typeLabel);
	auto *hint = new QLabel(QStringLiteral("Choose the type of feedback you are submitting."), &dialog);
	hint->setWordWrap(true);
	layout->addWidget(hint);
	auto *type = new QComboBox(&dialog);
	typeLabel->setBuddy(type);
	type->setAccessibleName(QStringLiteral("Feedback type (required)"));
	type->addItem(QStringLiteral("Select an option…"), QString());
	const QStringList categories{QStringLiteral("Bug Report"),
				     QStringLiteral("Feature Request"),
				     QStringLiteral("Transcription Quality"),
				     QStringLiteral("UI / Usability"),
				     QStringLiteral("Give Compliment"),
				     QStringLiteral("Other")};
	const QStringList ids{QStringLiteral("bug"), QStringLiteral("feature"),    QStringLiteral("quality"),
			      QStringLiteral("ui"),  QStringLiteral("compliment"), QStringLiteral("other")};
	for (int i = 0; i < categories.size(); ++i)
		type->addItem(categories[i], ids[i]);
	layout->addWidget(type);
	auto *messageLabel = new QLabel(QStringLiteral("Enter your message below. *"), &dialog);
	layout->addWidget(messageLabel);
	auto *message = new QTextEdit(&dialog);
	message->setAcceptRichText(false);
	message->setMinimumHeight(150);
	message->setAccessibleName(QStringLiteral("Feedback message (required)"));
	messageLabel->setBuddy(message);
	layout->addWidget(message, 1);
	auto *ratingLabel = new QLabel(
		QStringLiteral("Overall, how satisfied are you with your experience using Pixelion Captions?"),
		&dialog);
	ratingLabel->setWordWrap(true);
	layout->addWidget(ratingLabel);
	auto *ratings = new QButtonGroup(&dialog);
	auto *ratingRow = new QHBoxLayout;
	const QStringList levels{QStringLiteral("Very dissatisfied"), QStringLiteral("Dissatisfied"),
				 QStringLiteral("Neutral"), QStringLiteral("Satisfied"),
				 QStringLiteral("Very satisfied")};
	for (int i = 0; i < levels.size(); ++i) {
		auto *column = new QVBoxLayout;
		auto *radio = new QRadioButton(&dialog);
		radio->setAccessibleName(levels[i]);
		ratings->addButton(radio, i + 1);
		column->addWidget(radio, 0, Qt::AlignHCenter);
		auto *label = new QLabel(levels[i], &dialog);
		label->setAlignment(Qt::AlignCenter);
		label->setWordWrap(true);
		column->addWidget(label);
		ratingRow->addLayout(column, 1);
	}
	layout->addLayout(ratingRow);
	auto *separator = new QFrame(&dialog);
	separator->setFrameShape(QFrame::HLine);
	layout->addWidget(separator);
	auto *details = new QToolButton(&dialog);
	details->setText(QStringLiteral("Additional Details (optional)"));
	details->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	details->setArrowType(Qt::RightArrow);
	details->setCheckable(true);
	layout->addWidget(details);
	auto *email = new QLineEdit(&dialog);
	email->setPlaceholderText(QStringLiteral("Email address for a reply (optional)"));
	email->setAccessibleName(QStringLiteral("Reply email (optional)"));
	email->setMaxLength(254);
	email->hide();
	layout->addWidget(email);
	QObject::connect(details, &QToolButton::toggled, &dialog, [details, email](bool expanded) {
		details->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
		email->setVisible(expanded);
	});
	auto *status = new QLabel(&dialog);
	status->setWordWrap(true);
	status->hide();
	layout->addWidget(status);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
	auto *send = buttons->addButton(QStringLiteral("Send Feedback"), QDialogButtonBox::ActionRole);
	send->setEnabled(false);
	layout->addWidget(buttons);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	const auto emailValid = [email] {
		return email->text().trimmed().isEmpty() ||
		       QRegularExpression(QStringLiteral("^[^\\s@]+@[^\\s@]+\\.[^\\s@]+$"))
			       .match(email->text().trimmed())
			       .hasMatch();
	};
	const auto validate = [=] {
		send->setEnabled(type->currentIndex() > 0 && !message->toPlainText().trimmed().isEmpty() &&
				 ratings->checkedId() > 0 && emailValid());
	};
	QObject::connect(type, &QComboBox::currentIndexChanged, &dialog, validate);
	QObject::connect(message, &QTextEdit::textChanged, &dialog, validate);
	QObject::connect(ratings, &QButtonGroup::idToggled, &dialog, validate);
	QObject::connect(email, &QLineEdit::textChanged, &dialog, validate);
	auto *network = new QNetworkAccessManager(&dialog);
	QObject::connect(send, &QPushButton::clicked, &dialog, [=, &dialog] {
		const int rating = ratings->checkedId();
		if (type->currentIndex() <= 0 || message->toPlainText().trimmed().isEmpty() || rating <= 0 ||
		    !emailValid())
			return;
		QString reason =
			QStringLiteral("[%1] %2 | satisfaction: %3/5 (%4) | platform: obs_plugin | version: %5")
				.arg(type->currentData().toString(), message->toPlainText().trimmed())
				.arg(rating)
				.arg(levels[rating - 1], QString::fromUtf8(PLUGIN_VERSION));
		if (!email->text().trimmed().isEmpty())
			reason += QStringLiteral(" | email: ") + email->text().trimmed();
		QNetworkRequest request(
			QUrl(QStringLiteral("https://izxjmtbo5b.execute-api.us-east-1.amazonaws.com/prod/report-ai")));
		request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
		request.setTransferTimeout(30000);
		const QJsonObject body{{QStringLiteral("content_type"), QStringLiteral("feedback")},
				       {QStringLiteral("reason"), reason},
				       {QStringLiteral("content_snippet"), QString()},
				       {QStringLiteral("job_id"), QJsonValue(QJsonValue::Null)}};
		for (QWidget *field : QList<QWidget *>{type, message, email, details})
			field->setEnabled(false);
		for (auto *radio : ratings->buttons())
			radio->setEnabled(false);
		send->setEnabled(false);
		send->setText(QStringLiteral("Sending…"));
		status->hide();
		auto *reply = network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
		QObject::connect(reply, &QNetworkReply::finished, &dialog, [=, &dialog] {
			const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
			const bool ok = reply->error() == QNetworkReply::NoError && code >= 200 && code < 300;
			reply->deleteLater();
			status->setText(
				ok ? QStringLiteral("Thank you! Your feedback helps us improve Pixelion Captions.")
				   : QStringLiteral("Could not send feedback. Check your connection and try again."));
			status->show();
			if (ok) {
				send->hide();
				buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Done"));
			} else {
				for (QWidget *field : QList<QWidget *>{type, message, email, details})
					field->setEnabled(true);
				for (auto *radio : ratings->buttons())
					radio->setEnabled(true);
				send->setText(QStringLiteral("Send Feedback"));
				validate();
			}
		});
	});
	type->setFocus();
	dialog.exec();
}
