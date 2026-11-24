/*
    synapse-agent/agent_config.h
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI musl
    Статус доступа: общедоступный

    Загрузка и сохранение настроек агента
    Подсистема: пользовательские приложения
    Первое изменение: 24.11.2026
    Последнее изменение: 24.11.2026
*/

#ifndef AGENT_CONFIG_H
#define AGENT_CONFIG_H

#include "ai_provider.h"
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <vector>

// Класс для хранения конфигурации агента
class AgentConfig {
public:
    AgentConfig();

    // Загрузить конфигурацию из файла пользователя или системного
    bool load();

    // Сохранить конфигурацию
    bool save() const;

    // Получить список провайдеров
    std::vector<ProviderConfig> providers() const;

    // Получить активный провайдер по имени
    ProviderConfig provider(const QString& name) const;

    // Установить список провайдеров и активного
    void setProviders(const std::vector<ProviderConfig>& providers, const QString& active);

    // Имя активного провайдера
    QString activeProvider() const;
    void setActiveProvider(const QString& name);

    // Путь к файлу конфигурации
    static QString configPath();

private:
    QJsonObject toJson() const;
    void fromJson(const QJsonObject& obj);

    std::vector<ProviderConfig> m_providers;
    QString m_activeProvider;
};

#endif
