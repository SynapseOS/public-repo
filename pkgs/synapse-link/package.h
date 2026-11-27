// package.h: Чтение телефонного приложения из SPKG без распаковки на диск
// Автор: Елчинян Арен Артурович, 2026, Qt6 x86_64, подсистема USB
// Первое и последнее изменение 27.11.2026, доступ общедоступный
#pragma once
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <lumine/phone-link.h>

// В телефонном профиле исполняемый файл использует общий runtime телефона
static QByteArray phoneExecutable(QFile &file) {
    if (file.size() < 64 || file.size() > SYN_LINK_LIMIT + 65536) return {};
    QByteArray raw = file.read(SYN_LINK_LIMIT + 65537);
    if (raw.size() != file.size()) return {};
    if (raw.startsWith("\x7f" "ELF")) return raw;
    QMap<QByteArray,QByteArray> entries;
    qsizetype offset = 0;
    while (offset + 512 <= raw.size()) {
        QByteArray header = raw.mid(offset,512);
        if (header == QByteArray(512,'\0')) break;
        bool valid = false;
        const auto size = header.mid(124,12).split('\0').first().trimmed().toUInt(&valid,8);
        if (!valid || size > SYN_LINK_LIMIT || size > raw.size()-offset-512 || entries.size() > 16) return {};
        const auto name = header.left(100).split('\0').first();
        const auto type = header[156];
        if (type == '0' || type == 0) {
            if (entries.contains(name)) return {};
            entries.insert(name,raw.mid(offset+512,size));
        } else if (type != '5') return {};
        offset += 512 + ((size+511)/512)*512;
    }
    const auto manifest = QJsonDocument::fromJson(entries.value("manifest.json")).object();
    if (manifest.value("format").toInt() != 1 || manifest.value("arch").toString() != "aarch64" ||
        manifest.value("kind").toString() != "application") return {};
    QByteArray binary;
    for (const QJsonValue &item : manifest.value("files").toArray()) {
        const auto row = item.toObject();
        if (row.value("type").toString() != "file") continue;
        const QByteArray path = row.value("path").toString().toUtf8();
        if (path.startsWith("usr/share/licenses/")) continue;
        if (!path.startsWith("bin/") || path.count('/') != 1 || !(row.value("mode").toInt() & 0111) ||
            !binary.isEmpty()) return {};
        binary = entries.value("payload/"+path);
        if (row.value("size").toInteger() != binary.size() ||
            QCryptographicHash::hash(binary,QCryptographicHash::Sha256).toHex() != row.value("sha256").toString().toLatin1()) return {};
    }
    return binary;
}
