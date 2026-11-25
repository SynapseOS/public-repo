/*
    synapse-agent/chat_agent.cpp
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI musl
    Статус доступа: общедоступный

    Реализация оркестратора диалога с инструментами, стримингом и контекстом
    Подсистема: пользовательские приложения
    Первое изменение: 24.11.2026
    Последнее изменение: 25.11.2026
*/

#include "chat_agent.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>
#include <sys/stat.h>

namespace {

// Создать JSON-схему инструмента
QJsonObject makeToolSchema(const QString& name, const QString& description,
                           const QJsonObject& parameters) {
    QJsonObject schema;
    schema.insert("name", name);
    schema.insert("description", description);
    schema.insert("parameters", parameters);
    return schema;
}

// Создать описание параметров
QJsonObject makeParameters(const std::vector<std::pair<QString, QString>>& required,
                           const std::vector<std::pair<QString, QString>>& optional = {}) {
    QJsonObject properties;
    QJsonArray requiredArr;

    for (const auto& p : required) {
        QJsonObject prop;
        prop.insert("type", "string");
        prop.insert("description", p.second);
        properties.insert(p.first, prop);
        requiredArr.append(p.first);
    }

    for (const auto& p : optional) {
        QJsonObject prop;
        prop.insert("type", "string");
        prop.insert("description", p.second);
        properties.insert(p.first, prop);
    }

    QJsonObject paramsObj;
    paramsObj.insert("type", "object");
    paramsObj.insert("properties", properties);
    paramsObj.insert("required", requiredArr);
    return paramsObj;
}

// Пустые параметры
QJsonObject noParameters() {
    QJsonObject paramsObj;
    paramsObj.insert("type", "object");
    paramsObj.insert("properties", QJsonObject());
    paramsObj.insert("required", QJsonArray());
    return paramsObj;
}

// Выбрать группу доступа для инструмента
QString permissionCategory(const QString& name) {
    if (name == "run_shell") {
        return "shell";
    }
    if (name == "write_file" || name == "set_setting" || name == "kill_process" ||
        name == "package_install" || name == "set_cursor_theme") {
        return "change";
    }
    if (name == "take_screenshot") {
        return "screen";
    }
    if (name == "lan_send") {
        return "lan";
    }
    if (name == "read_file" || name == "list_directory" || name == "get_setting" ||
        name == "system_info" || name == "find_files" || name == "search_in_files" ||
        name == "list_processes" || name == "network_info" || name == "ping_host" ||
        name == "package_list" || name == "disk_usage" || name == "memory_info") {
        return "read";
    }
    return QString();
}

// Показывать текст сразу, но скрывать служебные блоки вызова инструментов
class VisibleStream {
public:
    explicit VisibleStream(const StreamCallback& callback) : m_callback(callback) {}

    void add(const QString& chunk) {
        m_pending += chunk;
        drain(false);
    }

    void finish() {
        drain(true);
    }

private:
    void drain(bool flush) {
        while (!m_pending.isEmpty()) {
            if (m_insideTool) {
                const int end = m_pending.indexOf("</tool>");
                if (end < 0) {
                    m_pending = flush ? QString() : m_pending.right(6);
                    return;
                }
                m_pending.remove(0, end + 7);
                m_insideTool = false;
                continue;
            }

            const int begin = m_pending.indexOf("<tool>");
            if (begin >= 0) {
                sendVisible(m_pending.left(begin));
                m_pending.remove(0, begin + 6);
                m_insideTool = true;
                continue;
            }

            const int count = flush ? m_pending.size() : qMax(0, m_pending.size() - 5);
            sendVisible(m_pending.left(count));
            m_pending.remove(0, count);
            return;
        }
    }

    void sendVisible(const QString& text) {
        if (m_callback && !text.isEmpty()) {
            m_callback(text);
        }
    }

    StreamCallback m_callback;
    QString m_pending;
    bool m_insideTool = false;
};

} // namespace

