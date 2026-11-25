/*
    synapse-agent/main_window.cpp
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI musl
    Статус доступа: общедоступный

    Графический интерфейс агента: стриминг, Markdown, подтверждения, горячие клавиши
    Подсистема: пользовательские приложения
    Первое изменение: 24.11.2026
    Последнее изменение: 25.11.2026
*/

#include "main_window.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QTextCursor>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace {

struct ModelCatalog {
    QStringList names;
    QSet<QString> vision;
    bool capabilitiesKnown = false;
};

using ModelListCallback = std::function<void(const ModelCatalog&, const QString&)>;

// Загрузить модели провайдера, включая страницы каталога Qwen
void requestModelList(const ProviderConfig& provider, QNetworkAccessManager* network,
                      QObject* context, const ModelListCallback& done,
                      int page = 1, const ModelCatalog& previous = {}) {
    if (provider.apiKey.isEmpty()) {
        done({}, "Сначала укажите API ключ");
        return;
    }

    QUrl url(provider.baseUrl.trimmed());
    if (!url.isValid() || url.scheme() != "https" || url.host().isEmpty() ||
        !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) {
        done({}, "Нужен базовый HTTPS URL без параметров");
        return;
    }

    const bool qwenCatalog = provider.name == "qwen" &&
        url.path().endsWith("/compatible-mode/v1");
    if (qwenCatalog) {
        url.setPath("/api/v1/models");
        QUrlQuery query;
        query.addQueryItem("page_no", QString::number(page));
        query.addQueryItem("page_size", "100");
        query.addQueryItem("capabilities", "TG");
        url.setQuery(query);
    } else {
        QString path = url.path();
        while (path.endsWith('/')) {
            path.chop(1);
        }
        url.setPath(path + (provider.type == "anthropic" ? "/v1/models" : "/models"));
    }

    QNetworkRequest request(url);
    if (provider.type == "anthropic") {
        request.setRawHeader("x-api-key", provider.apiKey.toUtf8());
        request.setRawHeader("anthropic-version", "2023-06-01");
    } else {
        request.setRawHeader("Authorization", ("Bearer " + provider.apiKey).toUtf8());
    }

    QNetworkReply* reply = network->get(request);
    QObject::connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    QObject::connect(reply, &QNetworkReply::finished, context,
            [provider, network, context, done, page, previous, reply, qwenCatalog]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 400 || reply->error() != QNetworkReply::NoError) {
            done({}, status >= 400 ? "HTTP " + QString::number(status) : reply->errorString());
            return;
        }

        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        if (!document.isObject()) {
            done({}, "Провайдер вернул неверный список моделей");
            return;
        }

        const QJsonObject root = document.object();
        const QJsonObject output = root.value("output").toObject();
        const QJsonArray models = qwenCatalog ? output.value("models").toArray()
                                              : root.value("data").toArray();
        if (models.isEmpty() && previous.names.isEmpty()) {
            done({}, "Провайдер не вернул модели");
            return;
        }

        ModelCatalog catalog = previous;
        catalog.capabilitiesKnown = qwenCatalog;
        for (const QJsonValue& value : models) {
            const QJsonObject modelObject = value.toObject();
            const QString name = modelObject.value(qwenCatalog ? "model" : "id")
                .toString().trimmed();
            if (!name.isEmpty() && !catalog.names.contains(name)) {
                catalog.names.append(name);
            }
            if (qwenCatalog) {
                for (const QJsonValue& capability : modelObject.value("capabilities").toArray()) {
                    if (capability.toString() == "VU") {
                        catalog.vision.insert(name);
                    }
                }
            }
        }
        if (qwenCatalog && page < 10 && !models.isEmpty() &&
            output.value("total").toInt() > catalog.names.size()) {
            requestModelList(provider, network, context, done, page + 1, catalog);
            return;
        }
        done(catalog, {});
    });
}

}

