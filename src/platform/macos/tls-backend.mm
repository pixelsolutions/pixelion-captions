#include <dlfcn.h>

#include "platform/tls-backend.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>

namespace PixelionPlatform {
    // macOS analog of src/platform/windows/tls-backend.cpp.
    //
    // On Windows the plugin binary lives in .../bin/64bit/ and Qt's SChannel TLS
    // backend ships in a "tls" subdirectory beside it; the Windows code locates its
    // own module directory and, when that "tls" subdirectory exists, adds the module
    // directory to Qt's library path so Qt discovers the bundled backend.
    //
    // On macOS the plugin binary lives at <Plugin>.plugin/Contents/MacOS/<binary>,
    // and the bundled Qt macOS TLS backend plugin ships in a "tls" subdirectory
    // beside that binary (task 6.1 places it there). We use dladdr on the address of
    // this function to resolve the dylib path (Contents/MacOS/<binary>), take its
    // containing directory, and -- only when the sibling "tls" subdirectory exists,
    // matching the Windows guard -- register that directory on Qt's library path.
    // Qt then finds the bundled TLS backend under the "tls" subdir at load, so
    // QNetworkAccessManager (HTTPS) and QWebSocket (WSS) have a working TLS backend
    // with no user-installed dependency.
    //
    // This must run before any QNetworkAccessManager/QWebSocket use; it is invoked
    // from obs_module_load() in shared plugin-main.cpp.
    void registerBundledQtTlsBackend()
    {
        Dl_info info = {};
        if (dladdr(reinterpret_cast<const void *>(&registerBundledQtTlsBackend), &info) == 0)
            return;
        if (info.dli_fname == nullptr)
            return;

        const QDir pluginDirectory = QFileInfo(QString::fromUtf8(info.dli_fname)).absoluteDir();
        if (QDir(pluginDirectory.filePath(QStringLiteral("tls"))).exists())
            QCoreApplication::addLibraryPath(pluginDirectory.absolutePath());
    }
}  // namespace PixelionPlatform