ChatAgent::ChatAgent(std::unique_ptr<AIProvider> provider)
    : m_provider(std::move(provider))
    , m_tools(nullptr) {

    // Базовые инструменты
    ToolDescription runShell;
    runShell.name = "run_shell";
    runShell.description = "Выполнить команду в терминале SynapseOS";
    runShell.schema = makeToolSchema("run_shell", runShell.description,
        makeParameters({{"command", "Команда shell для выполнения"}}));

    ToolDescription readFile;
    readFile.name = "read_file";
    readFile.description = "Прочитать содержимое файла";
    readFile.schema = makeToolSchema("read_file", readFile.description,
        makeParameters({{"path", "Путь к файлу"}}));

    ToolDescription writeFileTool;
    writeFileTool.name = "write_file";
    writeFileTool.description = "Записать текст в файл";
    writeFileTool.schema = makeToolSchema("write_file", writeFileTool.description,
        makeParameters({{"path", "Путь к файлу"}, {"content", "Содержимое для записи"}}));

    ToolDescription listDir;
    listDir.name = "list_directory";
    listDir.description = "Получить список файлов в каталоге с размерами и правами";
    listDir.schema = makeToolSchema("list_directory", listDir.description,
        makeParameters({{"path", "Путь к каталогу"}}));

    ToolDescription getSetting;
    getSetting.name = "get_setting";
    getSetting.description = "Прочитать значение из /etc/settings.ini";
    getSetting.schema = makeToolSchema("get_setting", getSetting.description,
        makeParameters({{"section", "Название секции"}, {"key", "Название ключа"}}));

    ToolDescription setSetting;
    setSetting.name = "set_setting";
    setSetting.description = "Установить значение в /etc/settings.ini";
    setSetting.schema = makeToolSchema("set_setting", setSetting.description,
        makeParameters({{"section", "Название секции"}, {"key", "Название ключа"}, {"value", "Значение"}}));

    ToolDescription sysInfo;
    sysInfo.name = "system_info";
    sysInfo.description = "Получить информацию о системе: ядро, CPU, память, uptime";
    sysInfo.schema = makeToolSchema("system_info", sysInfo.description, noParameters());

    // Инструменты поиска
    ToolDescription findFilesTool;
    findFilesTool.name = "find_files";
    findFilesTool.description = "Найти файлы по шаблону имени в каталоге и подкаталогах";
    findFilesTool.schema = makeToolSchema("find_files", findFilesTool.description,
        makeParameters({{"pattern", "Шаблон имени файла (например *.log)"}},
                       {{"directory", "Начальный каталог (по умолчанию /)"}}));

    ToolDescription grep;
    grep.name = "search_in_files";
    grep.description = "Поиск текста в файлах (аналог grep -rn)";
    grep.schema = makeToolSchema("search_in_files", grep.description,
        makeParameters({{"pattern", "Шаблон поиска"}, {"path", "Путь для поиска"}},
                       {{"max_results", "Максимум результатов (по умолчанию 50)"}}));

    // Управление процессами
    ToolDescription ps;
    ps.name = "list_processes";
    ps.description = "Получить список запущенных процессов";
    ps.schema = makeToolSchema("list_processes", ps.description, noParameters());

    ToolDescription kill;
    kill.name = "kill_process";
    kill.description = "Завершить процесс по PID";
    kill.schema = makeToolSchema("kill_process", kill.description,
        makeParameters({{"pid", "Идентификатор процесса (число)"}}));

    // Сеть
    ToolDescription netInfo;
    netInfo.name = "network_info";
    netInfo.description = "Сетевые интерфейсы, IP адреса, DNS, маршруты";
    netInfo.schema = makeToolSchema("network_info", netInfo.description, noParameters());

    ToolDescription ping;
    ping.name = "ping_host";
    ping.description = "Проверить доступность хоста через ICMP";
    ping.schema = makeToolSchema("ping_host", ping.description,
        makeParameters({{"host", "Имя или IP хоста"}},
                       {{"count", "Количество пакетов (по умолчанию 4)"}}));

    // Пакеты
    ToolDescription pkgList;
    pkgList.name = "package_list";
    pkgList.description = "Получить список установленных пакетов";
    pkgList.schema = makeToolSchema("package_list", pkgList.description, noParameters());

    ToolDescription pkgInstall;
    pkgInstall.name = "package_install";
    pkgInstall.description = "Установить пакет через setup";
    pkgInstall.schema = makeToolSchema("package_install", pkgInstall.description,
        makeParameters({{"name", "Название пакета"}}));

    // Диски и память
    ToolDescription diskInfo;
    diskInfo.name = "disk_usage";
    diskInfo.description = "Информация о дисках и использовании пространства";
    diskInfo.schema = makeToolSchema("disk_usage", diskInfo.description, noParameters());

    ToolDescription memInfo;
    memInfo.name = "memory_info";
    memInfo.description = "Подробная информация об использовании памяти";
    memInfo.schema = makeToolSchema("memory_info", memInfo.description, noParameters());

    // Скриншот экрана с распознаванием через vision-модель
    ToolDescription screenshot;
    screenshot.name = "take_screenshot";
    screenshot.description = "Сделать скриншот экрана и прикрепить к следующему запросу "
                             "для анализа моделью с поддержкой изображений";
    screenshot.schema = makeToolSchema("take_screenshot", screenshot.description,
        makeParameters({},
                      {{"window_title", "Заголовок окна (необязательно, захватывается весь экран)"}}));

    // Тема курсора системы
    ToolDescription cursorTheme;
    cursorTheme.name = "set_cursor_theme";
    cursorTheme.description = "Установить тему указателя мыши и её размер";
    cursorTheme.schema = makeToolSchema("set_cursor_theme", cursorTheme.description,
        makeParameters({{"theme", "Название темы курсора, например default или Adwaita"}},
                       {{"size", "Размер курсора в пикселях (по умолчанию 24)"}}));

    ToolDescription lanSend;
    lanSend.name = "lan_send";
    lanSend.description = "Отправить сообщение другому агенту в локальной сети через lan msg";
    lanSend.schema = makeToolSchema("lan_send", lanSend.description,
        makeParameters({{"ip", "IPv4-адрес получателя"}, {"message", "Текст сообщения"}}));

    m_toolDescriptions = {
        runShell, readFile, writeFileTool, listDir, getSetting, setSetting, sysInfo,
        findFilesTool, grep, ps, kill, netInfo, ping, pkgList, pkgInstall, diskInfo, memInfo,
        screenshot, cursorTheme, lanSend
    };
}

