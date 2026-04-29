#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>

struct GroupInfo {
    QString id;
    QString name;
    QStringList members;
};

Q_DECLARE_METATYPE(GroupInfo)
Q_DECLARE_METATYPE(QList<GroupInfo>)
