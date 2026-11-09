#pragma once

#include <QString>

// Maps the feedback dialog category id to a `type` accepted by POST /feedback on
// api.pixelion.ai. The server rejects unknown values with HTTP 400, so every dialog
// category must map to one of: bug, idea, praise, general (ai_content_report is
// not offered by this dialog). Platform-neutral and covered by tests.
inline QString feedbackTypeForServer(const QString &category)
{
	if (category == QStringLiteral("bug"))
		return QStringLiteral("bug");
	if (category == QStringLiteral("feature"))
		return QStringLiteral("idea");
	if (category == QStringLiteral("compliment"))
		return QStringLiteral("praise");
	return QStringLiteral("general");
}
