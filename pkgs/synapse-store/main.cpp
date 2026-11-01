/*
    main.cpp: Отдельный графический магазин программ SynapseOS
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI: synapse-x86_64-v1

    Описание: Каталог программ SynapseOS и Flatpak с явным состоянием Debian
    Подсистема: графические программы
    Первое изменение: 01.11.2026
    Последнее изменение: 01.11.2026
    Доступ: общедоступный
*/

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QGroupBox>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPalette>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <QSharedPointer>
#include <QStackedWidget>
#include <QTabBar>
#include <QTableWidget>
#include <QTemporaryFile>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <initializer_list>
#include <unistd.h>

enum class Source {
    Synapse,
    Flathub,
    Debian
};

enum class Category {
    All,
    Internet,
    Games,
    Development,
    Office,
    Multimedia,
    System,
    Other
};

struct Package {
    Source source;
    QString id;
    QString name;
    QString version;
    QString description;
    bool installed;
    bool installable;
};

// Название источника показывается возле каждого пакета
static QString sourceName(Source source) {
    switch (source) {
        case Source::Synapse:
            return QStringLiteral("SynapseOS");
        case Source::Flathub:
            return QStringLiteral("Flathub");
        case Source::Debian:
            return QStringLiteral("Debian");
    }

    return QString();
}

// Разнести программы по подборкам, когда источник не передаёт категории
static Category packageCategory(const Package &package) {
    const QString text = (package.id + QLatin1Char(' ') + package.name + QLatin1Char(' ') +
                          package.description).toLower();
    const auto containsAny = [&text](std::initializer_list<const char *> words) {
        for (const char *word : words) {
            if (text.contains(QLatin1String(word))) {
                return true;
            }
        }

        return false;
    };

    if (containsAny({"mindustry", "steam", "minecraft", "lutris", "heroic", "game"})) {
        return Category::Games;
    }

    if (containsAny({"firefox", "browser", "telegram", "discord", "mail", "network", "caddy",
                     "curl", "wget", "web", "ssh", "ftp"})) {
        return Category::Internet;
    }

    if (containsAny({"video", "audio", "media", "music", "vlc", "player", "photo", "image"})) {
        return Category::Multimedia;
    }

    if (containsAny({"libreoffice", "office", "document", "pdf", "spreadsheet"})) {
        return Category::Office;
    }

    if (containsAny({"compiler", "clang", "gcc", "python", "qt6", "rust", "cargo", "git",
                     "developer", "editor", "vscode"})) {
        return Category::Development;
    }

    if (containsAny({"settings", "terminal", "monitor", "package", "flatpak", "setup",
                     "system", "service", "files", "htop", "synapse-store"})) {
        return Category::System;
    }

    return Category::Other;
}

// Пропускать в команды менеджеров только допустимые имена пакетов
static bool validPackageId(const QString &id) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._+-]{0,127}$"));
    return pattern.match(id).hasMatch();
}

// Стереть пароль после передачи в локальную службу
static void clearSecret(QByteArray &secret) {
    volatile char *bytes = secret.data();
    for (qsizetype index = 0; index < secret.size(); ++index) {
        bytes[index] = 0;
    }

    secret.clear();
}

// Согласовать основные цвета магазина с выбранной темой рабочего стола
static void applyDesktopTheme() {
    QString path = QDir::homePath() + QStringLiteral("/.config/labwc/rc.xml");
    if (!QFileInfo::exists(path)) {
        path = QStringLiteral("/etc/xdg/labwc/rc.xml");
    }

    QFile file(path);
    const QString config = file.open(QIODevice::ReadOnly | QIODevice::Text)
                               ? QString::fromUtf8(file.readAll())
                               : QString();
    const bool light = config.contains(QStringLiteral("<name>Arc-Clone</name>"));
    const bool penumbra = config.contains(QStringLiteral("<name>Penumbra</name>"));
    const bool numix = config.contains(QStringLiteral("<name>Numix-Clone</name>"));
    const QColor background(light ? QStringLiteral("#e7e9ed") :
                            penumbra ? QStringLiteral("#243842") :
                            numix ? QStringLiteral("#303238") : QStringLiteral("#1b2027"));
    const QColor base(light ? QStringLiteral("#ffffff") :
                      penumbra ? QStringLiteral("#1b2e37") :
                      numix ? QStringLiteral("#26282d") : QStringLiteral("#141920"));
    const QColor surface(light ? QStringLiteral("#d9e0e7") :
                         penumbra ? QStringLiteral("#304b57") :
                         numix ? QStringLiteral("#41434a") : QStringLiteral("#29333d"));
    const QColor foreground(light ? QStringLiteral("#202a34") : QStringLiteral("#f3f4f5"));
    const QColor accent(numix ? QStringLiteral("#d65d3a") :
                        penumbra ? QStringLiteral("#769fb4") : QStringLiteral("#4388c5"));

    QPalette palette;
    palette.setColor(QPalette::Window, background);
    palette.setColor(QPalette::WindowText, foreground);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, surface);
    palette.setColor(QPalette::Text, foreground);
    palette.setColor(QPalette::Button, surface);
    palette.setColor(QPalette::ButtonText, foreground);
    palette.setColor(QPalette::Highlight, accent);
    palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
    QApplication::setPalette(palette);
}

