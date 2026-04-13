#include <QApplication>
#include <QMetaType>

#include "model/chattypes.h"
#include "model/peerinfo.h"
#include "ui/mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("LanLinkChat"));
    QApplication::setOrganizationName(QStringLiteral("Exp4"));

    qRegisterMetaType<PeerInfo>("PeerInfo");
    qRegisterMetaType<QList<PeerInfo>>("QList<PeerInfo>");
    qRegisterMetaType<GroupInfo>("GroupInfo");
    qRegisterMetaType<QList<GroupInfo>>("QList<GroupInfo>");

    MainWindow window;
    window.show();
    return app.exec();
}
