#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "tls-backend.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>

namespace PixelionPlatform {
void registerBundledQtTlsBackend()
{
	HMODULE module = nullptr;
	const auto moduleAddress = reinterpret_cast<LPCWSTR>(reinterpret_cast<uintptr_t>(&registerBundledQtTlsBackend));
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				moduleAddress, &module))
		return;
	wchar_t modulePath[MAX_PATH] = {};
	const DWORD pathLength = GetModuleFileNameW(module, modulePath, MAX_PATH);
	if (pathLength == 0 || pathLength >= MAX_PATH)
		return;
	const QDir pluginDirectory = QFileInfo(QString::fromWCharArray(modulePath)).absoluteDir();
	if (QDir(pluginDirectory.filePath(QStringLiteral("tls"))).exists())
		QCoreApplication::addLibraryPath(pluginDirectory.absolutePath());
}
} // namespace PixelionPlatform