class StoreWindow final : public QMainWindow {
  public:
    // Создать окно и начать чтение доступных каталогов
    StoreWindow() {
        setWindowTitle(QStringLiteral("Программы SynapseOS"));
        resize(980, 640);

        auto *central = new QWidget(this);
        auto *layout = new QVBoxLayout(central);
        auto *header = new QLabel(QStringLiteral("Программы"), central);
        QFont titleFont = header->font();
        titleFont.setPointSize(titleFont.pointSize() + 7);
        titleFont.setBold(true);
        header->setFont(titleFont);
        layout->addWidget(header);

        auto *description = new QLabel(
            QStringLiteral("Пакеты SynapseOS, приложения Flathub и проверенные программы Debian"), central);
        layout->addWidget(description);

        tabs_ = new QTabBar(central);
        tabs_->setExpanding(false);
        tabs_->addTab(QStringLiteral("Все"));
        tabs_->addTab(QStringLiteral("SynapseOS"));
        tabs_->addTab(QStringLiteral("Flathub"));
        tabs_->addTab(QStringLiteral("Debian"));
        tabs_->addTab(QStringLiteral("Установленные"));
        tabs_->addTab(QStringLiteral("Настройки"));
        layout->addWidget(tabs_);

        pages_ = new QStackedWidget(central);
        auto *catalogPage = new QWidget(pages_);
        auto *catalogLayout = new QVBoxLayout(catalogPage);
        pages_->addWidget(catalogPage);

        auto *controls = new QHBoxLayout();
        search_ = new QLineEdit(catalogPage);
        search_->setPlaceholderText(QStringLiteral("Поиск по названию и описанию"));
        controls->addWidget(search_, 1);

        category_ = new QComboBox(catalogPage);
        category_->addItem(QStringLiteral("Все категории"), static_cast<int>(Category::All));
        category_->addItem(QStringLiteral("Интернет"), static_cast<int>(Category::Internet));
        category_->addItem(QStringLiteral("Игры"), static_cast<int>(Category::Games));
        category_->addItem(QStringLiteral("Разработка"), static_cast<int>(Category::Development));
        category_->addItem(QStringLiteral("Офис"), static_cast<int>(Category::Office));
        category_->addItem(QStringLiteral("Мультимедиа"), static_cast<int>(Category::Multimedia));
        category_->addItem(QStringLiteral("Система"), static_cast<int>(Category::System));
        category_->addItem(QStringLiteral("Другое"), static_cast<int>(Category::Other));
        controls->addWidget(category_);

        refresh_ = new QPushButton(QStringLiteral("Перечитать списки"), catalogPage);
        controls->addWidget(refresh_);
        catalogLayout->addLayout(controls);

        table_ = new QTableWidget(catalogPage);
        table_->setColumnCount(5);
        table_->setHorizontalHeaderLabels(
            {QStringLiteral("Программа"), QStringLiteral("Источник"), QStringLiteral("Версия"),
             QStringLiteral("Состояние"), QStringLiteral("Описание")});
        table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        catalogLayout->addWidget(table_, 1);

        auto *actions = new QHBoxLayout();
        status_ = new QLabel(catalogPage);
        actions->addWidget(status_, 1);
        install_ = new QPushButton(QStringLiteral("Установить"), catalogPage);
        install_->setEnabled(false);
        actions->addWidget(install_);
        catalogLayout->addLayout(actions);

        log_ = new QPlainTextEdit(catalogPage);
        log_->setReadOnly(true);
        log_->document()->setMaximumBlockCount(500);
        log_->setMaximumHeight(120);
        catalogLayout->addWidget(log_);

        auto *settingsPage = new QWidget(pages_);
        auto *settingsLayout = new QVBoxLayout(settingsPage);
        auto *sources = new QGroupBox(QStringLiteral("Источники программ"), settingsPage);
        auto *sourceLayout = new QVBoxLayout(sources);
        showSynapse_ = new QCheckBox(QStringLiteral("Показывать пакеты SynapseOS"), sources);
        showFlathub_ = new QCheckBox(QStringLiteral("Показывать приложения Flathub"), sources);
        showDebian_ = new QCheckBox(QStringLiteral("Показывать проверенные пакеты Debian"), sources);
        showSynapse_->setChecked(settings_.value(QStringLiteral("sources/synapse"), true).toBool());
        showFlathub_->setChecked(settings_.value(QStringLiteral("sources/flathub"), true).toBool());
        showDebian_->setChecked(settings_.value(QStringLiteral("sources/debian"), true).toBool());
        sourceLayout->addWidget(showSynapse_);
        sourceLayout->addWidget(showFlathub_);
        sourceLayout->addWidget(showDebian_);
        settingsLayout->addWidget(sources);

        auto *settingsNote = new QLabel(
            QStringLiteral("Категории определяются по названию и описанию программы. "
                           "Debian показывает только пакеты, проверенные для SynapseOS. "
                           "Выключение источника скрывает его каталог в этом окне."), settingsPage);
        settingsNote->setWordWrap(true);
        settingsLayout->addWidget(settingsNote);

        backendStatus_ = new QLabel(settingsPage);
        backendStatus_->setWordWrap(true);
        settingsLayout->addWidget(backendStatus_);

        updateSynapse_ = new QPushButton(QStringLiteral("Обновить индекс SynapseOS"), settingsPage);
        updateSynapse_->setEnabled(QFileInfo(QStringLiteral("/bin/setup")).isExecutable());
        settingsLayout->addWidget(updateSynapse_);

        settingsStatus_ = new QLabel(
            geteuid() == 0 ? QStringLiteral("Индекс setup обновляется отдельно от перечитывания списков")
                            : QStringLiteral("Для обновления индекса setup потребуется пароль администратора"),
            settingsPage);
        settingsStatus_->setWordWrap(true);
        settingsLayout->addWidget(settingsStatus_);
        settingsLayout->addStretch();
        pages_->addWidget(settingsPage);
        layout->addWidget(pages_, 1);

        setCentralWidget(central);

        debianSearchTimer_ = new QTimer(this);
        debianSearchTimer_->setSingleShot(true);
        debianSearchTimer_->setInterval(250);
        connect(debianSearchTimer_, &QTimer::timeout, this, [this]() { searchDebian(); });

        connect(refresh_, &QPushButton::clicked, this, [this]() { refreshCatalogs(); });
        connect(updateSynapse_, &QPushButton::clicked, this, [this]() { updateSynapseIndex(); });
        connect(category_, &QComboBox::currentIndexChanged, this, [this]() { renderCatalog(); });
        connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
            pages_->setCurrentIndex(index == 5 ? 1 : 0);
            renderCatalog();
            if ((index == 0 || index == 3) && showDebian_->isChecked()) {
                debianSearchTimer_->start();
            }
        });
        const auto connectSource = [this](QCheckBox *check, Source source, const QString &key) {
            connect(check, &QCheckBox::toggled, this, [this, source, key](bool enabled) {
                settings_.setValue(key, enabled);
                if (!enabled && tabs_->currentIndex() == static_cast<int>(source) + 1) {
                    tabs_->setCurrentIndex(0);
                }

                tabs_->setTabEnabled(static_cast<int>(source) + 1, enabled);
                refreshCatalogs();
            });
            tabs_->setTabEnabled(static_cast<int>(source) + 1, check->isChecked());
        };
        connectSource(showSynapse_, Source::Synapse, QStringLiteral("sources/synapse"));
        connectSource(showFlathub_, Source::Flathub, QStringLiteral("sources/flathub"));
        connectSource(showDebian_, Source::Debian, QStringLiteral("sources/debian"));
        connect(search_, &QLineEdit::textChanged, this, [this]() {
            ++debianSearchGeneration_;
            renderCatalog();
            if (showDebian_->isChecked() && (tabs_->currentIndex() == 0 || tabs_->currentIndex() == 3)) {
                debianSearchTimer_->start();
            }
        });
        connect(table_, &QTableWidget::itemSelectionChanged, this, [this]() { updateAction(); });
        connect(install_, &QPushButton::clicked, this, [this]() { installSelected(); });

        refreshCatalogs();
    }

  private:
    // Обновить доступные источники без блокировки графического окна
    void refreshCatalogs() {
        if (installing_) {
            return;
        }

        ++catalogGeneration_;
        ++debianSearchGeneration_;
        debianSearchTimer_->stop();
        packages_.clear();
        flatpakInstalled_.clear();
        renderCatalog();
        status_->setText(QStringLiteral("Чтение каталогов…"));

        if (showSynapse_->isChecked() && QFileInfo(QStringLiteral("/bin/setup")).isExecutable()) {
            readCatalog(Source::Synapse, QStringLiteral("/bin/setup"), {QStringLiteral("ls")});
        } else if (showSynapse_->isChecked()) {
            log_->appendPlainText(QStringLiteral("Пакет setup не установлен"));
        }

        if (showFlathub_->isChecked() && QFileInfo(QStringLiteral("/bin/flatpak")).isExecutable()) {
            readInstalledFlatpaks();
            connectFlathub();
        } else if (showFlathub_->isChecked()) {
            log_->appendPlainText(QStringLiteral("Flatpak можно установить через setup"));
        }

        if (showDebian_->isChecked() && QFileInfo(QStringLiteral("/bin/debian-apt")).isExecutable()) {
            searchDebian();
        } else if (showDebian_->isChecked()) {
            log_->appendPlainText(QStringLiteral("Debian: установите пакет apt через setup"));
        }

        backendStatus_->setText(
            QFileInfo(QStringLiteral("/bin/debian-apt")).isExecutable()
                ? QStringLiteral("Debian: backend установлен, доступны только проверенные пакеты")
                : QStringLiteral("Debian: backend не установлен. Команда: setup install apt"));
    }

    // Явно обновить сетевой индекс setup по запросу администратора
    void updateSynapseIndex() {
        if (!QFileInfo(QStringLiteral("/bin/setup")).isExecutable()) {
            return;
        }

        const bool privileged = geteuid() == 0;
        if (!privileged && !QFileInfo(QStringLiteral("/var/run/synapse-store.sock")).exists()) {
            settingsStatus_->setText(QStringLiteral("Служба установки SynapseOS недоступна"));
            return;
        }

        auto secret = QSharedPointer<QByteArray>::create();
        if (!privileged) {
            bool accepted = false;
            QString value = QInputDialog::getText(this, QStringLiteral("Обновление каталога"),
                                                  QStringLiteral("Пароль администратора"),
                                                  QLineEdit::Password, QString(), &accepted);
            if (!accepted || value.isEmpty()) {
                value.fill(QChar(0));
                return;
            }

            *secret = value.toUtf8();
            value.fill(QChar(0));
        }

        updateSynapse_->setEnabled(false);
        settingsStatus_->setText(QStringLiteral("Обновление индекса SynapseOS…"));
        auto *process = new QProcess(this);
        process->setProgram(privileged ? QStringLiteral("/bin/setup")
                                       : QStringLiteral("/bin/synapse-store-control"));
        process->setArguments({privileged ? QStringLiteral("update") : QStringLiteral("--update")});
        process->setProcessChannelMode(QProcess::MergedChannels);
        if (!privileged) {
            connect(process, &QProcess::started, this, [process, secret]() {
                process->write(*secret);
                process->write("\n", 1);
                process->closeWriteChannel();
                clearSecret(*secret);
            });
        }

        connect(process, &QProcess::finished, this,
                [this, process, secret](int code, QProcess::ExitStatus exitStatus) {
                    clearSecret(*secret);
                    const bool success = exitStatus == QProcess::NormalExit && code == 0;
                    settingsStatus_->setText(success ? QStringLiteral("Индекс SynapseOS обновлён")
                                                     : QStringLiteral("Не удалось обновить индекс SynapseOS"));
                    const QString output = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
                    if (!output.isEmpty()) {
                        log_->appendPlainText(output);
                    }

                    updateSynapse_->setEnabled(true);
                    process->deleteLater();
                    if (success) {
                        refreshCatalogs();
                    }
                });
        connect(process, &QProcess::errorOccurred, this,
                [this, process, secret](QProcess::ProcessError error) {
                    if (error != QProcess::FailedToStart) {
                        return;
                    }

                    clearSecret(*secret);
                    settingsStatus_->setText(QStringLiteral("Не удалось запустить setup update"));
                    updateSynapse_->setEnabled(true);
                    process->deleteLater();
                });
        process->start();
    }

    // Подключить Flathub в выбранном профиле перед чтением каталога
    void connectFlathub() {
        auto *process = new QProcess(this);
        const unsigned generation = catalogGeneration_;
        const QString scope = geteuid() == 0 ? QStringLiteral("--system") : QStringLiteral("--user");
        process->setProgram(QStringLiteral("/bin/flatpak"));
        process->setArguments({QStringLiteral("remote-add"), scope, QStringLiteral("--if-not-exists"),
                               QStringLiteral("--from"), QStringLiteral("flathub"),
                               QStringLiteral("https://dl.flathub.org/repo/flathub.flatpakrepo")});

        connect(process, &QProcess::finished, this,
                [this, process, generation, scope](int code, QProcess::ExitStatus exitStatus) {
                    if (generation != catalogGeneration_) {
                        process->deleteLater();
                        return;
                    }

                    if (exitStatus == QProcess::NormalExit && code == 0) {
                        readCatalog(Source::Flathub, QStringLiteral("/bin/flatpak"),
                                    {QStringLiteral("remote-ls"), scope, QStringLiteral("--app"),
                                     QStringLiteral("--columns=application:f,name:f,branch:f"),
                                     QStringLiteral("flathub")});
                    } else {
                        log_->appendPlainText(QStringLiteral("Flathub: ") +
                                              QString::fromUtf8(process->readAllStandardError()).trimmed());
                    }

                    process->deleteLater();
                });

        process->start();
    }

    // Найти Debian-пакеты через изолированный каталог APT
    void searchDebian() {
        for (int index = packages_.size() - 1; index >= 0; --index) {
            if (packages_[index].source == Source::Debian) {
                packages_.removeAt(index);
            }
        }

        renderCatalog();
        const QString query = search_->text().trimmed();
        if (!QFileInfo(QStringLiteral("/bin/debian-apt")).isExecutable()) {
            return;
        }

        const unsigned generation = debianSearchGeneration_;
        auto *process = new QProcess(this);
        QTemporaryFile outputFile(QDir::tempPath() + QStringLiteral("/synapse-debian-XXXXXX"));
        outputFile.setAutoRemove(false);
        if (!outputFile.open()) {
            log_->appendPlainText(QStringLiteral("Debian: не удалось создать временный файл"));
            process->deleteLater();
            return;
        }

        const QString outputPath = outputFile.fileName();
        outputFile.close();
        process->setProgram(QStringLiteral("/bin/debian-apt"));
        QStringList arguments = {QStringLiteral("catalog")};
        if (!query.isEmpty()) {
            arguments.append(query);
        }

        process->setArguments(arguments);
        process->setProcessChannelMode(QProcess::SeparateChannels);
        process->setStandardOutputFile(outputPath, QIODevice::Truncate);

        connect(process, &QProcess::finished, this,
                [this, process, outputPath, generation](int code, QProcess::ExitStatus exitStatus) {
                    QFile outputFile(outputPath);
                    QByteArray output;
                    if (outputFile.open(QIODevice::ReadOnly)) {
                        output = outputFile.read(1024 * 1024);
                        outputFile.close();
                    }

                    outputFile.remove();
                    if (generation == debianSearchGeneration_) {
                        if (exitStatus == QProcess::NormalExit && code == 0) {
                            int found = 0;
                            for (const QByteArray &line : output.split('\n')) {
                                QJsonParseError error;
                                const QJsonDocument document = QJsonDocument::fromJson(line, &error);
                                if (error.error != QJsonParseError::NoError || !document.isObject()) {
                                    continue;
                                }

                                const QJsonObject item = document.object();
                                const QString id = item.value(QStringLiteral("id")).toString();
                                if (item.value(QStringLiteral("schema")).toInt() != 1 ||
                                    item.value(QStringLiteral("kind")).toString() != QStringLiteral("package") ||
                                    !validPackageId(id)) {
                                    continue;
                                }

                                packages_.append({Source::Debian, id, id,
                                                  item.value(QStringLiteral("version")).toString(),
                                                  item.value(QStringLiteral("description")).toString(),
                                                  item.value(QStringLiteral("installed")).toBool(),
                                                  item.value(QStringLiteral("installable")).toBool()});
                                if (++found == 100) {
                                    break;
                                }
                            }

                            if (found == 0 && !output.isEmpty()) {
                                log_->appendPlainText(QStringLiteral("Debian: ответ каталога не распознан"));
                            }

                            renderCatalog();
                        } else {
                            const QString error = QString::fromUtf8(process->readAllStandardError()).trimmed();
                            log_->appendPlainText(QStringLiteral("Debian: ") +
                                                  (error.isEmpty() ? process->errorString() : error));
                        }
                    }

                    process->deleteLater();
                });

        process->start();
    }

    // Прочитать установленные Flatpak до показа действия установки
    void readInstalledFlatpaks() {
        auto *process = new QProcess(this);
        const unsigned generation = catalogGeneration_;
        process->setProgram(QStringLiteral("/bin/flatpak"));
        process->setArguments({QStringLiteral("list"), QStringLiteral("--app"),
                               QStringLiteral("--columns=application:f")});

        connect(process, &QProcess::finished, this,
                [this, process, generation](int code, QProcess::ExitStatus exitStatus) {
                    if (generation == catalogGeneration_ && exitStatus == QProcess::NormalExit && code == 0) {
                        const QString output = QString::fromUtf8(process->readAllStandardOutput());
                        for (const QString &line : output.split(QLatin1Char('\n'))) {
                            const QString id = line.trimmed();
                            if (!id.isEmpty()) {
                                flatpakInstalled_.insert(id);
                            }
                        }

                        for (Package &package : packages_) {
                            if (package.source == Source::Flathub) {
                                package.installed = flatpakInstalled_.contains(package.id);
                            }
                        }

                        renderCatalog();
                    } else if (generation == catalogGeneration_) {
                        log_->appendPlainText(QStringLiteral("Flatpak: не удалось прочитать установленные программы"));
                    }

                    process->deleteLater();
                });

        process->start();
    }

    // Прочитать один каталог через программу без shell и произвольных аргументов пользователя
    void readCatalog(Source source, const QString &program, const QStringList &arguments) {
        auto *process = new QProcess(this);
        QTemporaryFile outputFile(QDir::tempPath() + QStringLiteral("/synapse-catalog-XXXXXX"));
        outputFile.setAutoRemove(false);
        if (!outputFile.open()) {
            log_->appendPlainText(sourceName(source) + QStringLiteral(": не удалось создать временный файл"));
            process->deleteLater();
            return;
        }

        const QString outputPath = outputFile.fileName();
        outputFile.close();
        const unsigned generation = catalogGeneration_;
        process->setProgram(program);
        process->setArguments(arguments);
        process->setProcessChannelMode(QProcess::SeparateChannels);
        process->setStandardOutputFile(outputPath, QIODevice::Truncate);

        connect(process, &QProcess::finished, this,
                [this, process, source, outputPath, generation](int code, QProcess::ExitStatus exitStatus) {
                    QFile outputFile(outputPath);
                    QByteArray output;
                    if (outputFile.open(QIODevice::ReadOnly)) {
                        output = outputFile.read(8 * 1024 * 1024);
                        outputFile.close();
                    }

                    outputFile.remove();
                    const QString error = QString::fromUtf8(process->readAllStandardError()).trimmed();

                    if (generation != catalogGeneration_) {
                        process->deleteLater();
                        return;
                    }

                    if (exitStatus == QProcess::NormalExit && code == 0) {
                        parseCatalog(source, QString::fromUtf8(output));
                        renderCatalog();
                    } else {
                        log_->appendPlainText(sourceName(source) + QStringLiteral(": ") +
                                              (error.isEmpty() ? process->errorString() : error));
                    }

                    process->deleteLater();
                });

        connect(process, &QProcess::errorOccurred, this,
                [this, process, source, outputPath](QProcess::ProcessError error) {
                    if (error != QProcess::FailedToStart) {
                        return;
                    }

                    QFile(outputPath).remove();
                    log_->appendPlainText(sourceName(source) + QStringLiteral(": ") + process->errorString());
                    process->deleteLater();
                });

        process->start();
    }

    // Разобрать строки setup и табличный вывод Flatpak
    void parseCatalog(Source source, const QString &output) {
        const QRegularExpression setupLine(
            QStringLiteral("^(\\S+)\\s+(\\S+)\\s+\\[([^\\]]+)\\]\\s*(.*)$"));

        for (const QString &line : output.split(QLatin1Char('\n'))) {
            if (source == Source::Synapse) {
                const QRegularExpressionMatch match = setupLine.match(line.trimmed());
                if (!match.hasMatch()) {
                    continue;
                }

                if (!validPackageId(match.captured(1))) {
                    continue;
                }

                QString description = match.captured(4);
                const bool installed = description.contains(QStringLiteral("(installed)"));
                description.replace(QStringLiteral("(installed)"), QString());
                packages_.append({source, match.captured(1), match.captured(1), match.captured(2),
                                  description.trimmed(), installed, true});
                continue;
            }

            if (source == Source::Flathub) {
                const QStringList columns = line.split(QLatin1Char('\t'));
                if (columns.size() < 3 || !validPackageId(columns[0])) {
                    continue;
                }

                packages_.append({source, columns[0], columns[1], columns[2], QString(),
                                  flatpakInstalled_.contains(columns[0]), true});
            }
        }
    }

    // Показать пакеты, соответствующие запросу пользователя
    void renderCatalog() {
        visible_.clear();
        table_->setUpdatesEnabled(false);
        table_->setRowCount(0);
        const QString query = search_->text().trimmed();
        const int section = tabs_->currentIndex();
        const Category category = static_cast<Category>(category_->currentData().toInt());
        int matches = 0;

        for (int index = 0; index < packages_.size(); ++index) {
            const Package &package = packages_[index];
            if (!sourceEnabled(package.source) ||
                (section >= 1 && section <= 3 && static_cast<int>(package.source) + 1 != section) ||
                (section == 4 && !package.installed) ||
                (category != Category::All && packageCategory(package) != category)) {
                continue;
            }

            if (!query.isEmpty() && !package.name.contains(query, Qt::CaseInsensitive) &&
                !package.description.contains(query, Qt::CaseInsensitive) &&
                !package.id.contains(query, Qt::CaseInsensitive)) {
                continue;
            }

            ++matches;
            if (matches > 200) {
                continue;
            }

            const int row = table_->rowCount();
            table_->insertRow(row);
            table_->setItem(row, 0, new QTableWidgetItem(package.name));
            table_->setItem(row, 1, new QTableWidgetItem(sourceName(package.source)));
            table_->setItem(row, 2, new QTableWidgetItem(package.version));
            const QString state = package.installed ? QStringLiteral("Установлено")
                                  : package.installable ? QStringLiteral("Доступно")
                                                        : QStringLiteral("Недоступно");
            table_->setItem(row, 3, new QTableWidgetItem(state));
            table_->setItem(row, 4, new QTableWidgetItem(package.description));
            visible_.append(index);
        }

        table_->setUpdatesEnabled(true);
        if (!installing_) {
            status_->setText(QStringLiteral("Показано %1 из %2 программ").arg(visible_.size()).arg(matches));
        }

        updateAction();
    }

    // Учесть выбранные пользователем источники во всех разделах
    bool sourceEnabled(Source source) const {
        switch (source) {
            case Source::Synapse:
                return showSynapse_->isChecked();
            case Source::Flathub:
                return showFlathub_->isChecked();
            case Source::Debian:
                return showDebian_->isChecked();
        }

        return false;
    }

    // Разрешить установку только для доступного backend
    void updateAction() {
        const int row = table_->currentRow();
        const bool available = !installing_ && row >= 0 && row < visible_.size() &&
                               packages_[visible_[row]].installable &&
                               !packages_[visible_[row]].installed;
        install_->setEnabled(available);
    }

    // Установить выбранный пакет через его собственный менеджер
    void installSelected() {
        const int row = table_->currentRow();
        if (row < 0 || row >= visible_.size()) {
            return;
        }

        const Package package = packages_[visible_[row]];
        if ((package.source == Source::Synapse || package.source == Source::Debian) && geteuid() != 0 &&
            !QFileInfo(QStringLiteral("/var/run/synapse-store.sock")).exists()) {
            QMessageBox::information(this, QStringLiteral("Нужны права администратора"),
                                     QStringLiteral("Служба установки системных пакетов недоступна"));
            return;
        }

        if (QMessageBox::question(this, QStringLiteral("Установка"),
                                  QStringLiteral("Установить %1 из %2?").arg(package.name,
                                                                            sourceName(package.source))) !=
            QMessageBox::Yes) {
            return;
        }

        installing_ = true;
        install_->setEnabled(false);
        refresh_->setEnabled(false);
        status_->setText(QStringLiteral("Установка %1…").arg(package.name));

        if (package.source == Source::Flathub && geteuid() != 0) {
            prepareUserFlatpakRemote(package);
            return;
        }

        QByteArray password;
        if ((package.source == Source::Synapse || package.source == Source::Debian) && geteuid() != 0) {
            bool accepted = false;
            QString value = QInputDialog::getText(this, QStringLiteral("Подтверждение установки"),
                                                  QStringLiteral("Пароль администратора"),
                                                  QLineEdit::Password, QString(), &accepted);
            if (!accepted || value.isEmpty()) {
                clearSecret(password);
                installing_ = false;
                refresh_->setEnabled(true);
                updateAction();
                return;
            }

            password = value.toUtf8();
            value.fill(QChar(0));
            value.clear();
        }

        runInstall(package, password);
        clearSecret(password);
    }

    // Добавить Flathub в профиль пользователя перед установкой
    void prepareUserFlatpakRemote(const Package &package) {
        auto *process = new QProcess(this);
        process->setProgram(QStringLiteral("/bin/flatpak"));
        process->setArguments({QStringLiteral("remote-add"), QStringLiteral("--user"),
                               QStringLiteral("--if-not-exists"), QStringLiteral("--from"),
                               QStringLiteral("flathub"),
                               QStringLiteral("https://dl.flathub.org/repo/flathub.flatpakrepo")});

        connect(process, &QProcess::finished, this,
                [this, process, package](int code, QProcess::ExitStatus exitStatus) {
                    const bool success = exitStatus == QProcess::NormalExit && code == 0;
                    const QString error = QString::fromUtf8(process->readAllStandardError()).trimmed();
                    process->deleteLater();
                    if (success) {
                        runInstall(package);
                        return;
                    }

                    log_->appendPlainText(QStringLiteral("Flathub: ") + error);
                    status_->setText(QStringLiteral("Не удалось подключить Flathub"));
                    installing_ = false;
                    refresh_->setEnabled(true);
                    updateAction();
                });

        connect(process, &QProcess::errorOccurred, this,
                [this, process](QProcess::ProcessError error) {
                    if (error != QProcess::FailedToStart) {
                        return;
                    }

                    log_->appendPlainText(QStringLiteral("Flathub: ") + process->errorString());
                    status_->setText(QStringLiteral("Не удалось подключить Flathub"));
                    installing_ = false;
                    refresh_->setEnabled(true);
                    updateAction();
                    process->deleteLater();
                });

        process->start();
    }

    // Запустить менеджер пакетов после подготовки нужного источника
    void runInstall(const Package &package, const QByteArray &password = QByteArray()) {
        QString program;
        QStringList arguments;
        if (package.source == Source::Synapse) {
            if (geteuid() == 0) {
                program = QStringLiteral("/bin/setup");
                arguments = {QStringLiteral("install"), package.id, QStringLiteral("--yes")};
            } else {
                program = QStringLiteral("/bin/synapse-store-control");
                arguments = {package.id};
            }
        } else if (package.source == Source::Flathub) {
            program = QStringLiteral("/bin/flatpak");
            arguments = {QStringLiteral("install")};
            if (geteuid() != 0) {
                arguments.append(QStringLiteral("--user"));
            }

            arguments.append({QStringLiteral("-y"), QStringLiteral("flathub"), package.id});
        } else {
            if (geteuid() == 0) {
                program = QStringLiteral("/bin/debian-apt");
                arguments = {QStringLiteral("install"), package.id};
            } else {
                program = QStringLiteral("/bin/synapse-store-control");
                arguments = {QStringLiteral("--debian"), package.id};
            }
        }

        auto *process = new QProcess(this);
        QTemporaryFile outputFile(QDir::tempPath() + QStringLiteral("/synapse-install-XXXXXX"));
        outputFile.setAutoRemove(false);
        if (!outputFile.open()) {
            log_->appendPlainText(QStringLiteral("Не удалось создать журнал установки"));
            status_->setText(QStringLiteral("Не удалось установить %1").arg(package.name));
            installing_ = false;
            refresh_->setEnabled(true);
            updateAction();
            process->deleteLater();
            return;
        }

        const QString outputPath = outputFile.fileName();
        outputFile.close();
        auto secret = QSharedPointer<QByteArray>::create(password);
        process->setProgram(program);
        process->setArguments(arguments);
        process->setProcessChannelMode(QProcess::MergedChannels);
        process->setStandardOutputFile(outputPath, QIODevice::Truncate);

        if (!secret->isEmpty()) {
            connect(process, &QProcess::started, this, [process, secret]() {
                process->write(*secret);
                process->write("\n", 1);
                process->closeWriteChannel();
                clearSecret(*secret);
            });
        }

        connect(process, &QProcess::finished, this,
                [this, process, package, outputPath](int code, QProcess::ExitStatus exitStatus) {
                    const bool success = exitStatus == QProcess::NormalExit && code == 0;
                    QFile outputFile(outputPath);
                    QByteArray outputBytes;
                    if (outputFile.open(QIODevice::ReadOnly)) {
                        outputBytes = outputFile.read(8 * 1024 * 1024);
                        outputFile.close();
                    }

                    outputFile.remove();
                    const QString output = QString::fromUtf8(outputBytes).trimmed();
                    if (!output.isEmpty()) {
                        log_->appendPlainText(output);
                    }

                    if (success) {
                        status_->setText(QStringLiteral("Установлено: %1").arg(package.name));
                    } else {
                        status_->setText(QStringLiteral("Не удалось установить %1").arg(package.name));
                        log_->appendPlainText(QStringLiteral("Код завершения: %1").arg(code));
                        if (exitStatus != QProcess::NormalExit) {
                            log_->appendPlainText(process->errorString());
                        }
                    }

                    installing_ = false;
                    refresh_->setEnabled(true);
                    process->deleteLater();
                    refreshCatalogs();
                });

        connect(process, &QProcess::errorOccurred, this,
                [this, process, package, secret, outputPath](QProcess::ProcessError error) {
                    if (error != QProcess::FailedToStart) {
                        return;
                    }

                    clearSecret(*secret);
                    QFile(outputPath).remove();
                    status_->setText(QStringLiteral("Не удалось установить %1").arg(package.name));
                    log_->appendPlainText(process->errorString());
                    installing_ = false;
                    refresh_->setEnabled(true);
                    updateAction();
                    process->deleteLater();
                });

        process->start();
    }

    QLineEdit *search_ = nullptr;
    QComboBox *category_ = nullptr;
    QTabBar *tabs_ = nullptr;
    QStackedWidget *pages_ = nullptr;
    QTableWidget *table_ = nullptr;
    QLabel *status_ = nullptr;
    QLabel *backendStatus_ = nullptr;
    QLabel *settingsStatus_ = nullptr;
    QPushButton *refresh_ = nullptr;
    QPushButton *updateSynapse_ = nullptr;
    QPushButton *install_ = nullptr;
    QTimer *debianSearchTimer_ = nullptr;
    QPlainTextEdit *log_ = nullptr;
    QCheckBox *showSynapse_ = nullptr;
    QCheckBox *showFlathub_ = nullptr;
    QCheckBox *showDebian_ = nullptr;
    QSettings settings_{QStringLiteral("SynapseOS"), QStringLiteral("Software")};
    QVector<Package> packages_;
    QVector<int> visible_;
    QSet<QString> flatpakInstalled_;
    unsigned catalogGeneration_ = 0;
    unsigned debianSearchGeneration_ = 0;
    bool installing_ = false;
};

// Запустить отдельное окно магазина программ
int main(int argc, char **argv) {
    QApplication application(argc, argv);
    applyDesktopTheme();
    StoreWindow window;
    window.show();
    return application.exec();
}