// Обновить модель и оставить накопленный контекст
void ChatAgent::setProvider(std::unique_ptr<AIProvider> provider) {
    m_provider = std::move(provider);
}

void ChatAgent::setTools(OSTools* tools) {
    m_tools = tools;
}

void ChatAgent::setConfirmationCallback(const ConfirmationCallback& cb) {
    m_confirmCallback = cb;
}

void ChatAgent::setPermissionCallback(const PermissionCallback& cb) {
    m_permissionCallback = cb;
}

QString ChatAgent::buildSystemPrompt() const {
    QString prompt;
    prompt += "Ты ассистент операционной системы SynapseOS. ";
    prompt += "Отвечай на языке последнего сообщения пользователя, если он не попросил другой язык. ";
    prompt += "Ты можешь работать с файлами, терминалом, сетью, пакетами и настройками системы. ";
    prompt += "Для каждого инструмента пользователь задаёт разрешение и может отклонить запрос. ";
    prompt += "Для вызова инструмента используй блок <tool>{...}</tool>. ";
    prompt += "Формат: <tool>{\"name\": \"run_shell\", \"arguments\": {\"command\": \"ls -la\"}}</tool>. ";
    prompt += "Можно вызывать несколько инструментов в одном ответе. ";
    prompt += "После результата можно вызвать следующие инструменты. ";
    prompt += "Называй проверенным только вызов, для которого получен результат. ";
    prompt += "Пустой ответ или ошибку отмечай как непроверенный либо неуспешный вызов. ";
    prompt += "В итоговом ответе не показывай блоки <tool>.\n\n";
    prompt += "Доступные инструменты:\n";

    for (const auto& tool : m_toolDescriptions) {
        prompt += "- " + tool.name + ": " + tool.description + "\n";
        prompt += "  " + QString::fromUtf8(QJsonDocument(tool.schema).toJson(QJsonDocument::Compact)) + "\n";
    }

    return prompt;
}

