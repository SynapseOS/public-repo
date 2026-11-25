/*
    synapse-agent/os_tools.cpp
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI musl
    Статус доступа: общедоступный

    Реализация инструментов для работы с ОС
    Подсистема: пользовательские приложения
    Первое изменение: 24.11.2026
    Последнее изменение: 25.11.2026
*/

#include "os_tools.h"

#include <QBuffer>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHostAddress>
#include <QJsonArray>
#include <QPainter>
#include <QPixmap>
#include <QPolygon>
#include <QProcess>
#include <QRegularExpression>
#include <QScreen>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>
#include <QWidget>
#include <sys/stat.h>

OSTools::OSTools()
    : shellPath("/bin/sh") {
}

ShellResult OSTools::runShell(const QString& command) {
    ShellResult result;
    result.exitCode = -1;

    QProcess process;
    process.start(shellPath, QStringList() << "-c" << command);
    if (!process.waitForStarted(5000)) {
        result.stderrText = "Не удалось запустить оболочку";
        return result;
    }

    if (!process.waitForFinished(120000)) {
        process.terminate();
        result.stderrText = "Команда превысила таймаут";
        return result;
    }

    result.exitCode = process.exitCode();
    result.stdoutText = QString::fromUtf8(process.readAllStandardOutput());
    result.stderrText = QString::fromUtf8(process.readAllStandardError());
    return result;
}

QString OSTools::readFile(const QString& path, bool& ok) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        ok = false;
        return QString();
    }

    // Ограничить размер чтения для безопасности
    const qint64 maxRead = 1024 * 1024;
    QTextStream stream(&file);
    QString content = stream.read(maxRead);
    ok = true;
    return content;
}

bool OSTools::writeFile(const QString& path, const QString& content) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream stream(&file);
    stream << content;
    return true;
}

QString OSTools::listDirectory(const QString& path) {
    QDir dir(path);
    if (!dir.exists()) {
        return "Каталог не найден";
    }

    QFileInfoList entries = dir.entryInfoList(
        QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot,
        QDir::Name | QDir::DirsFirst);

    QStringList result;
    for (const auto& info : entries) {
        QString prefix = info.isDir() ? "d " : "- ";
        QString size = info.isDir() ? "" : QString(" %1").arg(info.size());
        result.append(prefix + QString::number(info.permissions(), 16) + size + " " + info.fileName());
    }

    return result.join("\n");
}

QJsonObject OSTools::systemInfo() {
    QJsonObject info;

    QProcess uname;
    uname.start("uname", QStringList() << "-a");
    if (uname.waitForFinished(5000)) {
        info.insert("uname", QString::fromUtf8(uname.readAllStandardOutput()).trimmed());
    }

    QFile meminfo("/proc/meminfo");
    if (meminfo.open(QIODevice::ReadOnly)) {
        QTextStream stream(&meminfo);
        QString mem = stream.read(2048);
        info.insert("meminfo", mem);
    }

    QProcess uptime;
    uptime.start("uptime");
    if (uptime.waitForFinished(5000)) {
        info.insert("uptime", QString::fromUtf8(uptime.readAllStandardOutput()).trimmed());
    }

    // Количество CPU из /proc/cpuinfo
    QFile cpuinfo("/proc/cpuinfo");
    if (cpuinfo.open(QIODevice::ReadOnly)) {
        QTextStream stream(&cpuinfo);
        QString data = stream.readAll();
        int count = data.count("processor");
        info.insert("cpu_count", count);
    }

    return info;
}

QString OSTools::getSetting(const QString& section, const QString& key, bool& ok) {
    QFile file("/etc/settings.ini");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        ok = false;
        return QString();
    }

    QTextStream stream(&file);
    QString currentSection;
    QRegularExpression sectionRegex("^\\[(.+)\\]\\s*$");
    QRegularExpression keyRegex("^\\s*([^#;=]+)\\s*=\\s*(.*)$");

    while (!stream.atEnd()) {
        QString line = stream.readLine();
        QRegularExpressionMatch sectionMatch = sectionRegex.match(line);
        if (sectionMatch.hasMatch()) {
            currentSection = sectionMatch.captured(1).trimmed();
            continue;
        }

        QRegularExpressionMatch keyMatch = keyRegex.match(line);
        if (keyMatch.hasMatch() && currentSection == section) {
            QString currentKey = keyMatch.captured(1).trimmed();
            if (currentKey == key) {
                ok = true;
                return keyMatch.captured(2).trimmed();
            }
        }
    }

    ok = false;
    return QString();
}