class MainWindow::Impl {
public:
    QNetworkAccessManager network;
    AgentConfig config;
    OSTools tools;
    std::unique_ptr<ChatAgent> agent;

    QWidget* centralWidget = nullptr;
    QTextBrowser* chatView = nullptr;
    QLineEdit* inputEdit = nullptr;
    QPushButton* sendButton = nullptr;
    QPushButton* swarmButton = nullptr;
    QPushButton* settingsButton = nullptr;
    QPushButton* clearButton = nullptr;
    QPushButton* saveButton = nullptr;
    QPushButton* loadButton = nullptr;
    QComboBox* providerCombo = nullptr;
    QLabel* statusLabel = nullptr;
};

namespace {

// Заполнить список провайдеров
void fillProviderCombo(QComboBox* combo, const AgentConfig& config) {
    combo->clear();
    for (const auto& p : config.providers()) {
        combo->addItem(p.name);
    }

    int index = combo->findText(config.activeProvider());
    if (index >= 0) {
        combo->setCurrentIndex(index);
    }
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , d(std::make_unique<Impl>()) {

    d->config.load();
    d->centralWidget = new QWidget(this);
    setCentralWidget(d->centralWidget);

    auto* mainLayout = new QVBoxLayout(d->centralWidget);
    mainLayout->setSpacing(8);
    mainLayout->setContentsMargins(12, 12, 12, 12);

    // Верхняя панель
    auto* topLayout = new QHBoxLayout();
    d->providerCombo = new QComboBox(this);
    fillProviderCombo(d->providerCombo, d->config);
    topLayout->addWidget(new QLabel("Провайдер:", this));
    topLayout->addWidget(d->providerCombo);

    d->settingsButton = new QPushButton("Настройки", this);
    d->swarmButton = new QPushButton("Рой (до 100)", this);
    d->clearButton = new QPushButton("Очистить", this);
    d->saveButton = new QPushButton("Сохранить", this);
    d->loadButton = new QPushButton("Загрузить", this);
    topLayout->addWidget(d->swarmButton);
    topLayout->addWidget(d->settingsButton);
    topLayout->addWidget(d->clearButton);
    topLayout->addWidget(d->saveButton);
    topLayout->addWidget(d->loadButton);
    topLayout->addStretch();
    mainLayout->addLayout(topLayout);

    // Область диалога
    d->chatView = new QTextBrowser(this);
    d->chatView->setReadOnly(true);
    d->chatView->setOpenExternalLinks(true);
    mainLayout->addWidget(d->chatView);

    // Строка ввода
    auto* bottomLayout = new QHBoxLayout();
    d->inputEdit = new QLineEdit(this);
    d->inputEdit->setPlaceholderText("Введите сообщение (Ctrl+Enter для отправки)...");
    d->sendButton = new QPushButton("Отправить", this);
    bottomLayout->addWidget(d->inputEdit);
    bottomLayout->addWidget(d->sendButton);
    mainLayout->addLayout(bottomLayout);

    d->statusLabel = new QLabel("Готов", this);
    mainLayout->addWidget(d->statusLabel);

    rebuildAgent();

    // Сигналы
    connect(d->sendButton, &QPushButton::clicked, this, &MainWindow::onSend);
    connect(d->swarmButton, &QPushButton::clicked, this, &MainWindow::onSwarm);
    connect(d->inputEdit, &QLineEdit::returnPressed, this, &MainWindow::onSend);
    connect(d->settingsButton, &QPushButton::clicked, this, &MainWindow::onSettings);
    connect(d->clearButton, &QPushButton::clicked, this, &MainWindow::onClear);
    connect(d->saveButton, &QPushButton::clicked, this, &MainWindow::saveConversation);
    connect(d->loadButton, &QPushButton::clicked, this, &MainWindow::loadConversation);
    connect(d->providerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::updateProvider);

    setWindowTitle("Synapse Agent");
    resize(900, 650);

    // Собственный курсор агента на всех виджетах окна
    OSTools::applyAgentCursor(this);
    d->chatView->setCursor(Qt::IBeamCursor);
    d->inputEdit->setCursor(Qt::IBeamCursor);
}

MainWindow::~MainWindow() = default;

void MainWindow::rebuildAgent() {
    QString active = d->providerCombo->currentText();
    ProviderConfig cfg = d->config.provider(active);
    if (cfg.name.isEmpty()) {
        return;
    }

    auto provider = createProvider(cfg, &d->network);
    if (d->agent) {
        d->agent->setProvider(std::move(provider));
        return;
    }
    d->agent = std::make_unique<ChatAgent>(std::move(provider));
    d->agent->setTools(&d->tools);

    // Установить колбэк подтверждения опасных операций
    d->agent->setConfirmationCallback([this](const QString& toolName,
                                             const QString& description) -> bool {
        return confirmDangerousAction(toolName, description);
    });
    d->agent->setPermissionCallback([this](const QString& category,
                                           const QString& description) -> bool {
        return requestPermission(category, description);
    });
}

void MainWindow::onSend() {
    QString text = d->inputEdit->text().trimmed();
    if (text.isEmpty()) {
        return;
    }

    d->inputEdit->clear();
    appendMessage("Вы", text);
    d->sendButton->setEnabled(false);
    d->inputEdit->setEnabled(false);
    d->statusLabel->setText("Думаю...");

    // Потоковый вывод ответа
    appendMessageHtml("Агент", "");

    bool ok = false;
    QString response = d->agent->processUserMessageStream(text,
        [this](const QString& chunk) {
            // Добавить фрагмент текста к последнему сообщению агента
            QTextCursor cursor = d->chatView->textCursor();
            cursor.movePosition(QTextCursor::End);
            cursor.insertText(chunk);
            d->chatView->setTextCursor(cursor);
            QScrollBar* bar = d->chatView->verticalScrollBar();
            bar->setValue(bar->maximum());
        }, ok);

    d->sendButton->setEnabled(true);
    d->inputEdit->setEnabled(true);

    if (!ok) {
        d->statusLabel->setText("Ошибка: " + response);
        appendMessage("Система", response);
    } else {
        renderHistory();
        d->statusLabel->setText("Готов");
    }
}

// Собрать группу агентов из выбранных моделей и передать общий результат директору
void MainWindow::onSwarm() {
    const QString task = d->inputEdit->text().trimmed();
    if (task.isEmpty()) {
        QMessageBox::information(this, "Рой", "Сначала введите задачу в строку сообщения");
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle("Рой агентов");
    auto* layout = new QFormLayout(&dialog);
    auto* count = new QSpinBox(&dialog);
    count->setRange(2, 100);
    count->setValue(3);
    auto* director = new QComboBox(&dialog);
    auto* workers = new QListWidget(&dialog);
    workers->setMinimumHeight(130);
    auto* teams = new QLineEdit(&dialog);
    teams->setPlaceholderText("Например: поиск, код, проверка");

    struct ModelChoice {
        QString providerName;
        QString modelName;
        QString label;
    };
    std::vector<ModelChoice> choices;
    for (const ProviderConfig& provider : d->config.providers()) {
        if (provider.apiKey.isEmpty()) {
            continue;
        }
        QStringList models;
        if (!provider.autoModels.isEmpty()) {
            models.append("auto");
        }
        models.append(provider.model);
        models.append(provider.autoModels);
        models.removeDuplicates();
        for (const QString& model : models) {
            if (model.isEmpty()) {
                continue;
            }
            const int choiceIndex = static_cast<int>(choices.size());
            const QString label = provider.name + " / " + model;
            choices.push_back({provider.name, model, label});
            director->addItem(label, choiceIndex);
            auto* item = new QListWidgetItem(label, workers);
            item->setData(Qt::UserRole, choiceIndex);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            const QString defaultWorker = provider.autoModels.size() > 1
                ? "auto" : provider.model;
            item->setCheckState(model == defaultWorker ? Qt::Checked : Qt::Unchecked);
        }
    }

    if (director->count() == 0) {
        QMessageBox::warning(this, "Рой", "Сначала сохраните API ключ хотя бы для одной модели");
        return;
    }

    for (int index = 0; index < director->count(); ++index) {
        const int choiceIndex = director->itemData(index).toInt();
        if (choices.at(choiceIndex).providerName == d->config.activeProvider() &&
            choices.at(choiceIndex).modelName == d->config.provider(d->config.activeProvider()).model) {
            director->setCurrentIndex(index);
            break;
        }
    }

    layout->addRow("Всего агентов:", count);
    layout->addRow("Директор:", director);
    layout->addRow("Модели участников:", workers);
    layout->addRow("Команды через запятую:", teams);
    layout->addRow(new QLabel("Агенты работают по очереди для экономии памяти", &dialog));
    auto* buttons = new QHBoxLayout();
    auto* start = new QPushButton("Начать", &dialog);
    auto* cancel = new QPushButton("Отмена", &dialog);
    buttons->addWidget(start);
    buttons->addWidget(cancel);
    layout->addRow(buttons);
    connect(start, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    std::vector<int> workerChoices;
    for (int index = 0; index < workers->count(); ++index) {
        QListWidgetItem* item = workers->item(index);
        if (item->checkState() == Qt::Checked) {
            workerChoices.push_back(item->data(Qt::UserRole).toInt());
        }
    }
    if (workerChoices.empty()) {
        QMessageBox::warning(this, "Рой", "Выберите хотя бы одну модель участника");
        return;
    }

    QStringList teamNames;
    for (const QString& team : teams->text().split(',', Qt::SkipEmptyParts)) {
        if (!team.trimmed().isEmpty()) {
            teamNames.append(team.trimmed());
        }
    }
    if (teamNames.isEmpty()) {
        teamNames.append("общая");
    }

    const int total = count->value();
    const ModelChoice directorChoice = choices.at(director->currentData().toInt());
    const QString cost = QString("Будет не менее %1 запросов к внешним моделям. Продолжить?")
        .arg(total + 1);
    if (QMessageBox::question(this, "Запуск роя", cost,
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    d->inputEdit->clear();
    d->sendButton->setEnabled(false);
    d->swarmButton->setEnabled(false);
    d->inputEdit->setEnabled(false);
    QProgressDialog progress("Директор составляет план", "Остановить", 0, total + 1, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);

    ProviderConfig directorConfig = d->config.provider(directorChoice.providerName);
    directorConfig.model = directorChoice.modelName;
    auto directorProvider = createProvider(directorConfig, &d->network);
    std::vector<Message> planMessages = {
        {"system", "Ты директор группы ИИ агентов. Составь краткий план и раздели задачу на части.", {}, {}},
        {"user", task + "\nУчастников: " + QString::number(total - 1) +
            "\nКоманды: " + teamNames.join(", "), {}, {}}
    };
    QString error;
    QString plan = directorProvider->chat(planMessages, error);
    if (error.isEmpty()) {
        progress.setValue(1);
    }

    QStringList findings;
    if (error.isEmpty()) {
        for (int index = 0; index < total - 1 && !progress.wasCanceled(); ++index) {
            const ModelChoice& choice = choices.at(workerChoices.at(index % workerChoices.size()));
            const QString team = teamNames.at(index % teamNames.size());
            progress.setLabelText("Работает агент " + QString::number(index + 1) +
                                  " из " + QString::number(total - 1) + " (" + choice.label + ")");

            ProviderConfig workerConfig = d->config.provider(choice.providerName);
            workerConfig.model = choice.modelName;
            auto workerProvider = createProvider(workerConfig, &d->network);
            ChatAgent worker(std::move(workerProvider));
            worker.setTools(&d->tools);
            worker.setConfirmationCallback([this](const QString& name, const QString& description) {
                return confirmDangerousAction(name, description);
            });
            worker.setPermissionCallback([this](const QString& category, const QString& description) {
                return requestPermission(category, description);
            });

            QString prompt = "Исходная задача:\n" + task + "\n\nПлан директора:\n" + plan +
                "\n\nТы агент " + QString::number(index + 1) + " из " +
                QString::number(total - 1) + ", команда «" + team + "». " +
                "Выполни полезную часть плана, проверь свои выводы и кратко сообщи результат. ";
            if (!findings.isEmpty()) {
                prompt += "\n\nУже полученные результаты:\n" + findings.join("\n").right(12000);
            }

            bool ok = false;
            QString result = worker.processUserMessage(prompt, ok);
            if (!ok) {
                error = "Агент " + QString::number(index + 1) + ": " + result;
                break;
            }
            findings.append("[" + choice.label + ", " + team + "] " + result.left(700));
            progress.setValue(index + 2);
        }
    }

    QString answer;
    if (error.isEmpty() && !progress.wasCanceled()) {
        progress.setLabelText("Директор собирает итоговый ответ");
        std::vector<Message> summaryMessages = {
            {"system", "Ты директор группы ИИ агентов. Объедини результаты, укажи проверенные факты "
                "и нерешённые вопросы. Отвечай на языке исходной задачи.", {}, {}},
            {"user", "Исходная задача:\n" + task + "\n\nПлан:\n" + plan +
                "\n\nРезультаты участников:\n" + findings.join("\n").right(30000), {}, {}}
        };
        answer = directorProvider->chat(summaryMessages, error);
    }

    d->sendButton->setEnabled(true);
    d->swarmButton->setEnabled(true);
    d->inputEdit->setEnabled(true);
    if (!error.isEmpty()) {
        d->statusLabel->setText("Рой остановлен: " + error);
        return;
    }
    if (progress.wasCanceled()) {
        d->statusLabel->setText("Рой остановлен пользователем");
        return;
    }

    d->agent->addExchange(task, answer);
    renderHistory();
    d->statusLabel->setText("Рой завершил задачу: " + QString::number(total) + " агентов");
}

void MainWindow::onSettings() {
    QDialog dialog(this);
    dialog.setWindowTitle("Настройки провайдеров");
    auto* layout = new QFormLayout(&dialog);

    auto* providerCombo = new QComboBox(&dialog);
    for (const auto& p : d->config.providers()) {
        providerCombo->addItem(p.name);
    }

    std::vector<ProviderConfig> draftProviders = d->config.providers();
    QString currentName = d->config.activeProvider();
    int selectedIndex = providerCombo->findText(currentName);
    if (selectedIndex >= 0) {
        providerCombo->setCurrentIndex(selectedIndex);
    }

    auto* keyEdit = new QLineEdit(&dialog);
    keyEdit->setEchoMode(QLineEdit::Password);
    auto* modelEdit = new QComboBox(&dialog);
    modelEdit->setEditable(true);
    auto* autoModelsEdit = new QLineEdit(&dialog);
    autoModelsEdit->setPlaceholderText("Модели по порядку, через запятую");
    auto* urlEdit = new QLineEdit(&dialog);
    auto* modelStatus = new QLabel(&dialog);
    auto* visionStatus = new QLabel(&dialog);
    auto* refreshModels = new QPushButton("Обновить список моделей", &dialog);
    QHash<QString, ModelCatalog> fetchedModels;
    int requestSerial = 0;

    auto saveCurrent = [&]() {
        for (ProviderConfig& provider : draftProviders) {
            if (provider.name != currentName) {
                continue;
            }
            provider.apiKey = keyEdit->text().trimmed();
            provider.model = modelEdit->currentText().trimmed();
            provider.baseUrl = urlEdit->text().trimmed();
            provider.autoModels.clear();
            for (const QString& name : autoModelsEdit->text().split(',', Qt::SkipEmptyParts)) {
                const QString model = name.trimmed();
                if (!model.isEmpty() && model != "auto" && !provider.autoModels.contains(model)) {
                    provider.autoModels.append(model);
                }
            }
            if (provider.model != "auto" && !provider.model.isEmpty() &&
                !provider.autoModels.contains(provider.model)) {
                provider.autoModels.prepend(provider.model);
            }
            break;
        }
    };

    auto loadProvider = [&]() {
        currentName = providerCombo->currentText();
        for (const ProviderConfig& provider : draftProviders) {
            if (provider.name != currentName) {
                continue;
            }
            keyEdit->setText(provider.apiKey);
            urlEdit->setText(provider.baseUrl);
            autoModelsEdit->setText(provider.autoModels.join(", "));
            modelEdit->clear();
            modelEdit->addItem("auto");
            for (const QString& model : provider.autoModels) {
                modelEdit->addItem(model);
            }
            for (const QString& model : fetchedModels.value(currentName).names) {
                if (modelEdit->findText(model) < 0) {
                    modelEdit->addItem(model);
                }
            }
            modelEdit->setCurrentText(provider.model);
            break;
        }
    };

    auto updateVisionStatus = [&]() {
        const ModelCatalog catalog = fetchedModels.value(currentName);
        const QString model = modelEdit->currentText().trimmed();
        if (model == "auto") {
            visionStatus->setText("Скриншоты: зависят от выбранной auto-модели");
        } else if (catalog.capabilitiesKnown && catalog.names.contains(model)) {
            visionStatus->setText(catalog.vision.contains(model)
                ? "Скриншоты: модель поддерживает vision"
                : "Скриншоты: эта модель не поддерживает vision");
        } else {
            visionStatus->setText("Скриншоты: возможности модели не проверены");
        }
    };

    auto fetchModels = [&]() {
        const QString requestedName = currentName;
        ProviderConfig requested;
        for (const ProviderConfig& provider : draftProviders) {
            if (provider.name == requestedName) {
                requested = provider;
                break;
            }
        }
        requested.apiKey = keyEdit->text().trimmed();
        requested.baseUrl = urlEdit->text().trimmed();
        const int serial = ++requestSerial;
        modelStatus->setText("Загрузка моделей…");
        requestModelList(requested, &d->network, &dialog,
            [&, serial, requestedName](const ModelCatalog& catalog, const QString& error) {
                if (serial != requestSerial || requestedName != currentName) {
                    return;
                }
                if (!error.isEmpty()) {
                    modelStatus->setText("Список недоступен: " + error);
                    return;
                }
                fetchedModels.insert(requestedName, catalog);
                const QString selected = modelEdit->currentText();
                for (const QString& name : catalog.names) {
                    if (modelEdit->findText(name) < 0) {
                        modelEdit->addItem(name);
                    }
                }
                modelEdit->setCurrentText(selected);
                modelStatus->setText("Получено моделей: " +
                    QString::number(catalog.names.size()));
                updateVisionStatus();
            });
    };

    connect(providerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
            [&](int) {
                saveCurrent();
                loadProvider();
                updateVisionStatus();
                fetchModels();
            });
    loadProvider();
    updateVisionStatus();

    layout->addRow("Провайдер:", providerCombo);
    layout->addRow("API ключ:", keyEdit);
    layout->addRow("Модель:", modelEdit);
    layout->addRow(refreshModels);
    layout->addRow(modelStatus);
    layout->addRow(visionStatus);
    layout->addRow("Порядок auto:", autoModelsEdit);
    layout->addRow("Базовый URL:", urlEdit);

    auto* accessGroup = new QGroupBox("Постоянные разрешения", &dialog);
    auto* accessLayout = new QVBoxLayout(accessGroup);
    std::vector<std::pair<QString, QCheckBox*>> permissionChecks;
    const std::vector<std::pair<QString, QString>> categories = {
        {"read", "Чтение файлов и сведений о системе"},
        {"shell", "Команды оболочки"},
        {"change", "Изменение файлов и системы"},
        {"screen", "Снимки экрана"},
        {"lan", "Сообщения другим агентам через LAN"}
    };
    for (const auto& category : categories) {
        auto* check = new QCheckBox(category.second, accessGroup);
        check->setChecked(d->config.permissionAllowed(category.first));
        accessLayout->addWidget(check);
        permissionChecks.emplace_back(category.first, check);
    }
    layout->addRow(accessGroup);

    connect(refreshModels, &QPushButton::clicked, &dialog, fetchModels);
    connect(modelEdit, &QComboBox::currentTextChanged, &dialog, updateVisionStatus);
    connect(keyEdit, &QLineEdit::editingFinished, &dialog, fetchModels);
    connect(urlEdit, &QLineEdit::editingFinished, &dialog, fetchModels);
    fetchModels();

    auto* buttons = new QHBoxLayout();
    auto* saveBtn = new QPushButton("Сохранить", &dialog);
    auto* cancelBtn = new QPushButton("Отмена", &dialog);
    buttons->addWidget(saveBtn);
    buttons->addWidget(cancelBtn);
    layout->addRow(buttons);

    connect(saveBtn, &QPushButton::clicked, [&]() {
        saveCurrent();
        for (const ProviderConfig& provider : draftProviders) {
            if (provider.model.isEmpty() ||
                (provider.model == "auto" && provider.autoModels.isEmpty())) {
                QMessageBox::warning(&dialog, "Ошибка", "Укажите модель или список для auto: " + provider.name);
                return;
            }
            if (provider.autoModels.size() > 8) {
                QMessageBox::warning(&dialog, "Ошибка", "Для auto допускается до 8 моделей: " + provider.name);
                return;
            }
        }

        AgentConfig candidate = d->config;
        candidate.setProviders(draftProviders, d->config.activeProvider());
        for (const auto& check : permissionChecks) {
            candidate.setPermissionAllowed(check.first, check.second->isChecked());
        }
        if (!candidate.save()) {
            QMessageBox::warning(&dialog, "Ошибка", "Не удалось сохранить настройки");
            return;
        }
        d->config = std::move(candidate);
        QSignalBlocker blocker(d->providerCombo);
        fillProviderCombo(d->providerCombo, d->config);
        rebuildAgent();
        dialog.accept();
    });

    connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
    dialog.exec();
}

void MainWindow::onClear() {
    d->chatView->clear();
    d->agent->clearHistory();
    d->statusLabel->setText("История очищена");
}

void MainWindow::updateProvider() {
    d->config.setActiveProvider(d->providerCombo->currentText());
    if (!d->config.save()) {
        d->statusLabel->setText("Не удалось сохранить выбор провайдера");
    }
    rebuildAgent();
}

void MainWindow::saveConversation() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    dir += "/synapse-agent/conversations";
    QDir().mkpath(dir);

    QString path = QFileDialog::getSaveFileName(
        this, "Сохранить разговор", dir, "JSON (*.json)");

    if (path.isEmpty()) {
        return;
    }

    if (d->agent->saveHistory(path)) {
        d->statusLabel->setText("Разговор сохранён");
    } else {
        d->statusLabel->setText("Не удалось сохранить");
    }
}

void MainWindow::loadConversation() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    dir += "/synapse-agent/conversations";

    QString path = QFileDialog::getOpenFileName(
        this, "Загрузить разговор", dir, "JSON (*.json)");

    if (path.isEmpty()) {
        return;
    }

    if (d->agent->loadHistory(path)) {
        renderHistory();
        d->statusLabel->setText("Разговор загружен (" + QString::number(d->agent->historySize()) + " сообщений)");
    } else {
        d->statusLabel->setText("Не удалось загрузить");
    }
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
    // Ctrl+L - очистить историю
    if (event->modifiers() & Qt::ControlModifier && event->key() == Qt::Key_L) {
        onClear();
        return;
    }

    // Ctrl+S - сохранить разговор
    if (event->modifiers() & Qt::ControlModifier && event->key() == Qt::Key_S) {
        saveConversation();
        return;
    }

    // Ctrl+O - загрузить разговор
    if (event->modifiers() & Qt::ControlModifier && event->key() == Qt::Key_O) {
        loadConversation();
        return;
    }

    QMainWindow::keyPressEvent(event);
}

bool MainWindow::confirmDangerousAction(const QString& toolName, const QString& description) {
    QString action = toolName + ": " + description;
    QMessageBox::StandardButton result = QMessageBox::warning(
        this,
        "Подтверждение операции",
        "Агент запрашивает выполнение потенциально опасной операции:\n\n" +
        action + "\n\nРазрешить?",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);

    return result == QMessageBox::Yes;
}

bool MainWindow::requestPermission(const QString& category, const QString& description) {
    if (d->config.permissionAllowed(category)) {
        return true;
    }

    QMessageBox::StandardButton result = QMessageBox::question(
        this,
        "Запрос доступа",
        "Агенту нужен доступ к группе «" + category + "»:\n\n" +
        description + "\n\nРазрешить один раз? Постоянный доступ задаётся в настройках",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    return result == QMessageBox::Yes;
}

QString MainWindow::markdownToHtml(const QString& md) const {
    QString html = md.toHtmlEscaped();

    // Жирный: **text** или __text__
    QRegularExpression bold(R"((\*\*|__)(.+?)\1)");
    html.replace(bold, "<b>\\2</b>");

    // Курсив: *text* или _text_
    QRegularExpression italic(R"((\*|_)(.+?)\1)");
    html.replace(italic, "<i>\\2</i>");

    // Код inline: `text`
    QRegularExpression inlineCode(R"(`(.+?)`)");
    html.replace(inlineCode, "<code>\\1</code>");

    // Блок кода: ```...```
    QRegularExpression codeBlock("```(.*?)```");
    codeBlock.setPatternOptions(QRegularExpression::DotMatchesEverythingOption);
    html.replace(codeBlock, "<pre><code>\\1</code></pre>");

    // Заголовки: # text
    QRegularExpression h3(R"(^### (.+)$)", QRegularExpression::MultilineOption);
    html.replace(h3, "<h3>\\1</h3>");
    QRegularExpression h2(R"(^## (.+)$)", QRegularExpression::MultilineOption);
    html.replace(h2, "<h2>\\1</h2>");
    QRegularExpression h1(R"(^# (.+)$)", QRegularExpression::MultilineOption);
    html.replace(h1, "<h1>\\1</h1>");

    // Списки: - item
    QRegularExpression listItem(R"(^- (.+)$)", QRegularExpression::MultilineOption);
    html.replace(listItem, "&bull; \\1<br>");

    // Переводы строк
    html.replace("\n", "<br>");

    return html;
}

void MainWindow::appendMessage(const QString& sender, const QString& text) {
    d->chatView->append("<b>" + sender + ":</b> " + text.toHtmlEscaped());
    QScrollBar* bar = d->chatView->verticalScrollBar();
    bar->setValue(bar->maximum());
}

void MainWindow::appendMessageHtml(const QString& sender, const QString& html) {
    d->chatView->append("<b>" + sender + ":</b> " + html);
    QScrollBar* bar = d->chatView->verticalScrollBar();
    bar->setValue(bar->maximum());
}

void MainWindow::renderHistory() {
    d->chatView->clear();
    for (const Message& message : d->agent->history()) {
        if (message.role == "user") {
            appendMessage("Вы", message.content);
        } else if (message.role == "assistant" && !message.content.isEmpty()) {
            appendMessageHtml("Агент", markdownToHtml(message.content));
        }
    }
}