QString ChatAgent::processUserMessage(const QString& userMessage, bool& ok) {
    if (!m_provider) {
        ok = false;
        return "Провайдер не выбран";
    }

    if (m_history.empty()) {
        Message systemMsg;
        systemMsg.role = "system";
        systemMsg.content = buildSystemPrompt();
        m_history.push_back(systemMsg);
    }

    Message userMsg;
    userMsg.role = "user";
    userMsg.content = userMessage;
    m_history.push_back(userMsg);
    trimHistory();

    int totalToolCalls = 0;
    for (int round = 0; round <= 8; ++round) {
        QString error;
        const QString response = m_provider->chat(m_history, error);
        if (!error.isEmpty()) {
            ok = false;
            return error;
        }

        const auto toolCalls = parseToolCalls(response);
        if (toolCalls.empty()) {
            const QString answer = stripToolCalls(response);
            m_history.push_back({"assistant", answer, {}, {}});
            trimHistory();
            ok = true;
            return answer;
        }
        if (round == 8 || totalToolCalls + static_cast<int>(toolCalls.size()) > 32) {
            ok = false;
            return "Достигнут предел вызовов инструментов в одном запросе";
        }
        totalToolCalls += static_cast<int>(toolCalls.size());
        m_history.push_back({"assistant", stripToolCalls(response), {}, {}});

        for (const auto& call : toolCalls) {
            Message toolMsg;
            toolMsg.role = "tool";
            toolMsg.content = executeTool(call.first, call.second);
            toolMsg.toolCallId = call.first;
            if (m_hasPendingAttachment) {
                toolMsg.attachments.push_back(m_pendingAttachment);
                m_pendingAttachment = Attachment();
                m_hasPendingAttachment = false;
            }
            m_history.push_back(toolMsg);
        }
    }
    ok = false;
    return "Достигнут предел вызовов инструментов в одном запросе";
}

QString ChatAgent::processUserMessageStream(const QString& userMessage,
                                            const StreamCallback& onChunk,
                                            bool& ok) {
    if (!m_provider) {
        ok = false;
        return "Провайдер не выбран";
    }

    if (m_history.empty()) {
        Message systemMsg;
        systemMsg.role = "system";
        systemMsg.content = buildSystemPrompt();
        m_history.push_back(systemMsg);
    }

    Message userMsg;
    userMsg.role = "user";
    userMsg.content = userMessage;
    m_history.push_back(userMsg);
    trimHistory();

    int totalToolCalls = 0;
    for (int round = 0; round <= 8; ++round) {
        QString error;
        VisibleStream visible(onChunk);
        const QString response = m_provider->chatStream(m_history,
            [&](const QString& chunk) { visible.add(chunk); }, error);
        visible.finish();
        if (!error.isEmpty()) {
            ok = false;
            return error;
        }

        const auto toolCalls = parseToolCalls(response);
        if (toolCalls.empty()) {
            const QString answer = stripToolCalls(response);
            m_history.push_back({"assistant", answer, {}, {}});
            trimHistory();
            ok = true;
            return answer;
        }
        if (round == 8 || totalToolCalls + static_cast<int>(toolCalls.size()) > 32) {
            ok = false;
            return "Достигнут предел вызовов инструментов в одном запросе";
        }
        totalToolCalls += static_cast<int>(toolCalls.size());
        m_history.push_back({"assistant", stripToolCalls(response), {}, {}});

        for (const auto& call : toolCalls) {
            Message toolMsg;
            toolMsg.role = "tool";
            toolMsg.content = executeTool(call.first, call.second);
            toolMsg.toolCallId = call.first;
            if (m_hasPendingAttachment) {
                toolMsg.attachments.push_back(m_pendingAttachment);
                m_pendingAttachment = Attachment();
                m_hasPendingAttachment = false;
            }
            m_history.push_back(toolMsg);
        }
    }
    ok = false;
    return "Достигнут предел вызовов инструментов в одном запросе";
}

void ChatAgent::clearHistory() {
    m_history.clear();
}

bool ChatAgent::saveHistory(const QString& path) const {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QJsonArray arr;
    for (const auto& msg : m_history) {
        QJsonObject obj;
        obj.insert("role", msg.role);
        obj.insert("content", msg.content);
        if (!msg.toolCallId.isEmpty()) {
            obj.insert("tool_call_id", msg.toolCallId);
        }
        if (!msg.attachments.empty()) {
            QJsonArray atts;
            for (const auto& att : msg.attachments) {
                QJsonObject attObj;
                attObj.insert("type", att.type);
                attObj.insert("media_type", att.mediaType);
                attObj.insert("data", att.data);
                atts.append(attObj);
            }
            obj.insert("attachments", atts);
        }
        arr.append(obj);
    }

    QJsonDocument doc(arr);
    QByteArray data = doc.toJson(QJsonDocument::Indented);
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(data) != data.size() || !file.commit()) {
        return false;
    }

    return chmod(QFile::encodeName(path).constData(), 0600) == 0;
}

