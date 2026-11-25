/*
    synapse-agent/ai_provider.cpp
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI musl
    Статус доступа: общедоступный

    Реализации провайдеров ИИ: синхронный, потоковый и мультимодальный режимы
    Подсистема: пользовательские приложения
    Первое изменение: 24.11.2026
    Последнее изменение: 25.11.2026
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

// Собрать адрес запроса и не передавать ключ по незашифрованному соединению
static QUrl providerUrl(const ProviderConfig& config, const QString& suffix, QString& outError) {
    QUrl url(config.baseUrl.trimmed());
    if (config.apiKey.isEmpty()) {
        outError = "API ключ не задан";
        return QUrl();
    }
    if (!url.isValid() || url.scheme() != "https" || url.host().isEmpty() ||
        !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) {
        outError = "Базовый URL должен быть адресом HTTPS без параметров";
        return QUrl();
    }

    QString path = url.path();
    while (path.endsWith('/')) {
        path.chop(1);
    }
    url.setPath(path + suffix);
    return url;
}

// Ожидание ответа с таймаутом (для синхронного режима)
static QByteArray waitForReply(QNetworkReply* reply, int timeoutMs, QString& outError) {
    QByteArray result;
    if (!reply) {
        outError = QString::fromUtf8("Нет соединения");
        return result;
    }

    QTimer timer;
    QEventLoop loop;
    bool timedOut = false;
    if (reply->isRunning()) {
        QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
            timedOut = true;
            loop.quit();
        });
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        timer.start(timeoutMs);
        loop.exec();
    }

    if (timedOut) {
        reply->abort();
        outError = "Провайдер не ответил вовремя";
        reply->deleteLater();
        return result;
    }

    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 400) {
        outError = "HTTP " + QString::number(status);
        reply->deleteLater();
        return result;
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

    if (reply->isRunning()) {
        timer.start(timeoutMs);
        loop.exec();
        timer.stop();
    }

    if (!finished && !reply->isFinished()) {
        reply->abort();
    }
    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (outError.isEmpty() && status >= 400) {
        outError = "HTTP " + QString::number(status);
    }
    if (outError.isEmpty() && reply->error() != QNetworkReply::NoError) {
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
        const bool toolResult = msg.role == "tool";
        obj.insert("role", toolResult ? "user" : msg.role);
        const QString content = toolResult
            ? "Результат инструмента " + msg.toolCallId + ":\n" + msg.content
            : msg.content;

        if (msg.attachments.empty()) {
            obj.insert("content", content);
        } else {
            // Мультимодальный формат: массив из текста и изображений
            QJsonArray contentArr;
            QJsonObject textPart;
            textPart.insert("type", "text");
            textPart.insert("text", content);
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
        const QString content = msg.role == "tool"
            ? "Результат инструмента " + msg.toolCallId + ":\n" + msg.content
            : msg.content;

        if (msg.attachments.empty()) {
            obj.insert("content", content);
        } else {
            QJsonArray contentArr;
            QJsonObject textPart;
            textPart.insert("type", "text");
            textPart.insert("text", content);
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
        QUrl url = providerUrl(m_config, "/chat/completions", outError);
        if (!outError.isEmpty()) {
            return QString();
        }

        QJsonObject body;
        body.insert("model", m_config.model);
        body.insert("messages", formatOpenAIMessages(messages));
        body.insert("temperature", 0.3);

        QJsonDocument doc(body);
        QNetworkRequest request(url);
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
        QUrl url = providerUrl(m_config, "/chat/completions", outError);
        if (!outError.isEmpty()) {
            return QString();
        }

        QJsonObject body;
        body.insert("model", m_config.model);
        body.insert("messages", formatOpenAIMessages(messages));
        body.insert("temperature", 0.3);
        body.insert("stream", true);

        QJsonDocument doc(body);
        QNetworkRequest request(url);
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
        QUrl url = providerUrl(m_config, "/v1/messages", outError);
        if (!outError.isEmpty()) {
            return QString();
        }

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
        QNetworkRequest request(url);
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
        QUrl url = providerUrl(m_config, "/v1/messages", outError);
        if (!outError.isEmpty()) {
            return QString();
        }

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
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("x-api-key", m_config.apiKey.toUtf8());
        request.setRawHeader("anthropic-version", "2023-06-01");

        QNetworkReply* reply = m_network->post(request, doc.toJson());
        return readSSEStream(reply, 120000, onChunk, parseAnthropicSSEChunk, outError);
    }
};

// Переключать настроенные модели после лимита или недоступности модели
class AutoProvider : public AIProvider {
public:
    AutoProvider(const ProviderConfig& config, QNetworkAccessManager* net)
        : AIProvider(config, net) {
        for (const QString& model : config.autoModels) {
            QString name = model.trimmed();
            if (!name.isEmpty() && name != "auto" && !m_models.contains(name)) {
                m_models.append(name);
            }
        }
    }

    bool supportsVision() const override {
        return false;
    }

    QString chat(const std::vector<Message>& messages, QString& outError) override {
        return tryModels(messages, {}, outError, false);
    }

    QString chatStream(const std::vector<Message>& messages, const StreamCallback& onChunk,
                       QString& outError) override {
        return tryModels(messages, onChunk, outError, true);
    }

private:
    // Выполнить запрос по очереди через доступные модели
    QString tryModels(const std::vector<Message>& messages, const StreamCallback& onChunk,
                      QString& outError, bool streaming) {
        if (m_models.isEmpty()) {
            outError = "Для auto укажите модели в настройках";
            return QString();
        }

        const int attempts = qMin(static_cast<int>(m_models.size()), 8);
        for (int attempt = 0; attempt < attempts; ++attempt) {
            int index = (m_current + attempt) % m_models.size();
            ProviderConfig selected = m_config;
            selected.model = m_models.at(index);
            auto provider = createProvider(selected, m_network);
            QString error;
            QString response;
            if (streaming) {
                response = provider->chatStream(messages, {}, error);
            } else {
                response = provider->chat(messages, error);
            }

            if (error.isEmpty()) {
                m_current = index;
                outError.clear();
                if (streaming && onChunk && !response.isEmpty()) {
                    onChunk(response);
                }
                return response;
            }

            outError = error;
            bool limited = error.startsWith("HTTP 400") || error.startsWith("HTTP 402") ||
                error.startsWith("HTTP 403") ||
                error.startsWith("HTTP 404") || error.startsWith("HTTP 429") ||
                error.startsWith("HTTP 503");
            if (!limited || !response.isEmpty()) {
                return QString();
            }
        }

        outError = "Все модели auto недоступны: " + outError;
        return QString();
    }

    QStringList m_models;
    int m_current = 0;
};

std::unique_ptr<AIProvider> createProvider(const ProviderConfig& config,
                                          QNetworkAccessManager* net) {
    if (config.model == "auto") {
        return std::make_unique<AutoProvider>(config, net);
    }
    if (config.type == "anthropic") {
        return std::make_unique<AnthropicProvider>(config, net);
    }
    return std::make_unique<OpenAIProvider>(config, net);
}