bool OSTools::setSetting(const QString& section, const QString& key, const QString& value) {
    QFile file("/etc/settings.ini");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream in(&file);
    QStringList lines;
    QString currentSection;
    bool sectionFound = false;
    bool keyReplaced = false;
    QRegularExpression sectionRegex("^\\[(.+)\\]\\s*$");

    while (!in.atEnd()) {
        QString line = in.readLine();
        QRegularExpressionMatch sectionMatch = sectionRegex.match(line);
        if (sectionMatch.hasMatch()) {
            if (sectionFound && !keyReplaced) {
                lines.append(key + " = " + value);
                keyReplaced = true;
            }
            currentSection = sectionMatch.captured(1).trimmed();
            if (currentSection == section) {
                sectionFound = true;
            }
        }

        QRegularExpression keyRegex("^\\s*" + QRegularExpression::escape(key) + "\\s*=.*$");
        if (currentSection == section && keyRegex.match(line).hasMatch()) {
            lines.append(key + " = " + value);
            keyReplaced = true;
            continue;
        }

        lines.append(line);
    }

    if (!sectionFound) {
        lines.append("[" + section + "]");
        lines.append(key + " = " + value);
        keyReplaced = true;
    } else if (!keyReplaced) {
        lines.append(key + " = " + value);
    }

    file.close();
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        return false;
    }

    QTextStream out(&file);
    for (const QString& line : lines) {
        out << line << "\n";
    }

    return true;
}

QString OSTools::findFiles(const QString& pattern, const QString& directory) {
    QDir dir(directory.isEmpty() ? "/" : directory);
    QStringList nameFilters;
    nameFilters << pattern;

    QStringList found;
    QDirIterator it(dir.path(), nameFilters,
                    QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);

    int count = 0;
    while (it.hasNext() && count < 100) {
        found.append(it.next());
        count++;
    }

    if (found.isEmpty()) {
        return "Файлы не найдены";
    }

    return found.join("\n");
}

QString OSTools::searchInFiles(const QString& pattern, const QString& path, int maxResults) {
    QProcess process;
    int limit = qBound(1, maxResults, 100);
    process.start("/bin/grep", {"-rn", "--binary-files=without-match",
                                "--max-count=" + QString::number(limit), "--", pattern, path});
    if (!process.waitForStarted(5000)) {
        return "Не удалось запустить поиск";
    }
    if (!process.waitForFinished(15000)) {
        process.kill();
        process.waitForFinished(1000);
        return "Поиск превысил таймаут";
    }

    QString output = QString::fromUtf8(process.readAllStandardOutput());
    if (output.trimmed().isEmpty()) {
        return "Совпадений не найдено";
    }

    return output.split('\n').mid(0, limit).join('\n').trimmed();
}

QString OSTools::listProcesses() {
    // Чтение /proc для списка процессов
    QDir procDir("/proc");
    QStringList pids = procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    QStringList result;
    result.append(QString("%1 %2 %3 %4").arg("PID", 7).arg("STATE", 6).arg("MEM(KB)", 9).arg("CMD"));

    for (const QString& pid : pids) {
        bool isNumber = false;
        pid.toInt(&isNumber);
        if (!isNumber) {
            continue;
        }

        QFile cmdline("/proc/" + pid + "/cmdline");
        QFile stat("/proc/" + pid + "/stat");

        QString cmd;
        if (cmdline.open(QIODevice::ReadOnly)) {
            QByteArray data = cmdline.readAll();
            cmd = QString::fromUtf8(data).replace('\0', ' ').trimmed();
            cmdline.close();
        }

        if (cmd.isEmpty()) {
            continue;
        }

        QString state = "?";
        QString memKb = "0";
        if (stat.open(QIODevice::ReadOnly)) {
            QString statData = QString::fromUtf8(stat.readAll());
            QStringList parts = statData.split(' ');
            if (parts.size() > 3) {
                state = parts[2];
            }
            if (parts.size() > 23) {
                memKb = QString::number(parts[23].toLongLong() / 1024);
            }
            stat.close();
        }

        result.append(QString("%1 %2 %3 %4")
            .arg(pid, 7)
            .arg(state, 6)
            .arg(memKb, 9)
            .arg(cmd.left(80)));
    }

    return result.join("\n");
}

QString OSTools::killProcess(int pid) {
    if (pid <= 0) {
        return "Некорректный PID";
    }

    ShellResult result = runShell("kill " + QString::number(pid));
    if (result.exitCode == 0) {
        return "Процесс " + QString::number(pid) + " завершён";
    }

    return "Не удалось завершить: " + result.stderrText.trimmed();
}

