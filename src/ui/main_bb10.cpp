#include "todoist/ui/cascades_app_bridge.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>
#include <bb/cascades/AbstractPane>
#include <bb/cascades/Application>
#include <bb/cascades/Control>
#include <bb/cascades/QmlDocument>
#include <bb/cascades/SceneCover>

int main(int argc, char** argv) {
    bb::cascades::Application app(argc, argv);
    QCoreApplication::setOrganizationName("TodoistPassport");
    QCoreApplication::setApplicationName("Todoist");

    QString dataDir = QDir::homePath();
    QDir dir(dataDir);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    QString dbPath = dataDir + "/todoist_local.db";

    QString caPath = QCoreApplication::applicationDirPath() + "/certs/cacert.pem";
    if (!QFileInfo(caPath).exists()) {
        caPath = "app/native/certs/cacert.pem";
    }

    todoist::CascadesAppBridge bridge(dbPath, caPath, &app);

    // Load Active Frame SceneCover (minimized home screen tile)
    bb::cascades::QmlDocument* coverQml =
        bb::cascades::QmlDocument::create("asset:///ActiveFrameCover.qml").parent(&app);
    if (coverQml && !coverQml->hasErrors()) {
        coverQml->setContextProperty("appBridge", &bridge);
        bb::cascades::Control* coverContent = coverQml->createRootObject<bb::cascades::Control>();
        if (coverContent) {
            bb::cascades::SceneCover* cover =
                bb::cascades::SceneCover::create().content(coverContent);
            app.setCover(cover);
        }
    }

    // Load Primary 1440x1440 Square Viewport Scene
    bb::cascades::QmlDocument* mainQml =
        bb::cascades::QmlDocument::create("asset:///main.qml").parent(&app);
    if (!mainQml || mainQml->hasErrors()) {
        return 1;
    }
    mainQml->setContextProperty("appBridge", &bridge);

    bb::cascades::AbstractPane* rootPane = mainQml->createRootObject<bb::cascades::AbstractPane>();
    if (!rootPane) {
        return 1;
    }
    app.setScene(rootPane);

    QObject::connect(&app, SIGNAL(fullscreen()), &bridge, SLOT(onFullscreen()));
    QObject::connect(&app, SIGNAL(thumbnail()), &bridge, SLOT(onThumbnail()));
    QObject::connect(&app, SIGNAL(invisible()), &bridge, SLOT(onInvisible()));
    QObject::connect(&app, SIGNAL(aboutToQuit()), &bridge, SLOT(onAboutToQuit()));

    QTimer::singleShot(0, &bridge, SLOT(initialize()));

    return bb::cascades::Application::exec();
}
