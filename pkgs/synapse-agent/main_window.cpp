/*
    synapse-agent/main_window.cpp
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI musl
    Статус доступа: общедоступный

    Графический интерфейс агента: стриминг, Markdown, подтверждения, горячие клавиши
    Подсистема: пользовательские приложения
    Первое изменение: 24.11.2026
    Последнее изменение: 24.11.2026
*/

#include "main_window.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QTextCursor>
#include <QVBoxLayout>

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
    d->clearButton = new QPushButton("Очистить", this);
    d->saveButton = new QPushButton("Сохранить", this);
    d->loadButton = new QPushButton("Загрузить", this);
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
    d->agent = std::make_unique<ChatAgent>(std::move(provider));
    d->agent->setTools(&d->tools);

    // Установить колбэк подтверждения опасных операций
    d->agent->setConfirmationCallback([this](const QString& toolName,
                                             const QString& description) -> bool {
        return confirmDangerousAction(toolName, description);
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
            QApplication::processEvents();
        }, ok);

    d->sendButton->setEnabled(true);
    d->inputEdit->setEnabled(true);

    if (!ok) {
        d->statusLabel->setText("Ошибка: " + response);
        appendMessage("Система", response);
    } else {
        // Перерисовать ответ как Markdown
        // Удалить последний блок (сырой текст) и заменить на Markdown
        QTextCursor cursor = d->chatView->textCursor();
        cursor.movePosition(QTextCursor::End);
        cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
        // Пометить как готовый
        d->statusLabel->setText("Готов");
    }
}

void MainWindow::onSettings() {
    QDialog dialog(this);
    dialog.setWindowTitle("Настройки провайдеров");
    auto* layout = new QFormLayout(&dialog);

    auto* providerCombo = new QComboBox(&dialog);
    for (const auto& p : d->config.providers()) {
        providerCombo->addItem(p.name);
    }

    auto* keyEdit = new QLineEdit(&dialog);
    keyEdit->setEchoMode(QLineEdit::Password);
    auto* modelEdit = new QLineEdit(&dialog);
    auto* urlEdit = new QLineEdit(&dialog);

    auto loadProvider = [&]() {
        ProviderConfig cfg = d->config.provider(providerCombo->currentText());
        keyEdit->setText(cfg.apiKey);
        modelEdit->setText(cfg.model);
        urlEdit->setText(cfg.baseUrl);
    };

    connect(providerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), loadProvider);
    loadProvider();

    layout->addRow("Провайдер:", providerCombo);
    layout->addRow("API ключ:", keyEdit);
    layout->addRow("Модель:", modelEdit);
    layout->addRow("Базовый URL:", urlEdit);

    auto* buttons = new QHBoxLayout();
    auto* saveBtn = new QPushButton("Сохранить", &dialog);
    auto* cancelBtn = new QPushButton("Отмена", &dialog);
    buttons->addWidget(saveBtn);
    buttons->addWidget(cancelBtn);
    layout->addRow(buttons);

    connect(saveBtn, &QPushButton::clicked, [&]() {
        std::vector<ProviderConfig> providers = d->config.providers();
        QString selected = providerCombo->currentText();
        for (auto& p : providers) {
            if (p.name == selected) {
                p.apiKey = keyEdit->text();
                p.model = modelEdit->text();
                p.baseUrl = urlEdit->text();
                break;
            }
        }

        d->config.setProviders(providers, d->config.activeProvider());
        d->config.save();
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
        d->chatView->clear();
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
    QMessageBox::StandardButton result = QMessageBox::warning(
        this,
        "Подтверждение операции",
        "Агент запрашивает выполнение потенциально опасной операции:\n\n" +
        description + "\n\nРазрешить?",
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
    codeBlock.setPatternOptions(QRegularExpression::DotMatchesEverythingPredicate);
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
