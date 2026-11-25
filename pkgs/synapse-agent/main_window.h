/*
    synapse-agent/main_window.h
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI musl
    Статус доступа: общедоступный

    Главное окно агента SynapseOS
    Подсистема: пользовательские приложения
    Первое изменение: 24.11.2026
    Последнее изменение: 25.11.2026
*/

#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include "agent_config.h"
#include "chat_agent.h"
#include <QMainWindow>
#include <memory>

// Главное окно с интерфейсом чата
class MainWindow : public QMainWindow {
public:
    MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    void onSend();
    void onSwarm();
    void onSettings();
    void onClear();
    void updateProvider();
    void rebuildAgent();
    void appendMessage(const QString& sender, const QString& text);
    void appendMessageHtml(const QString& sender, const QString& html);
    void renderHistory();
    void saveConversation();
    void loadConversation();
    void keyPressEvent(QKeyEvent* event) override;
    bool confirmDangerousAction(const QString& toolName, const QString& description);
    bool requestPermission(const QString& category, const QString& description);
    QString markdownToHtml(const QString& md) const;

    class Impl;
    std::unique_ptr<Impl> d;
};

#endif
