/*
    synapse-agent/ai_provider.cpp
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI musl
    Статус доступа: общедоступный

    Реализации провайдеров ИИ: синхронный, потоковый и мультимодальный режимы
    Подсистема: пользовательские приложения
    Первое изменение: 24.11.2026
    Последнее изменение: 24.11.2026
*/

#include "ai_provider.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace {

// Ожидание ответа с таймаутом (для синхронного режима)
static QByteArray waitForReply(QNetworkReply* reply, int timeoutMs, QString& outError) {
    QByteArray result;
    if (!reply) {
        outError = QString::fromUtf8("Нет соединения");
        return result;
    }

    QTimer timer;
    QEventLoop loop;
    if (reply->isRunning()) {
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        timer.start(timeoutMs);
        loop.exec();
    }

    if (reply->error() != QNetworkReply::NoError) {
        outError = reply->errorString();
        reply->deleteLater();
        return result;
    }

    result = reply->readAll();
    reply->deleteLater();
    return result;
}

// Разобрать одну строку SSE и извлечь текст дельты (OpenAI формат)
static QString parseOpenAISSEChunk(const QByteArray& line) {
    if (!line.startsWith("data: ")) {
        return QString();
    }

    QByteArray json = line.mid(6).trimmed();
    if (json == "[DONE]") {
        return QString();
    }

    QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        return QString();
    }

    QJsonArray choices = doc.object().value("choices").toArray();
    if (choices.isEmpty()) {
        return QString();
    }

    QJsonObject delta = choices.first().toObject().value("delta").toObject();
    return delta.value("content").toString();
}

// Разобрать одну строку SSE (Anthropic формат)
static QString parseAnthropicSSEChunk(const QByteArray& line) {
    if (!line.startsWith("data: ")) {
        return QString();
    }

    QByteArray json = line.mid(6).trimmed();
    QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        return QString();
    }

    QJsonObject obj = doc.object();
    QString type = obj.value("type").toString();

    if (type == "content_block_delta") {
        return obj.value("delta").toObject().value("text").toString();
    }

    return QString();
}

// Потоковое чтение SSE с колбэком
static QString readSSEStream(QNetworkReply* reply, int timeoutMs,
                             const StreamCallback& onChunk,
                             const std::function<QString(const QByteArray&)>& parseChunk,
                             QString& outError) {
    QString fullResponse;
    if (!reply) {
        outError = "Нет соединения";
        return fullResponse;
    }

    QTimer timer;
    QEventLoop loop;
    bool finished = false;

    QObject::connect(reply, &QNetworkReply::readyRead, [&]() {
        while (reply->canReadLine()) {
            QByteArray line = reply->readLine().trimmed();
            if (line.isEmpty()) {
                continue;
            }

            QString chunk = parseChunk(line);
            if (!chunk.isEmpty()) {
                fullResponse += chunk;
                if (onChunk) {
                    onChunk(chunk);
                }
            }
        }
    });

    QObject::connect(reply, &QNetworkReply::finished, [&]() {
        finished = true;
        loop.quit();
    });

    QObject::connect(&timer, &QTimer::timeout, [&]() {
        if (!finished) {
            outError = "Таймаут стриминга";
            loop.quit();
        }
    });

    timer.start(timeoutMs);
    loop.exec();
    timer.stop();

    if (reply->error() != QNetworkReply::NoError && !finished) {
        outError = reply->errorString();
    }

    reply->deleteLater();
    return fullResponse;
}

// Собрать массив сообщений в формате OpenAI с поддержкой вложений
static QJsonArray formatOpenAIMessages(const std::vector<Message>& messages) {
    QJsonArray msgs;
    for (const auto& msg : messages) {
        QJsonObject obj;
        obj.insert("role", msg.role);
        if (!msg.toolCallId.isEmpty()) {
            obj.insert("name", msg.toolCallId);
        }

        if (msg.attachments.empty()) {
            obj.insert("content", msg.content);
        } else {
            // Мультимодальный формат: массив из текста и изображений
            QJsonArray contentArr;
            QJsonObject textPart;
            textPart.insert("type", "text");
            textPart.insert("text", msg.content);
            contentArr.append(textPart);

            for (const auto& att : msg.attachments) {
                if (att.type != "image") {
                    continue;
                }
                QJsonObject imagePart;
                imagePart.insert("type", "image_url");
                QJsonObject urlObj;
                urlObj.insert("url", "data:" + att.mediaType + ";base64," + att.data);
                imagePart.insert("image_url", urlObj);
                contentArr.append(imagePart);
            }

            obj.insert("content", contentArr);
        }

        msgs.append(obj);
    }
    return msgs;
}

// Собрать массив сообщений в формате Anthropic с поддержкой вложений
static QJsonArray formatAnthropicMessages(const std::vector<Message>& messages, QString& outSystem) {
    QJsonArray msgs;
    outSystem.clear();
    for (const auto& msg : messages) {
        if (msg.role == "system") {
            outSystem = msg.content;
            continue;
        }

        QJsonObject obj;
        QString role = msg.role;
        if (role == "tool") {
            role = "user";
        }
        obj.insert("role", role);

        if (msg.attachments.empty()) {
            obj.insert("content", msg.content);
        } else {
            QJsonArray contentArr;
            QJsonObject textPart;
            textPart.insert("type", "text");
            textPart.insert("text", msg.content);
            contentArr.append(textPart);

            for (const auto& att : msg.attachments) {
                if (att.type != "image") {
                    continue;
                }
                QJsonObject imagePart;
                imagePart.insert("type", "image");
                QJsonObject sourceObj;
                sourceObj.insert("type", "base64");
                sourceObj.insert("media_type", att.mediaType);
                sourceObj.insert("data", att.data);
                imagePart.insert("source", sourceObj);
                contentArr.append(imagePart);
            }

            obj.insert("content", contentArr);
        }

        msgs.append(obj);
    }
    return msgs;
}

} // namespace

