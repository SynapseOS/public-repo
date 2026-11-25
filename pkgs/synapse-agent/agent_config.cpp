/*
    synapse-agent/agent_config.cpp
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI musl
    Статус доступа: общедоступный

    Реализация загрузки и сохранения настроек агента
    Подсистема: пользовательские приложения
    Первое изменение: 24.11.2026
    Последнее изменение: 25.11.2026
*/

#include "agent_config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <sys/stat.h>

namespace {

// Заполнить конфигурацию по умолчанию с шаблонами провайдеров
std::vector<ProviderConfig> defaultProviders() {
    std::vector<ProviderConfig> list;

    ProviderConfig openai;
    openai.name = "openai";
    openai.type = "openai";
    openai.baseUrl = "https://api.openai.com/v1";
    openai.model = "gpt-4o";
    list.push_back(openai);

    ProviderConfig kimi;
    kimi.name = "kimi";
    kimi.type = "openai";
    kimi.baseUrl = "https://api.moonshot.cn/v1";
    kimi.model = "moonshot-v1-8k";
    list.push_back(kimi);

    ProviderConfig alibaba;
    alibaba.name = "qwen";
    alibaba.type = "openai";
    alibaba.baseUrl = "https://eu-central-1.maas.qwencloudapi.com/compatible-mode/v1";
    alibaba.model = "qwen-plus";
    list.push_back(alibaba);

    ProviderConfig gemini;
    gemini.name = "gemini";
    gemini.type = "openai";
    gemini.baseUrl = "https://generativelanguage.googleapis.com/v1beta/openai";
    gemini.model = "gemini-3-flash";
    list.push_back(gemini);

    ProviderConfig deepseek;
    deepseek.name = "deepseek";
    deepseek.type = "openai";
    deepseek.baseUrl = "https://api.deepseek.com";
    deepseek.model = "deepseek-chat";
    list.push_back(deepseek);

    ProviderConfig claude;
    claude.name = "claude";
    claude.type = "anthropic";
    claude.baseUrl = "https://api.anthropic.com";
    claude.model = "claude-3-5-sonnet-20241022";
    list.push_back(claude);

    return list;
}

} // namespace

AgentConfig::AgentConfig()
    : m_providers(defaultProviders())
    , m_activeProvider("openai") {
}

QString AgentConfig::configPath() {
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    QDir dir(configDir);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    return dir.filePath("synapse-agent/config.json");
}

bool AgentConfig::load() {
    QString path = configPath();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_providers = defaultProviders();
        m_activeProvider = "openai";
        m_allowedPermissions.clear();
        return false;
    }

    QByteArray data = file.readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        m_providers = defaultProviders();
        m_activeProvider = "openai";
        m_allowedPermissions.clear();
        return false;
    }

    fromJson(doc.object());
    return true;
}

bool AgentConfig::save() const {
    QString path = configPath();
    QString directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory)) {
        return false;
    }

    if (chmod(QFile::encodeName(directory).constData(), 0700) != 0) {
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    QJsonDocument doc(toJson());
    QByteArray data = doc.toJson(QJsonDocument::Indented);
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(data) != data.size() || !file.commit()) {
        return false;
    }

    return chmod(QFile::encodeName(path).constData(), 0600) == 0;
}

std::vector<ProviderConfig> AgentConfig::providers() const {
    return m_providers;
}

ProviderConfig AgentConfig::provider(const QString& name) const {
    for (const auto& p : m_providers) {
        if (p.name == name) {
            return p;
        }
    }
    return ProviderConfig();
}

void AgentConfig::setProviders(const std::vector<ProviderConfig>& providers,
                            const QString& active) {
    m_providers = providers;
    m_activeProvider = active;
}

QString AgentConfig::activeProvider() const {
    return m_activeProvider;
}

void AgentConfig::setActiveProvider(const QString& name) {
    m_activeProvider = name;
}

bool AgentConfig::permissionAllowed(const QString& category) const {
    return m_allowedPermissions.contains(category);
}

void AgentConfig::setPermissionAllowed(const QString& category, bool allowed) {
    if (allowed) {
        m_allowedPermissions.insert(category);
    } else {
        m_allowedPermissions.remove(category);
    }
}

QJsonObject AgentConfig::toJson() const {
    QJsonArray arr;
    for (const auto& p : m_providers) {
        QJsonObject obj;
        obj.insert("name", p.name);
        obj.insert("type", p.type);
        obj.insert("api_key", p.apiKey);
        obj.insert("base_url", p.baseUrl);
        obj.insert("model", p.model);
        arr.append(obj);
    }

    QJsonObject root;
    root.insert("active_provider", m_activeProvider);
    root.insert("providers", arr);
    QJsonArray permissions;
    for (const QString& category : m_allowedPermissions) {
        permissions.append(category);
    }
    root.insert("allowed_permissions", permissions);
    return root;
}

void AgentConfig::fromJson(const QJsonObject& obj) {
    m_providers.clear();
    m_allowedPermissions.clear();
    m_activeProvider = obj.value("active_provider").toString("openai");

    QJsonArray arr = obj.value("providers").toArray();
    for (const auto& val : arr) {
        QJsonObject p = val.toObject();
        ProviderConfig cfg;
        cfg.name = p.value("name").toString();
        cfg.type = p.value("type").toString();
        cfg.apiKey = p.value("api_key").toString();
        cfg.baseUrl = p.value("base_url").toString();
        cfg.model = p.value("model").toString();
        m_providers.push_back(cfg);
    }

    if (m_providers.empty()) {
        m_providers = defaultProviders();
    }

    for (const QJsonValue& value : obj.value("allowed_permissions").toArray()) {
        if (value.isString()) {
            m_allowedPermissions.insert(value.toString());
        }
    }
}