bool ChatAgent::loadHistory(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray()) {
        return false;
    }

    m_history.clear();
    QJsonArray arr = doc.array();
    for (const auto& val : arr) {
        QJsonObject obj = val.toObject();
        Message msg;
        msg.role = obj.value("role").toString();
        msg.content = obj.value("content").toString();
        msg.toolCallId = obj.value("tool_call_id").toString();

        QJsonArray atts = obj.value("attachments").toArray();
        for (const auto& attVal : atts) {
            QJsonObject attObj = attVal.toObject();
            Attachment att;
            att.type = attObj.value("type").toString();
            att.mediaType = attObj.value("media_type").toString();
            att.data = attObj.value("data").toString();
            msg.attachments.push_back(att);
        }

        m_history.push_back(msg);
    }

    return true;
}

int ChatAgent::historySize() const {
    return static_cast<int>(m_history.size());
}

const std::vector<Message>& ChatAgent::history() const {
    return m_history;
}

void ChatAgent::addExchange(const QString& userText, const QString& assistantText) {
    if (m_history.empty()) {
        Message system;
        system.role = "system";
        system.content = buildSystemPrompt();
        m_history.push_back(system);
    }

    Message user;
    user.role = "user";
    user.content = userText;
    m_history.push_back(user);

    Message assistant;
    assistant.role = "assistant";
    assistant.content = assistantText;
    m_history.push_back(assistant);
    trimHistory();
}

void ChatAgent::trimHistory() {
    // Сохраняем системное сообщение, обрезаем старые пользовательские
    if (static_cast<int>(m_history.size()) <= MAX_HISTORY_MESSAGES) {
        return;
    }

    // Первое сообщение - системный промпт, его не трогаем
    std::vector<Message> trimmed;
    if (!m_history.empty() && m_history.front().role == "system") {
        trimmed.push_back(m_history.front());
    }

    // Берём последние сообщения
    int keep = MAX_HISTORY_MESSAGES - static_cast<int>(trimmed.size());
    auto start = m_history.end() - keep;
    if (start < m_history.begin() + static_cast<long>(trimmed.size())) {
        start = m_history.begin() + static_cast<long>(trimmed.size());
    }

    for (auto it = start; it != m_history.end(); ++it) {
        trimmed.push_back(*it);
    }

    m_history = std::move(trimmed);
}

bool ChatAgent::isDangerousTool(const QString& name) const {
    return name == "run_shell" || name == "write_file" || name == "set_setting" ||
           name == "kill_process" || name == "package_install" ||
           name == "set_cursor_theme" || name == "lan_send";
}

QString ChatAgent::describeToolCall(const QString& name, const QJsonObject& arguments) const {
    if (name == "run_shell") {
        return "Выполнить: " + arguments.value("command").toString();
    }
    if (name == "write_file") {
        return "Записать файл: " + arguments.value("path").toString();
    }
    if (name == "set_setting") {
        return "Изменить настройку: [" + arguments.value("section").toString() +
               "] " + arguments.value("key").toString() + " = " + arguments.value("value").toString();
    }
    if (name == "kill_process") {
        return "Завершить процесс PID " + arguments.value("pid").toString();
    }
    if (name == "package_install") {
        return "Установить пакет: " + arguments.value("name").toString();
    }
    if (name == "lan_send") {
        return "Отправить на " + arguments.value("ip").toString() + ": " +
               arguments.value("message").toString().left(512);
    }

    return name + "(" + QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact)) + ")";
}