QJsonObject OSTools::networkInfo() {
    QJsonObject info;

    // IP адреса через /proc/net/if_inet6 и ip addr
    ShellResult ipResult = runShell("ip -brief addr 2>/dev/null || ifconfig 2>/dev/null");
    if (!ipResult.stdoutText.isEmpty()) {
        info.insert("interfaces", ipResult.stdoutText.trimmed());
    }

    // DNS из /etc/settings.ini
    bool dnsOk = false;
    int dnsIdx = 1;
    QJsonArray dnsServers;
    while (dnsOk || dnsIdx == 1) {
        dnsOk = false;
        QString dns = getSetting("DNS", QString::number(dnsIdx), dnsOk);
        if (!dnsOk || dns.isEmpty()) {
            break;
        }
        dnsServers.append(dns);
        dnsIdx++;
    }

    if (!dnsServers.isEmpty()) {
        info.insert("dns", dnsServers);
    }

    // Маршрут по умолчанию
    ShellResult routeResult = runShell("ip route show default 2>/dev/null");
    if (!routeResult.stdoutText.isEmpty()) {
        info.insert("default_route", routeResult.stdoutText.trimmed());
    }

    return info;
}

QString OSTools::pingHost(const QString& host, int count) {
    int c = (count > 0 && count <= 10) ? count : 4;
    QProcess process;
    process.start("/bin/ping", {"-c", QString::number(c), "-W", "5", "--", host});
    if (!process.waitForStarted(5000) || !process.waitForFinished(30000)) {
        process.kill();
        process.waitForFinished(1000);
        return "Не удалось завершить ping";
    }

    QString output = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    return process.exitCode() == 0
        ? output
        : "Хост недоступен: " + QString::fromUtf8(process.readAllStandardError()).trimmed();
}

QString OSTools::packageList() {
    ShellResult result = runShell("setup list 2>/dev/null || echo 'setup не доступен'");
    return result.stdoutText.trimmed();
}

QString OSTools::packageInstall(const QString& name) {
    QProcess process;
    process.start("/bin/setup", {"install", name, "--yes"});
    if (!process.waitForStarted(5000) || !process.waitForFinished(120000)) {
        process.kill();
        process.waitForFinished(1000);
        return "Установка пакета превысила таймаут";
    }

    return process.exitCode() == 0
        ? "Пакет " + name + " установлен"
        : "Ошибка установки: " + QString::fromUtf8(process.readAllStandardError()).trimmed();
}

QString OSTools::sendLanMessage(const QString& ip, const QString& message) {
    QHostAddress address;
    QByteArray data = message.toUtf8();
    if (!address.setAddress(ip) || address.protocol() != QAbstractSocket::IPv4Protocol ||
        data.isEmpty() || data.size() > 8000) {
        return "Нужны IPv4-адрес и сообщение не длиннее 8000 байт";
    }

    QProcess process;
    process.start("/bin/lan", {"msg", ip, message});
    if (!process.waitForStarted(5000) || !process.waitForFinished(15000)) {
        process.kill();
        process.waitForFinished(1000);
        return "LAN-сервис не ответил";
    }

    if (process.exitCode() != 0) {
        return "Сообщение не отправлено: " +
               QString::fromUtf8(process.readAllStandardError()).trimmed();
    }

    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}

QString OSTools::diskUsage() {
    ShellResult result = runShell("df -h 2>/dev/null || cat /proc/mounts");
    return result.stdoutText.trimmed();
}

QJsonObject OSTools::memoryInfo() {
    QJsonObject info;

    QFile meminfo("/proc/meminfo");
    if (!meminfo.open(QIODevice::ReadOnly)) {
        return info;
    }

    QTextStream stream(&meminfo);
    QRegularExpression regex("^(\\S+):\\s+(\\d+)");

    while (!stream.atEnd()) {
        QString line = stream.readLine();
        QRegularExpressionMatch match = regex.match(line);
        if (match.hasMatch()) {
            QString key = match.captured(1);
            qlonglong value = match.captured(2).toLongLong();
            info.insert(key, value);
        }
    }

    return info;
}

