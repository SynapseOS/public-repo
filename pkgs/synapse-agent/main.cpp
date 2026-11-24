/*
    main.cpp: Графический ИИ-агент SynapseOS
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI: synapse-x86_64-v1

    Описание: Чат с внешними моделями и действия в SynapseOS с разрешения пользователя
    Подсистема: пользовательские приложения
    Первое изменение: 24.11.2026
    Последнее изменение: 24.11.2026
    Доступ: общедоступный
*/

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMap>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <QImage>
#include <QUrl>
#include <QtEndian>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

struct Profile {
    QString model;
    QString endpoint;
    QString key;
};

// Вернуть адрес API и начальную модель выбранного поставщика
static Profile initialProfile(const QString &provider) {
    if (provider == "OpenAI") {
        return {"gpt-4.1-mini", "https://api.openai.com/v1/chat/completions", {}};
    }
    if (provider == "Claude") {
        return {"claude-sonnet-4-5", "https://api.anthropic.com/v1/messages", {}};
    }
    if (provider == "Gemini") {
        return {"gemini-2.5-flash", "https://generativelanguage.googleapis.com/v1beta/models/", {}};
    }
    if (provider == "Qwen") {
        return {"qwen-plus", "https://dashscope-intl.aliyuncs.com/compatible-mode/v1/chat/completions", {}};
    }
    if (provider == "DeepSeek") {
        return {"deepseek-flash", "https://api.deepseek.com/chat/completions", {}};
    }
    return {"kimi-k2.5", "https://api.moonshot.ai/v1/chat/completions", {}};
}

// Вернуть личный путь настроек текущего пользователя
static QString settingsPath() {
    return QDir::homePath() + "/.config/synapse-agent/settings.json";
}

