/*
    synapse-agent/ai_provider.h
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI musl
    Статус доступа: общедоступный

    Абстракция провайдера ИИ: синхронный, потоковый и мультимодальный режимы
    Подсистема: пользовательские приложения
    Первое изменение: 24.11.2026
    Последнее изменение: 24.11.2026
*/

#ifndef AI_PROVIDER_H
#define AI_PROVIDER_H

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>
#include <vector>

// Структура настроек одного провайдера
struct ProviderConfig {
    QString name;
    QString type; // "openai" или "anthropic"
    QString apiKey;
    QString baseUrl;
    QString model;
    QStringList autoModels;
};

// Вложение к сообщению (изображение и тд)
struct Attachment {
    QString type;     // "image"
    QString mediaType; // "image/png"
    QString data;     // base64-закодированные данные
};

// Один диалоговый обмен
struct Message {
    QString role; // "system", "user", "assistant", "tool"
    QString content;
    QString toolCallId;
    std::vector<Attachment> attachments;
};

// Описание инструмента для включения в промпт
struct ToolDescription {
    QString name;
    QString description;
    QJsonObject schema;
};

// Колбэк для потокового получения текста
using StreamCallback = std::function<void(const QString& chunk)>;

// Базовый класс провайдера
class AIProvider {
public:
    AIProvider(const ProviderConfig& config, QNetworkAccessManager* net);
    virtual ~AIProvider();

    // Синхронный запрос
    virtual QString chat(const std::vector<Message>& messages,
                         QString& outError) = 0;

    // Потоковый запрос
    virtual QString chatStream(const std::vector<Message>& messages,
                               const StreamCallback& onChunk,
                               QString& outError) = 0;

    // Поддерживает ли провайдер изображения
    virtual bool supportsVision() const = 0;

    QString lastError() const;

protected:
    ProviderConfig m_config;
    QNetworkAccessManager* m_network;
    QString m_lastError;
};

// Создать конкретную реализацию по конфигурации
std::unique_ptr<AIProvider> createProvider(const ProviderConfig& config,
                                          QNetworkAccessManager* net);

#endif
