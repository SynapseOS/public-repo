/*
    synapse-agent/chat_agent.h
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI musl
    Статус доступа: общедоступный

    Оркестратор диалога, исполнения инструментов и управления контекстом
    Подсистема: пользовательские приложения
    Первое изменение: 24.11.2026
    Последнее изменение: 25.11.2026
*/

#ifndef CHAT_AGENT_H
#define CHAT_AGENT_H

#include "ai_provider.h"
#include "os_tools.h"
#include <QJsonObject>
#include <QString>
#include <memory>
#include <vector>

// Максимальное число сообщений в истории до обрезки
static constexpr int MAX_HISTORY_MESSAGES = 40;

// Колбэк для запроса подтверждения опасной операции
// Возвращает true если операция разрешена
using ConfirmationCallback = std::function<bool(const QString& toolName,
                                                const QString& description)>;

// Запросить доступ к группе инструментов
using PermissionCallback = std::function<bool(const QString& category,
                                              const QString& description)>;

// Оркестратор диалога и исполнения инструментов
struct ChatAgent {
public:
    explicit ChatAgent(std::unique_ptr<AIProvider> provider);

    // Сменить провайдера без удаления разговора
    void setProvider(std::unique_ptr<AIProvider> provider);

    // Установить инструменты для агента
    void setTools(OSTools* tools);

    // Установить колбэк подтверждения опасных операций
    void setConfirmationCallback(const ConfirmationCallback& cb);

    // Установить обработчик запроса доступа
    void setPermissionCallback(const PermissionCallback& cb);

    // Получить полную системную подсказку
    QString buildSystemPrompt() const;

    // Обработать сообщение пользователя синхронно
    QString processUserMessage(const QString& userMessage, bool& ok);

    // Обработать сообщение пользователя с потоковым выводом
    QString processUserMessageStream(const QString& userMessage,
                                     const StreamCallback& onChunk,
                                     bool& ok);

    // Очистить историю
    void clearHistory();

    // Сохранить историю в файл
    bool saveHistory(const QString& path) const;

    // Загрузить историю из файла
    bool loadHistory(const QString& path);

    // Текущее число сообщений в истории
    int historySize() const;

    // Сообщения текущего разговора для отображения в окне
    const std::vector<Message>& history() const;

    // Добавить результат работы роя в текущий разговор
    void addExchange(const QString& userText, const QString& assistantText);

private:
    QString executeTool(const QString& name, const QJsonObject& arguments);
    std::vector<std::pair<QString, QJsonObject>> parseToolCalls(const QString& text) const;
    QString stripToolCalls(const QString& text) const;
    void trimHistory();
    bool isDangerousTool(const QString& name) const;
    QString describeToolCall(const QString& name, const QJsonObject& arguments) const;

    std::unique_ptr<AIProvider> m_provider;
    OSTools* m_tools;
    ConfirmationCallback m_confirmCallback;
    PermissionCallback m_permissionCallback;
    std::vector<Message> m_history;
    std::vector<ToolDescription> m_toolDescriptions;

    // Отложенное вложение из инструмента скриншота
    Attachment m_pendingAttachment;
    bool m_hasPendingAttachment = false;
};

#endif
