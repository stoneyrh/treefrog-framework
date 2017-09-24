/* Copyright (c) 2010-2017, AOYAMA Kazuharu
 * All rights reserved.
 *
 * This software may be used and distributed according to the terms of
 * the New BSD License, which is incorporated herein by reference.
 */

#include <QTextCodec>
#include <QStringList>
#include <QMap>
#include <TWebApplication>
#include <TAppSettings>
#include <TThreadApplicationServer>
#include <TMultiplexingServer>
#if QT_VERSION >= 0x050000
# include <TJSLoader>
#endif
#include <TSystemGlobal>
#include <cstdlib>
#include "thazardptrmanager.h"
#include "tsystemglobal.h"
#include "signalhandler.h"
using namespace TreeFrog;

#define DEBUG_MODE_OPTION  "--debug"
#define SOCKET_OPTION      "-s"
#define AUTO_RELOAD_OPTION "-r"


#if QT_VERSION >= 0x050000
static void messageOutput(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    QByteArray msg = message.toLocal8Bit();
    switch (type) {
    case QtFatalMsg:
    case QtCriticalMsg:
        tSystemError("%s (%s:%u %s)", msg.constData(), context.file, context.line, context.function);
        break;
    case QtWarningMsg:
        tSystemWarn("%s (%s:%u %s)", msg.constData(), context.file, context.line, context.function);
        break;
    case QtDebugMsg:
        tSystemDebug("%s (%s:%u %s)", msg.constData(), context.file, context.line, context.function);
        break;
    default:
        break;
    }
}
#else
static void messageOutput(QtMsgType type, const char *msg)
{
    switch (type) {
    case QtFatalMsg:
    case QtCriticalMsg:
        tSystemError("%s", msg);
        break;
    case QtWarningMsg:
        tSystemWarn("%s", msg);
        break;
    case QtDebugMsg:
        tSystemDebug("%s", msg);
        break;
    default:
        break;
    }
}
#endif // QT_VERSION >= 0x050000

#if defined(Q_OS_UNIX)
static void writeFailure(const void *data, int size)
{
    tSystemError("%s", QByteArray((const char *)data, size).data());
}
#endif

static QMap<QString, QString> convertArgs(const QStringList &args)
{
    QMap<QString, QString> map;
    for (int i = 1; i < args.count(); ++i) {
        if (args.value(i).startsWith('-')) {
            if (args.value(i + 1).startsWith('-')) {
                map.insert(args.value(i), QString());
            } else {
                map.insert(args.value(i), args.value(i + 1));
                ++i;
            }
        }
    }
    return map;
}


int main(int argc, char *argv[])
{
    TWebApplication webapp(argc, argv);
    TApplicationServerBase *server = 0;
    int ret = -1;

    // Setup loggers
    Tf::setupSystemLogger();
    Tf::setupAccessLogger();
    Tf::setupQueryLogger();
    Tf::setupAppLoggers();

    // Setup hazard pointer
    THazardPtrManager::instance().setGarbageCollectionBufferSize(Tf::app()->maxNumberOfThreadsPerAppServer());

#if QT_VERSION >= 0x050000
    qInstallMessageHandler(messageOutput);
#else
    qInstallMsgHandler(messageOutput);
#endif
    QMap<QString, QString> args = convertArgs(QCoreApplication::arguments());
    int sock = args.value(SOCKET_OPTION).toInt();
    bool reload = args.contains(AUTO_RELOAD_OPTION);
    bool debug = args.contains(DEBUG_MODE_OPTION);

#if defined(Q_OS_UNIX)
    webapp.watchUnixSignal(SIGTERM);
    if (!debug) {
        webapp.ignoreUnixSignal(SIGINT);
    }

    // Setup signal handlers for SIGSEGV, SIGILL, SIGFPE, SIGABRT and SIGBUS
    setupFailureWriter(writeFailure);
    setupSignalHandler();

#elif defined(Q_OS_WIN)
    if (!debug) {
        webapp.ignoreConsoleSignal();
    }
#endif

    // Sets the app locale
    QString loc = Tf::appSettings()->value(Tf::Locale).toString().trimmed();
    if (!loc.isEmpty()) {
        QLocale locale(loc);
        QLocale::setDefault(locale);
        tSystemInfo("Application's default locale: %s", qPrintable(locale.name()));
    }

    // Sets codec
    QTextCodec *codec = webapp.codecForInternal();
    QTextCodec::setCodecForLocale(codec);

#if QT_VERSION < 0x050000
    QTextCodec::setCodecForTr(codec);
    QTextCodec::setCodecForCStrings(codec);
    tSystemDebug("setCodecForTr: %s", codec->name().data());
    tSystemDebug("setCodecForCStrings: %s", codec->name().data());
#endif

    if (!webapp.webRootExists()) {
        tSystemError("No such directory");
        fprintf(stderr, "No such directory\n");
        goto finish;
    }
    tSystemDebug("Web Root: %s", qPrintable(webapp.webRootPath()));

    if (!webapp.appSettingsFileExists()) {
        tSystemError("Settings file not found");
        fprintf(stderr, "Settings file not found\n");
        goto finish;
    } else {
#if QT_VERSION >= 0x050000
        // Sets search paths for JavaScript
        QStringList jpaths = Tf::appSettings()->value(Tf::JavaScriptPath, "script;node_modules").toString().split(';');
        TJSLoader::setDefaultSearchPaths(jpaths);
#endif
    }

#ifdef Q_OS_WIN
    if (sock <= 0)
#else
    if (sock <= 0 && debug)
#endif
    {
        int port = Tf::appSettings()->value(Tf::ListenPort).toInt();
        if (port <= 0 || port > USHRT_MAX) {
            tSystemError("Invalid port number: %d", port);
            fprintf(stderr, "Invalid port number: %d\n", port);
            goto finish;
        }
        // Listen address
        QString listenAddress = Tf::appSettings()->value(Tf::ListenAddress).toString();
        if (listenAddress.isEmpty()) {
            listenAddress = "0.0.0.0";
        }

        TApplicationServerBase::nativeSocketInit();
        sock = TApplicationServerBase::nativeListen(QHostAddress(listenAddress), port);
    }

    if (sock <= 0) {
        tSystemError("Invalid socket descriptor: %d", sock);
        fprintf(stderr, "Invalid option\n");
        goto finish;
    }

    switch (webapp.multiProcessingModule()) {
    case TWebApplication::Thread:
        server = new TThreadApplicationServer(sock, &webapp);
        break;

    case TWebApplication::Hybrid:
#ifdef Q_OS_LINUX
        // Sets a listening socket descriptor
        TMultiplexingServer::instantiate(sock);
        tSystemDebug("Set socket descriptor: %d", sock);
        server = TMultiplexingServer::instance();
#else
        tFatal("Unsupported MPM: hybrid");
#endif
        break;

    default:
        break;
    }

    server->setAutoReloadingEnabled(reload);
    if (!server->start(debug)) {
        tSystemError("Server open failed");
        fprintf(stderr, "Server open failed\n");
        goto finish;
    }

    ret = webapp.exec();
    server->stop();

finish:
    // Release loggers
    Tf::releaseAppLoggers();
    Tf::releaseQueryLogger();
    Tf::releaseAccessLogger();
    Tf::releaseSystemLogger();

    _exit(ret);
    return ret;
}