// Создать личный каталог с доступом только владельцу
static bool ensureSettingsDir() {
    const QByteArray base = (QDir::homePath() + "/.config").toLocal8Bit();
    const QByteArray own = (QDir::homePath() + "/.config/synapse-agent").toLocal8Bit();
    struct stat info = {};

    if (::mkdir(base.constData(), 0700) != 0 && errno != EEXIST) {
        return false;
    }
    if (::mkdir(own.constData(), 0700) != 0 && errno != EEXIST) {
        return false;
    }
    if (::lstat(own.constData(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid()) {
        return false;
    }
    return (info.st_mode & 077) == 0;
}

// Прочитать настройки, отклонив ссылки и доступные другим пользователям файлы
static QJsonObject readSettings() {
    const QByteArray path = settingsPath().toLocal8Bit();
    const int fd = ::open(path.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return {};
    }

    struct stat info = {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
        (info.st_mode & 077) != 0 || info.st_size > 65536) {
        ::close(fd);
        return {};
    }

    QByteArray data;
    data.resize(static_cast<qsizetype>(info.st_size));
    qsizetype done = 0;
    while (done < data.size()) {
        const ssize_t count = ::read(fd, data.data() + done, static_cast<size_t>(data.size() - done));
        if (count <= 0) {
            ::close(fd);
            return {};
        }
        done += count;
    }
    ::close(fd);
    return QJsonDocument::fromJson(data).object();
}

// Сохранить настройки атомарно с правами 0600
static bool writeSettings(const QJsonObject &object) {
    if (!ensureSettingsDir()) {
        return false;
    }

    QSaveFile file(settingsPath());
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.cancelWriting();
        return false;
    }

    const QByteArray data = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (file.write(data) != data.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

// Добавить ограниченную по размеру часть к выводу команды
static void appendOutput(QByteArray &output, const QByteArray &part) {
    constexpr qsizetype limit = 16384;
    if (output.size() < limit) {
        output.append(part.left(limit - output.size()));
    }
}

// Выполнить команду и собрать ограниченный вывод
static QString runProcess(const QString &program, const QStringList &arguments,
                          const QByteArray &input, int timeoutMs) {
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);
    if (!process.waitForStarted(5000)) {
        return "Не удалось запустить программу";
    }
    if (!input.isEmpty()) {
        process.write(input);
        process.waitForBytesWritten(5000);
    }
    process.closeWriteChannel();

    QByteArray output;
    QElapsedTimer timer;
    timer.start();
    while (process.state() != QProcess::NotRunning && timer.elapsed() < timeoutMs) {
        process.waitForReadyRead(250);
        appendOutput(output, process.readAll());
    }
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(1000);
        return QString::fromUtf8(output) + "\nПревышено время выполнения";
    }

    appendOutput(output, process.readAll());
    return QString("Код выхода: %1\n%2").arg(process.exitCode()).arg(QString::fromUtf8(output));
}

// Сформировать описание функции для API Chat Completions
static QJsonObject functionTool(const QString &name, const QString &description,
                                const QJsonObject &properties, const QJsonArray &required) {
    QJsonObject parameters{{"type", "object"}, {"properties", properties}, {"required", required}};
    parameters.insert("additionalProperties", false);
    return {{"type", "function"},
            {"function", QJsonObject{{"name", name}, {"description", description},
                                     {"parameters", parameters}}}};
}

class AgentWindow final : public QMainWindow {
public:
    // Построить окно чата и загрузить личные настройки
    AgentWindow() {
        setWindowTitle("Агент SynapseOS");
        resize(950, 680);
        buildUi();
        loadSettings();
    }

private:
    QNetworkAccessManager network;
    QMap<QString, Profile> profiles;
    QJsonArray messages;
    QComboBox *providerBox = nullptr;
    QLineEdit *modelEdit = nullptr;
    QLineEdit *endpointEdit = nullptr;
    QLineEdit *keyEdit = nullptr;
    QPlainTextEdit *history = nullptr;
    QPlainTextEdit *prompt = nullptr;
    QPushButton *sendButton = nullptr;
    QCheckBox *commandsAllowed = nullptr;
    QCheckBox *rootAllowed = nullptr;
    QCheckBox *captureAllowed = nullptr;
    QCheckBox *lanAllowed = nullptr;
    QString activeProvider;
    bool busy = false;
    int toolTurns = 0;

    // Построить две вкладки с чатом и настройками
    void buildUi() {
        auto *tabs = new QTabWidget(this);
        setCentralWidget(tabs);
        auto *chat = new QWidget(tabs);
        auto *chatLayout = new QVBoxLayout(chat);
        auto *header = new QHBoxLayout();
        providerBox = new QComboBox(chat);
        providerBox->addItems({"OpenAI", "Claude", "Gemini", "Qwen", "DeepSeek", "Kimi"});
        header->addWidget(new QLabel("Поставщик:", chat));
        header->addWidget(providerBox);
        header->addStretch();
        auto *newChat = new QPushButton("Новый чат", chat);
        header->addWidget(newChat);
        chatLayout->addLayout(header);

        history = new QPlainTextEdit(chat);
        history->setReadOnly(true);
        history->setPlaceholderText("Выберите поставщика, добавьте ключ в настройках и начните диалог");
        chatLayout->addWidget(history, 1);
        prompt = new QPlainTextEdit(chat);
        prompt->setPlaceholderText("Сообщение агенту");
        prompt->setMaximumHeight(110);
        chatLayout->addWidget(prompt);

        auto *actions = new QHBoxLayout();
        auto *screenButton = new QPushButton("Снимок экрана", chat);
        auto *commandButton = new QPushButton("Команда", chat);
        auto *lanButton = new QPushButton("Сообщение LAN", chat);
        sendButton = new QPushButton("Отправить", chat);
        actions->addWidget(screenButton);
        actions->addWidget(commandButton);
        actions->addWidget(lanButton);
        actions->addStretch();
        actions->addWidget(sendButton);
        chatLayout->addLayout(actions);
        tabs->addTab(chat, "Чат");

        auto *settings = new QWidget(tabs);
        auto *settingsLayout = new QVBoxLayout(settings);
        auto *form = new QFormLayout();
        modelEdit = new QLineEdit(settings);
        endpointEdit = new QLineEdit(settings);
        keyEdit = new QLineEdit(settings);
        keyEdit->setEchoMode(QLineEdit::Password);
        form->addRow("Модель", modelEdit);
        form->addRow("Адрес API", endpointEdit);
        form->addRow("Ключ API", keyEdit);
        settingsLayout->addLayout(form);
        commandsAllowed = new QCheckBox("Разрешить выполнение команд после подтверждения", settings);
        rootAllowed = new QCheckBox("Разрешить запрос прав root для команд", settings);
        captureAllowed = new QCheckBox("Разрешить снимки экрана", settings);
        lanAllowed = new QCheckBox("Разрешить отправку сообщений через synapse-lan", settings);
        settingsLayout->addWidget(commandsAllowed);
        settingsLayout->addWidget(rootAllowed);
        settingsLayout->addWidget(captureAllowed);
        settingsLayout->addWidget(lanAllowed);
        auto *saveButton = new QPushButton("Сохранить настройки", settings);
        settingsLayout->addWidget(saveButton);
        settingsLayout->addStretch();
        tabs->addTab(settings, "Настройки");

        connect(providerBox, &QComboBox::currentTextChanged, this, [this](const QString &name) {
            displayProfile(name);
            messages = {};
            history->clear();
        });
        connect(saveButton, &QPushButton::clicked, this, [this]() { saveSettings(); });
        connect(sendButton, &QPushButton::clicked, this, [this]() { sendMessage(); });
        connect(newChat, &QPushButton::clicked, this, [this]() {
            messages = {};
            history->clear();
        });
        connect(screenButton, &QPushButton::clicked, this, [this]() { captureScreen(); });
        connect(commandButton, &QPushButton::clicked, this, [this]() { manualCommand(); });
        connect(lanButton, &QPushButton::clicked, this, [this]() { manualLan(); });
        displayProfile(providerBox->currentText());
    }

    // Показать поля выбранного поставщика
    void displayProfile(const QString &name) {
        activeProvider = name;
        const Profile profile = profiles.value(name, initialProfile(name));
        modelEdit->setText(profile.model);
        endpointEdit->setText(profile.endpoint);
        keyEdit->setText(profile.key);
    }

    // Загрузить ключи и разрешения из личного файла
    void loadSettings() {
        const QJsonObject root = readSettings();
        const QJsonObject saved = root.value("providers").toObject();
        for (const QString name : {"OpenAI", "Claude", "Gemini", "Qwen", "DeepSeek", "Kimi"}) {
            const QJsonObject data = saved.value(name).toObject();
            Profile profile = initialProfile(name);
            if (!data.isEmpty()) {
                profile.model = data.value("model").toString(profile.model);
                profile.endpoint = data.value("endpoint").toString(profile.endpoint);
                profile.key = data.value("key").toString();
            }
            profiles.insert(name, profile);
        }
        commandsAllowed->setChecked(root.value("commands").toBool(false));
        rootAllowed->setChecked(root.value("root").toBool(false));
        captureAllowed->setChecked(root.value("capture").toBool(false));
        lanAllowed->setChecked(root.value("lan").toBool(false));
        displayProfile(providerBox->currentText());
    }

    // Сохранить выбранный профиль и разрешения
    void saveSettings() {
        profiles.insert(activeProvider, {modelEdit->text().trimmed(), endpointEdit->text().trimmed(),
                                         keyEdit->text().trimmed()});
        QJsonObject saved;
        for (auto it = profiles.cbegin(); it != profiles.cend(); ++it) {
            saved.insert(it.key(), QJsonObject{{"model", it.value().model},
                                               {"endpoint", it.value().endpoint},
                                               {"key", it.value().key}});
        }
        const QJsonObject root{{"providers", saved}, {"commands", commandsAllowed->isChecked()},
                               {"root", rootAllowed->isChecked()}, {"capture", captureAllowed->isChecked()},
                               {"lan", lanAllowed->isChecked()}};
        if (!writeSettings(root)) {
            QMessageBox::warning(this, "Настройки", "Не удалось сохранить закрытый файл настроек");
            return;
        }
        statusBar()->showMessage("Настройки сохранены", 3000);
    }

    // Добавить запись в историю окна
    void showLine(const QString &speaker, const QString &text) {
        history->appendPlainText(speaker + ":\n" + text + "\n");
    }

    // Отправить пользовательский текст в выбранную модель
    void sendMessage() {
        if (busy) {
            return;
        }
        const QString text = prompt->toPlainText().trimmed();
        if (text.isEmpty()) {
            return;
        }
        saveSettings();
        const Profile profile = profiles.value(activeProvider);
        if (profile.key.isEmpty() || profile.model.isEmpty() || profile.endpoint.isEmpty()) {
            QMessageBox::warning(this, "API", "Укажите адрес API, модель и ключ в настройках");
            return;
        }
        prompt->clear();
        showLine("Вы", text);
        messages.append(QJsonObject{{"role", "user"}, {"content", text}});
        toolTurns = 0;
        requestModel();
    }

    // Собрать перечень действий для совместимых с OpenAI API поставщиков
    QJsonArray availableTools() const {
        QJsonArray tools;
        if (commandsAllowed->isChecked()) {
            tools.append(functionTool("run_command", "Run a SynapseOS shell command after user approval",
                {{"command", QJsonObject{{"type", "string"}}},
                 {"root", QJsonObject{{"type", "boolean"}}}}, {"command", "root"}));
        }
        if (captureAllowed->isChecked()) {
            tools.append(functionTool("capture_screen", "Save a screenshot of the SynapseOS desktop", {}, {}));
        }
        if (lanAllowed->isChecked()) {
            tools.append(functionTool("lan_send", "Send text to a LAN peer through synapse-lan",
                {{"ip", QJsonObject{{"type", "string"}}},
                 {"text", QJsonObject{{"type", "string"}}}}, {"ip", "text"}));
        }
        return tools;
    }

    // Преобразовать историю в формат Claude или Gemini
    QJsonArray providerMessages(const QString &provider) const {
        QJsonArray converted;
        for (const QJsonValue &value : messages) {
            const QJsonObject item = value.toObject();
            const QString role = item.value("role").toString();
            if (role != "user" && role != "assistant") {
                continue;
            }
            const QString content = item.value("content").toString();
            if (provider == "Gemini") {
                converted.append(QJsonObject{{"role", role == "assistant" ? "model" : "user"},
                                             {"parts", QJsonArray{QJsonObject{{"text", content}}}}});
            } else {
                converted.append(QJsonObject{{"role", role}, {"content", content}});
            }
        }
        return converted;
    }

    // Выполнить запрос HTTPS с проверкой сертификата средствами Qt
    void requestModel() {
        const Profile profile = profiles.value(activeProvider);
        const QString provider = activeProvider;
        QUrl url(profile.endpoint);
        QJsonObject body;
        if (provider == "Claude") {
            body = {{"model", profile.model}, {"max_tokens", 4096},
                    {"messages", providerMessages(provider)}};
        } else if (provider == "Gemini") {
            QString address = profile.endpoint;
            if (!address.endsWith('/')) {
                address += '/';
            }
            url = QUrl(address + QString::fromUtf8(QUrl::toPercentEncoding(profile.model)) + ":generateContent");
            body = {{"contents", providerMessages(provider)}};
        } else {
            body = {{"model", profile.model}, {"messages", messages}, {"stream", false}};
            const QJsonArray tools = availableTools();
            if (!tools.isEmpty()) {
                body.insert("tools", tools);
            }
        }

        if (!url.isValid() || url.scheme() != "https" || url.host().isEmpty() ||
            url.userInfo().size() != 0) {
            showLine("Ошибка", "Адрес API должен быть HTTPS без учётных данных в ссылке");
            return;
        }
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setTransferTimeout(120000);
        if (provider == "Claude") {
            request.setRawHeader("x-api-key", profile.key.toUtf8());
            request.setRawHeader("anthropic-version", "2023-06-01");
        } else if (provider == "Gemini") {
            request.setRawHeader("x-goog-api-key", profile.key.toUtf8());
        } else {
            request.setRawHeader("Authorization", "Bearer " + profile.key.toUtf8());
        }

        busy = true;
        sendButton->setEnabled(false);
        statusBar()->showMessage("Ожидание ответа модели");
        QNetworkReply *reply = network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, provider]() {
            const QByteArray data = reply->readAll();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString error = reply->errorString();
            reply->deleteLater();
            busy = false;
            sendButton->setEnabled(true);
            statusBar()->clearMessage();
            if (status < 200 || status >= 300) {
                const QJsonObject object = QJsonDocument::fromJson(data).object();
                QString detail = object.value("error").toObject().value("message").toString();
                if (detail.isEmpty()) {
                    detail = error;
                }
                showLine("Ошибка API", QString("HTTP %1: %2").arg(status).arg(detail));
                return;
            }
            processReply(provider, data);
        });
    }

    // Прочитать ответ модели и выполнить разрешённые вызовы функций
    void processReply(const QString &provider, const QByteArray &data) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            showLine("Ошибка API", "Ответ поставщика не содержит корректный JSON");
            return;
        }
        const QJsonObject object = document.object();
        QString answer;
        QJsonObject message;
        if (provider == "Claude") {
            for (const QJsonValue &block : object.value("content").toArray()) {
                answer += block.toObject().value("text").toString();
            }
        } else if (provider == "Gemini") {
            const QJsonArray candidates = object.value("candidates").toArray();
            if (!candidates.isEmpty()) {
                const QJsonArray parts = candidates.first().toObject().value("content").toObject()
                                             .value("parts").toArray();
                for (const QJsonValue &part : parts) {
                    answer += part.toObject().value("text").toString();
                }
            }
        } else {
            const QJsonArray choices = object.value("choices").toArray();
            if (!choices.isEmpty()) {
                message = choices.first().toObject().value("message").toObject();
                answer = message.value("content").toString();
            }
        }
        if (!answer.isEmpty()) {
            showLine(provider, answer);
        }

        const QJsonArray calls = message.value("tool_calls").toArray();
        if (!calls.isEmpty()) {
            if (++toolTurns > 8) {
                showLine("Ошибка", "Превышен предел последовательных действий модели");
                return;
            }
            messages.append(message);
            for (const QJsonValue &value : calls) {
                const QJsonObject call = value.toObject();
                const QJsonObject function = call.value("function").toObject();
                const QJsonObject arguments = QJsonDocument::fromJson(
                    function.value("arguments").toString().toUtf8()).object();
                const QString result = executeTool(function.value("name").toString(), arguments);
                messages.append(QJsonObject{{"role", "tool"}, {"tool_call_id", call.value("id")},
                                            {"content", result}});
            }
            requestModel();
            return;
        }
        if (answer.isEmpty()) {
            showLine("Ошибка API", "Модель не вернула текст");
            return;
        }
        messages.append(QJsonObject{{"role", "assistant"}, {"content", answer}});
    }

    // Выполнить запрошенное моделью действие после проверки разрешений
    QString executeTool(const QString &name, const QJsonObject &arguments) {
        if (name == "run_command" && commandsAllowed->isChecked()) {
            const QString command = arguments.value("command").toString();
            const bool root = arguments.value("root").toBool(false);
            if (command.isEmpty() || command.size() > 4096 || (root && !rootAllowed->isChecked())) {
                return "Команда отклонена настройками";
            }
            if (QMessageBox::question(this, "Подтвердите команду",
                    QString("%1\n\n%2").arg(root ? "Права root" : "Текущий пользователь", command))
                != QMessageBox::Yes) {
                return "Пользователь отклонил команду";
            }
            QString output;
            if (root) {
                bool accepted = false;
                QString password = QInputDialog::getText(this, "Права root", "Пароль sudo:",
                                                         QLineEdit::Password, {}, &accepted);
                if (!accepted) {
                    return "Пользователь отклонил запрос прав root";
                }
                QByteArray input = password.toUtf8() + '\n';
                password.fill(QChar('\0'));
                output = runProcess("/bin/sudo", {"-S", "-p", "", "/bin/sh", "-c", command}, input, 30000);
                input.fill(0);
            } else {
                output = runProcess("/bin/sh", {"-c", command}, {}, 30000);
            }
            showLine("Команда", output);
            return output;
        }
        if (name == "lan_send" && lanAllowed->isChecked()) {
            const QString ip = arguments.value("ip").toString();
            const QString text = arguments.value("text").toString();
            if (text.isEmpty() || text.toUtf8().size() > 60000 || ip.size() > 45) {
                return "Сообщение отклонено";
            }
            if (QMessageBox::question(this, "Отправить через LAN", ip + "\n\n" + text) != QMessageBox::Yes) {
                return "Пользователь отклонил отправку";
            }
            const QString result = runProcess("/bin/lan", {"msg", ip, text}, {}, 15000);
            showLine("LAN", result);
            return result;
        }
        if (name == "capture_screen" && captureAllowed->isChecked()) {
            const QString path = QDir::homePath() + "/screenshot-" +
                                 QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss") + ".png";
            const QString result = saveScreen(path);
            showLine("Снимок экрана", result);
            return result;
        }
        return "Действие не разрешено";
    }

    // Сохранить кадр Wayland через штатную программу Synapse Recorder
    QString saveScreen(const QString &path) {
        QProcess process;
        process.start("/usr/libexec/recorder-grab");
        if (!process.waitForStarted(5000)) {
            return "Программа захвата экрана не запустилась";
        }
        process.write("x", 1);
        process.closeWriteChannel();
        QByteArray bytes;
        QElapsedTimer timer;
        timer.start();
        while (process.state() != QProcess::NotRunning && timer.elapsed() < 10000) {
            process.waitForReadyRead(250);
            bytes.append(process.readAllStandardOutput());
            if (bytes.size() > 64 * 1024 * 1024) {
                process.kill();
                return "Снимок превышает предел памяти";
            }
        }
        bytes.append(process.readAllStandardOutput());
        if (process.state() != QProcess::NotRunning || process.exitCode() != 0 || bytes.size() < 8) {
            process.kill();
            return "Не удалось получить кадр Wayland";
        }

        const unsigned char *data = reinterpret_cast<const unsigned char *>(bytes.constData());
        const quint32 width = qFromLittleEndian<quint32>(data);
        const quint32 height = qFromLittleEndian<quint32>(data + 4);
        if (width == 0 || height == 0 || width > 4096 || height > 4096 ||
            static_cast<quint64>(width) * height * 4 + 8 != static_cast<quint64>(bytes.size())) {
            return "Неверный размер кадра";
        }
        const QImage image(data + 8, static_cast<int>(width), static_cast<int>(height),
                           static_cast<int>(width * 4), QImage::Format_RGB32);
        return image.save(path, "PNG") ? "Сохранено: " + path : "Не удалось сохранить PNG";
    }

    // Запросить у пользователя путь для снимка экрана
    void captureScreen() {
        if (!captureAllowed->isChecked()) {
            QMessageBox::information(this, "Снимок экрана", "Разрешите снимки в настройках");
            return;
        }
        const QString path = QFileDialog::getSaveFileName(this, "Сохранить снимок экрана",
                                                           QDir::homePath() + "/screenshot.png", "PNG (*.png)");
        if (!path.isEmpty()) {
            showLine("Снимок экрана", saveScreen(path));
        }
    }

    // Выполнить введённую пользователем команду через тот же контроль прав
    void manualCommand() {
        bool accepted = false;
        const QString command = QInputDialog::getText(this, "Команда SynapseOS", "Команда:",
                                                      QLineEdit::Normal, {}, &accepted);
        if (accepted && !command.isEmpty()) {
            showLine("Команда", executeTool("run_command", {{"command", command}, {"root", false}}));
        }
    }

    // Передать введённый пользователем текст через synapse-lan
    void manualLan() {
        bool accepted = false;
        const QString ip = QInputDialog::getText(this, "Synapse LAN", "IP соседа:",
                                                 QLineEdit::Normal, {}, &accepted);
        if (!accepted || ip.isEmpty()) {
            return;
        }
        const QString text = QInputDialog::getMultiLineText(this, "Synapse LAN", "Сообщение:",
                                                            {}, &accepted);
        if (accepted && !text.isEmpty()) {
            showLine("LAN", executeTool("lan_send", {{"ip", ip}, {"text", text}}));
        }
    }
};

// Запустить графический агент только в SynapseOS
int main(int argc, char **argv) {
#ifndef __synapse__
    (void)argc;
    (void)argv;
    return 1;
#else
    QApplication app(argc, argv);
    AgentWindow window;
    window.show();
    return app.exec();
#endif
}
