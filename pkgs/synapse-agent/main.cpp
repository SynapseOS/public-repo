// synapse-agent/main.cpp
/*
    Главный модуль агентской среды SynapseOS
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI musl
    Статус доступа: общедоступный

    Точка входа агентской среды с поддержкой графического и CLI режимов
    Подсистема: пользовательские приложения
    Первое изменение: 24.11.2026
    Последнее изменение: 25.11.2026
*/

#include "agent_config.h"
#include "chat_agent.h"
#include "main_window.h"
#include "os_tools.h"
#include "ai_provider.h"

#include <QApplication>
#include <QDir>
#include <QNetworkAccessManager>
#include <QStandardPaths>
#include <QTextStream>
#include <iostream>
#include <memory>

// Запустить агентскую среду в консольном режиме
static int runCliMode(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    AgentConfig config;
    config.load();

    QString providerName = config.activeProvider();
    if (argc >= 3) {
        providerName = QString::fromUtf8(argv[2]);
    }

    ProviderConfig cfg = config.provider(providerName);
    if (cfg.name.isEmpty()) {
        std::cerr << "Провайдер не найден: " << providerName.toStdString() << std::endl;
        return 1;
    }

    if (cfg.apiKey.isEmpty()) {
        std::cerr << "API ключ не задан для провайдера " << providerName.toStdString() << std::endl;
        std::cerr << "Укажите ключ через настройки или файл " << AgentConfig::configPath().toStdString() << std::endl;
        return 1;
    }

    QNetworkAccessManager network;
    auto provider = createProvider(cfg, &network);
    OSTools tools;
    ChatAgent agent(std::move(provider));
    agent.setTools(&tools);

    QTextStream cin_stream(stdin);
    QTextStream cout_stream(stdout);

    // Подтверждать действия, меняющие систему
    agent.setConfirmationCallback([&cin_stream](const QString&, const QString& desc) -> bool {
        std::cerr << "[Подтверждение] " << desc.toStdString() << " (y/N) " << std::flush;
        QString answer = cin_stream.readLine().trimmed();
        return answer == "y" || answer == "Y";
    });

    agent.setPermissionCallback([&config, &cin_stream](const QString& category,
                                                      const QString& desc) -> bool {
        if (config.permissionAllowed(category)) {
            return true;
        }

        std::cerr << "[Запрос доступа: " << category.toStdString() << "] "
                  << desc.toStdString() << " (y/N) " << std::flush;
        QString answer = cin_stream.readLine().trimmed();
        return answer == "y" || answer == "Y";
    });

    cout_stream << "Synapse Agent CLI (" << providerName << ", " << cfg.model << ")" << Qt::endl;
    cout_stream << "Команды: /exit - выход, /clear - очистить, /save - сохранить, /load - загрузить" << Qt::endl;
    cout_stream.flush();

    QString convDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    convDir += "/synapse-agent/conversations";

    while (true) {
        cout_stream << "> ";
        cout_stream.flush();

        QString line = cin_stream.readLine();
        if (line.isNull()) {
            break;
        }
        if (line.trimmed() == "/exit") {
            break;
        }

        if (line.trimmed() == "/clear") {
            agent.clearHistory();
            cout_stream << "История очищена" << Qt::endl;
            continue;
        }

        if (line.trimmed() == "/save") {
            QDir().mkpath(convDir);
            QString path = convDir + "/cli-" +
                QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss") + ".json";
            if (agent.saveHistory(path)) {
                cout_stream << "Сохранено: " << path << Qt::endl;
            } else {
                cout_stream << "Ошибка сохранения" << Qt::endl;
            }
            continue;
        }

        if (line.trimmed().startsWith("/load ")) {
            QString path = line.trimmed().mid(7);
            if (agent.loadHistory(path)) {
                cout_stream << "Загружено: " << agent.historySize() << " сообщений" << Qt::endl;
            } else {
                cout_stream << "Ошибка загрузки" << Qt::endl;
            }
            continue;
        }

        // Потоковый вывод в CLI
        bool ok = false;
        QString response = agent.processUserMessageStream(line,
            [&cout_stream](const QString& chunk) {
                cout_stream << chunk;
                cout_stream.flush();
            }, ok);

        if (!ok) {
            cout_stream << "\n[Ошибка] " << response << Qt::endl;
        } else {
            cout_stream << Qt::endl;
        }
        cout_stream.flush();
    }

    return 0;
}

// Запустить графический интерфейс
static int runGuiMode(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("synapse-agent");

    MainWindow window;
    window.show();

    return app.exec();
}

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        QString arg = QString::fromUtf8(argv[i]);
        if (arg == "--cli" || arg == "-c") {
            return runCliMode(argc, argv);
        }

        if (arg == "--help" || arg == "-h") {
            std::cout << "Synapse Agent - агентская среда SynapseOS" << std::endl;
            std::cout << std::endl;
            std::cout << "Использование:" << std::endl;
            std::cout << "  synapse-agent            Запустить графический интерфейс" << std::endl;
            std::cout << "  synapse-agent --cli      Запустить консольный режим" << std::endl;
            std::cout << "  synapse-agent --cli <провайдер>  Указать провайдера" << std::endl;
            std::cout << "  synapse-agent --help     Показать справку" << std::endl;
            std::cout << std::endl;
            std::cout << "Горячие клавиши (GUI):" << std::endl;
            std::cout << "  Ctrl+L  Очистить историю" << std::endl;
            std::cout << "  Ctrl+S  Сохранить разговор" << std::endl;
            std::cout << "  Ctrl+O  Загрузить разговор" << std::endl;
            std::cout << std::endl;
            std::cout << "Команды CLI:" << std::endl;
            std::cout << "  /exit   Выход" << std::endl;
            std::cout << "  /clear  Очистить историю" << std::endl;
            std::cout << "  /save   Сохранить разговор" << std::endl;
            std::cout << "  /load <путь>  Загрузить разговор" << std::endl;
            return 0;
        }
    }

    return runGuiMode(argc, argv);
}
