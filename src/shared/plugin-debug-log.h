#pragma once

#include <QString>

namespace PixelionDebugLog {

QString filePath();
void write(const char *level, const QString &message);

} // namespace PixelionDebugLog
