/*
    synapse-agent/os_tools.h
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI musl
    Статус доступа: общедоступный

    Инструменты для работы с операционной системой
    Подсистема: пользовательские приложения
    Первое изменение: 24.11.2026
    Последнее изменение: 24.11.2026
*/

#ifndef OS_TOOLS_H
#define OS_TOOLS_H

#include "ai_provider.h"
#include <QString>
#include <QJsonObject>

// Результат выполнения команды оболочки
struct ShellResult {
    int exitCode;
    QString stdoutText;
    QString stderrText;
};

// Результат скриншота: путь и вложение для отправки модели
struct ScreenshotResult {
    bool ok;
    QString path;
    QString error;
    Attachment attachment;
};

// Набор инструментов для работы с ОС
class OSTools {
public:
    OSTools();

    // Выполнить команду в оболочке
    ShellResult runShell(const QString& command);

    // Прочитать файл
    QString readFile(const QString& path, bool& ok);

    // Записать файл
    bool writeFile(const QString& path, const QString& content);

    // Получить список файлов в каталоге
    QString listDirectory(const QString& path);

    // Системная информация
    QJsonObject systemInfo();

    // Прочитать параметр из /etc/settings.ini
    QString getSetting(const QString& section, const QString& key, bool& ok);

    // Установить параметр в /etc/settings.ini
    bool setSetting(const QString& section, const QString& key, const QString& value);

    // Найти файлы по шаблону имени
    QString findFiles(const QString& pattern, const QString& directory);

    // Поиск текста в файлах (аналог grep)
    QString searchInFiles(const QString& pattern, const QString& path, int maxResults);

    // Список процессов
    QString listProcesses();

    // Завершить процесс по PID
    QString killProcess(int pid);

    // Сетевая информация: интерфейсы и адреса
    QJsonObject networkInfo();

    // Ping хоста
    QString pingHost(const QString& host, int count);

    // Список установленных пакетов
    QString packageList();

    // Установить пакет
    QString packageInstall(const QString& name);

    // Информация о дисках
    QString diskUsage();

    // Подробная информация о памяти
    QJsonObject memoryInfo();

    // Сделать скриншот экрана и вернуть вложение для модели
    ScreenshotResult takeScreenshot(const QString& windowTitle);

    // Установить тему курсора системы
    QString setCursorTheme(const QString& theme, int size);

    // Установить системный указатель мыши для окна приложения
    static void applyAgentCursor(class QWidget* widget);

private:
    QString shellPath;
};

#endif