AIProvider::AIProvider(const ProviderConfig& config, QNetworkAccessManager* net)
    : m_config(config)
    , m_network(net) {
}

AIProvider::~AIProvider() = default;

QString AIProvider::lastError() const {
    return m_lastError;
}

// Провайдер, совместимый с OpenAI API
class OpenAIProvider : public AIProvider {
public:
    OpenAIProvider(const ProviderConfig& config, QNetworkAccessManager* net)
        : AIProvider(config, net) {
    }

    bool supportsVision() const override {
        // Основные мультимодальные модели OpenAI
        return m_config.model.contains("gpt-4o") || m_config.model.contains("gpt-4-vision");
    }

    QString chat(const std::vector<Message>& messages,
                 QString& outError) override {
        QJsonObject body;
        body.insert("model", m_config.model);
        body.insert("messages", formatOpenAIMessages(messages));
        body.insert("temperature", 0.3);

        QJsonDocument doc(body);
        QNetworkRequest request(QUrl(m_config.baseUrl + "/chat/completions"));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("Authorization", ("Bearer " + m_config.apiKey).toUtf8());

        QNetworkReply* reply = m_network->post(request, doc.toJson());
        QByteArray response = waitForReply(reply, 120000, outError);
        if (!outError.isEmpty()) {
            m_lastError = outError;
            return QString();
        }

        QJsonObject root = QJsonDocument::fromJson(response).object();
        QJsonArray choices = root.value("choices").toArray();
        if (choices.isEmpty()) {
            outError = "Провайдер вернул пустой choices";
            m_lastError = outError;
            return QString();
        }

        QJsonObject message = choices.first().toObject().value("message").toObject();
        return message.value("content").toString();
    }

    QString chatStream(const std::vector<Message>& messages,
                       const StreamCallback& onChunk,
                       QString& outError) override {
        QJsonObject body;
        body.insert("model", m_config.model);
        body.insert("messages", formatOpenAIMessages(messages));
        body.insert("temperature", 0.3);
        body.insert("stream", true);

        QJsonDocument doc(body);
        QNetworkRequest request(QUrl(m_config.baseUrl + "/chat/completions"));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("Authorization", ("Bearer " + m_config.apiKey).toUtf8());

        QNetworkReply* reply = m_network->post(request, doc.toJson());
        return readSSEStream(reply, 120000, onChunk, parseOpenAISSEChunk, outError);
    }
};

// Провайдер для Anthropic Claude API
class AnthropicProvider : public AIProvider {
public:
    AnthropicProvider(const ProviderConfig& config, QNetworkAccessManager* net)
        : AIProvider(config, net) {
    }

    bool supportsVision() const override {
        // Claude 3 и новее поддерживают изображения
        return m_config.model.contains("claude-3");
    }

    QString chat(const std::vector<Message>& messages,
                 QString& outError) override {
        QString systemText;
        QJsonArray msgs = formatAnthropicMessages(messages, systemText);

        QJsonObject body;
        body.insert("model", m_config.model);
        body.insert("max_tokens", 4096);
        body.insert("messages", msgs);
        if (!systemText.isEmpty()) {
            body.insert("system", systemText);
        }

        QJsonDocument doc(body);
        QNetworkRequest request(QUrl(m_config.baseUrl + "/v1/messages"));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("x-api-key", m_config.apiKey.toUtf8());
        request.setRawHeader("anthropic-version", "2023-06-01");

        QNetworkReply* reply = m_network->post(request, doc.toJson());
        QByteArray response = waitForReply(reply, 120000, outError);
        if (!outError.isEmpty()) {
            m_lastError = outError;
            return QString();
        }

        QJsonObject root = QJsonDocument::fromJson(response).object();
        QJsonArray content = root.value("content").toArray();
        QString result;
        for (const auto& item : content) {
            result += item.toObject().value("text").toString();
        }
        return result;
    }

    QString chatStream(const std::vector<Message>& messages,
                       const StreamCallback& onChunk,
                       QString& outError) override {
        QString systemText;
        QJsonArray msgs = formatAnthropicMessages(messages, systemText);

        QJsonObject body;
        body.insert("model", m_config.model);
        body.insert("max_tokens", 4096);
        body.insert("messages", msgs);
        body.insert("stream", true);
        if (!systemText.isEmpty()) {
            body.insert("system", systemText);
        }

        QJsonDocument doc(body);
        QNetworkRequest request(QUrl(m_config.baseUrl + "/v1/messages"));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("x-api-key", m_config.apiKey.toUtf8());
        request.setRawHeader("anthropic-version", "2023-06-01");

        QNetworkReply* reply = m_network->post(request, doc.toJson());
        return readSSEStream(reply, 120000, onChunk, parseAnthropicSSEChunk, outError);
    }
};

std::unique_ptr<AIProvider> createProvider(const ProviderConfig& config,
                                          QNetworkAccessManager* net) {
    if (config.type == "anthropic") {
        return std::make_unique<AnthropicProvider>(config, net);
    }
    return std::make_unique<OpenAIProvider>(config, net);
}