ScreenshotResult OSTools::takeScreenshot(const QString& windowTitle) {
    ScreenshotResult result;
    result.ok = false;

    QPixmap pixmap;

    QTemporaryDir temporary;
    if (temporary.isValid()) {
        QString source = temporary.path() + "/screen.png";
        QProcess recorder;
        recorder.start("/bin/recorder", {"shot", source, "--screen", "wayland"});
        if (recorder.waitForStarted(3000) && recorder.waitForFinished(10000) &&
            recorder.exitCode() == 0) {
            pixmap.load(source);
        }
    }

    if (pixmap.isNull() || pixmap.width() < 2) {
        QScreen* screen = QGuiApplication::primaryScreen();
        if (screen) {
            pixmap = screen->grabWindow(0);
        }
        if (pixmap.isNull() || pixmap.width() < 2) {
            result.error = "Не удалось сделать скриншот";
            return result;
        }
    }

    // Сжать изображение до разумного размера для API
    if (pixmap.width() > 1280 || pixmap.height() > 800) {
        pixmap = pixmap.scaled(1280, 800, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    // Сохранить в память и закодировать в base64
    QByteArray pngData;
    QBuffer buffer(&pngData);
    buffer.open(QIODevice::WriteOnly);
    pixmap.save(&buffer, "PNG");

    if (pngData.isEmpty()) {
        result.error = "Не удалось закодировать изображение";
        return result;
    }

    QString folder = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/screenshots";
    if (!QDir().mkpath(folder) || chmod(QFile::encodeName(folder).constData(), 0700) != 0) {
        result.error = "Не удалось создать каталог снимков";
        return result;
    }
    QString path = folder + "/screen-" +
        QDateTime::currentDateTimeUtc().toString("yyyyMMdd-hhmmsszzz") + ".png";
    if (!pixmap.save(path, "PNG") || chmod(QFile::encodeName(path).constData(), 0600) != 0) {
        result.error = "Не удалось сохранить снимок";
        return result;
    }

    result.ok = true;
    result.path = path;
    result.attachment.type = "image";
    result.attachment.mediaType = "image/png";
    result.attachment.data = QString::fromLatin1(pngData.toBase64());

    // Подсказка про указанное окно: захват всего экрана
    if (!windowTitle.isEmpty()) {
        result.path = path;
    }

    return result;
}

QString OSTools::setCursorTheme(const QString& theme, int size) {
    int cursorSize = (size > 0 && size <= 128) ? size : 24;

    // Установить переменные окружения для текущего процесса и новых окон
    qputenv("XCURSOR_THEME", theme.toUtf8());
    qputenv("XCURSOR_SIZE", QByteArray::number(cursorSize));

    // Обновить конфигурацию labwc если каталог существует
    QString labwcDir = QDir::homePath() + "/.config/labwc";
    if (QDir(labwcDir).exists()) {
        QString rcPath = labwcDir + "/rc.xml";
        QFile rc(rcPath);
        if (rc.open(QIODevice::ReadOnly)) {
            QString content = QString::fromUtf8(rc.readAll());
            rc.close();

            // Заменить или добавить блок cursor в theme
            QRegularExpression cursorRegex("<cursor>.*?</cursor>",
                QRegularExpression::DotMatchesEverythingOption);
            QString newCursor = "<cursor><name>" + theme + "</name><size>" +
                QString::number(cursorSize) + "</size></cursor>";

            if (content.contains("<theme>")) {
                if (content.contains(cursorRegex)) {
                    content.replace(cursorRegex, newCursor);
                } else {
                    content.replace("<theme>", "<theme>\n      " + newCursor);
                }

                if (rc.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    rc.write(content.toUtf8());
                    rc.close();
                }
            }
        }
    }

    return "Тема курсора установлена: " + theme +
        " (размер " + QString::number(cursorSize) +
        ", вступит в силу для новых окон и после перезапуска композитора)";
}

// Сгенерировать собственный курсор агента: стрелка с фирменной подсветкой
static QPixmap makeAgentCursorPixmap() {
    const int w = 28;
    const int h = 28;
    QPixmap pixmap(w, h);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    // Классический контур стрелки
    QPolygon arrow;
    arrow << QPoint(1, 1)
          << QPoint(1, 20)
          << QPoint(6, 15)
          << QPoint(9, 22)
          << QPoint(12, 20)
          << QPoint(9, 14)
          << QPoint(16, 14);

    // Тёмная обводка для контраста на любом фоне
    QPolygon outline = arrow;
    outline.translate(1, 0);
    painter.setPen(QPen(QColor(0, 0, 0, 200), 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(QBrush(QColor(0, 0, 0, 200)));
    painter.drawPolygon(outline);
    outline.translate(-2, 0);
    painter.drawPolygon(outline);
    outline.translate(1, 1);
    painter.drawPolygon(outline);
    outline.translate(0, -2);
    painter.drawPolygon(outline);

    // Основная заливка: белый с бирюзовой каймой SynapseOS
    painter.setPen(QPen(QColor(0, 200, 190), 1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(QBrush(Qt::white));
    painter.drawPolygon(arrow);

    painter.end();
    return pixmap;
}

void OSTools::applyAgentCursor(QWidget* widget) {
    if (!widget) {
        return;
    }

    QPixmap pixmap = makeAgentCursorPixmap();
    if (pixmap.isNull()) {
        return;
    }

    // Точка клика: кончик стрелки
    QCursor cursor(pixmap, 1, 1);
    widget->setCursor(cursor);
}