QString ChatAgent::executeTool(const QString& name, const QJsonObject& arguments) {
    if (!m_tools) {
        return "Инструменты ОС недоступны";
    }

    QString category = permissionCategory(name);
    if (category.isEmpty()) {
        return "Неизвестный инструмент: " + name;
    }

    QString desc = describeToolCall(name, arguments);
    if (!m_permissionCallback || !m_permissionCallback(category, desc)) {
        return "Доступ не разрешён: " + desc;
    }

    if (isDangerousTool(name) &&
        (!m_confirmCallback || !m_confirmCallback(name, desc))) {
        return "Операция отменена пользователем: " + desc;
    }

    if (name == "run_shell") {
        ShellResult result = m_tools->runShell(arguments.value("command").toString());
        return QString("exit_code: %1\nstdout:\n%2\nstderr:\n%3")
            .arg(result.exitCode)
            .arg(result.stdoutText)
            .arg(result.stderrText);
    }

    if (name == "read_file") {
        bool ok = false;
        QString content = m_tools->readFile(arguments.value("path").toString(), ok);
        return ok ? content : "Не удалось прочитать файл";
    }

    if (name == "write_file") {
        bool ok = m_tools->writeFile(arguments.value("path").toString(),
                                     arguments.value("content").toString());
        return ok ? "Файл записан" : "Не удалось записать файл";
    }

    if (name == "list_directory") {
        return m_tools->listDirectory(arguments.value("path").toString());
    }

    if (name == "get_setting") {
        bool ok = false;
        QString value = m_tools->getSetting(arguments.value("section").toString(),
                                            arguments.value("key").toString(), ok);
        return ok ? value : "Настройка не найдена";
    }

    if (name == "set_setting") {
        bool ok = m_tools->setSetting(arguments.value("section").toString(),
                                      arguments.value("key").toString(),
                                      arguments.value("value").toString());
        return ok ? "Настройка обновлена" : "Не удалось обновить настройку";
    }

    if (name == "system_info") {
        QJsonObject info = m_tools->systemInfo();
        return QString::fromUtf8(QJsonDocument(info).toJson());
    }

    if (name == "find_files") {
        return m_tools->findFiles(arguments.value("pattern").toString(),
                                  arguments.value("directory").toString());
    }

    if (name == "search_in_files") {
        int max = arguments.value("max_results").toString().toInt();
        if (max <= 0) {
            max = 50;
        }
        return m_tools->searchInFiles(arguments.value("pattern").toString(),
                                      arguments.value("path").toString(), max);
    }

    if (name == "list_processes") {
        return m_tools->listProcesses();
    }

    if (name == "kill_process") {
        int pid = arguments.value("pid").toString().toInt();
        return m_tools->killProcess(pid);
    }

    if (name == "network_info") {
        QJsonObject info = m_tools->networkInfo();
        return info.isEmpty() ? "Не удалось получить сведения о сети"
                              : QString::fromUtf8(QJsonDocument(info).toJson());
    }

    if (name == "ping_host") {
        int count = arguments.value("count").toString().toInt();
        if (count <= 0) {
            count = 4;
        }
        return m_tools->pingHost(arguments.value("host").toString(), count);
    }

    if (name == "package_list") {
        return m_tools->packageList();
    }

    if (name == "package_install") {
        return m_tools->packageInstall(arguments.value("name").toString());
    }

    if (name == "disk_usage") {
        return m_tools->diskUsage();
    }

    if (name == "memory_info") {
        QJsonObject info = m_tools->memoryInfo();
        return info.isEmpty() ? "Не удалось прочитать сведения о памяти"
                              : QString::fromUtf8(QJsonDocument(info).toJson());
    }

    if (name == "take_screenshot") {
        ScreenshotResult shot = m_tools->takeScreenshot(
            arguments.value("window_title").toString());

        if (shot.ok) {
            m_pendingAttachment = shot.attachment;
            m_hasPendingAttachment = true;
            return "Скриншот сделан и прикреплён к следующему запросу: " + shot.path;
        }

        return "Ошибка: " + shot.error;
    }

    if (name == "set_cursor_theme") {
        int size = arguments.value("size").toString().toInt();
        return m_tools->setCursorTheme(arguments.value("theme").toString(), size);
    }

    if (name == "lan_send") {
        return m_tools->sendLanMessage(arguments.value("ip").toString(),
                                       arguments.value("message").toString());
    }

    return "Неизвестный инструмент: " + name;
}

std::vector<std::pair<QString, QJsonObject>> ChatAgent::parseToolCalls(const QString& text) const {
    std::vector<std::pair<QString, QJsonObject>> result;
    QRegularExpression regex("<tool>(.*?)</tool>");
    regex.setPatternOptions(QRegularExpression::DotMatchesEverythingOption);
    QRegularExpressionMatchIterator it = regex.globalMatch(text);

    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        QString jsonText = match.captured(1).trimmed();
        QJsonDocument doc = QJsonDocument::fromJson(jsonText.toUtf8());
        if (doc.isObject()) {
            QJsonObject obj = doc.object();
            QString name = obj.value("name").toString();
            QJsonObject args = obj.value("arguments").toObject();
            result.emplace_back(name, args);
        }
    }

    return result;
}

QString ChatAgent::stripToolCalls(const QString& text) const {
    QRegularExpression regex("<tool>.*?</tool>");
    regex.setPatternOptions(QRegularExpression::DotMatchesEverythingOption);
    QString result = text;
    result.remove(regex);
    return result.trimmed();
}
