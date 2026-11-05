/*
    main.cpp: Отдельный графический магазин программ SynapseOS
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI: synapse-x86_64-v1

    Описание: Каталог программ SynapseOS и Flatpak с явным состоянием Debian
    Подсистема: графические программы
    Первое изменение: 01.11.2026
    Последнее изменение: 03.11.2026
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
#include <QHash>
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
#include <QStringDecoder>
#include <QTabBar>
#include <QTableWidget>
#include <QTemporaryFile>
#include <QTextCursor>
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
    QString ref = {};
    QString origin = {};
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
    QString description = package.description;
    // Шаблон сборки не описывает назначение программы
    if (description.startsWith(QStringLiteral("Built into SynapseOS image "))) {
        description.clear();
    }

    const QString text = (package.id + QLatin1Char(' ') + package.name + QLatin1Char(' ') +
                          description).toLower();
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

// Сохранить выбранную ветку Flatpak, не подменяя её веткой по умолчанию
static QString flatpakApplicationRef(const QString &id, const QString &branch, const QString &arch) {
    if (!validPackageId(id) || !validPackageId(branch) || arch != QStringLiteral("x86_64")) {
        return QString();
    }

    return QStringLiteral("app/%1/%2/%3").arg(id, arch, branch);
}

// Сравнить числовые версии setup, не предлагая обновление для неизвестного формата
static bool newerSynapseVersion(const QString &available, const QString &installed) {
    static const QRegularExpression pattern(QStringLiteral("^([0-9]+)\\.([0-9]+)\\.([0-9]+)-r([0-9]+)$"));
    const auto candidate = pattern.match(available);
    const auto current = pattern.match(installed);
    if (!candidate.hasMatch() || !current.hasMatch()) {
        return false;
    }

    quint32 candidateValues[4] = {};
    quint32 currentValues[4] = {};
    for (int field = 0; field < 4; ++field) {
        bool candidateOk = false;
        bool currentOk = false;
        candidateValues[field] = candidate.captured(field + 1).toUInt(&candidateOk);
        currentValues[field] = current.captured(field + 1).toUInt(&currentOk);
        if (!candidateOk || !currentOk) {
            return false;
        }
    }

    for (int field = 0; field < 4; ++field) {
        if (candidateValues[field] != currentValues[field]) {
            return candidateValues[field] > currentValues[field];
        }
    }

    return false;
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

        prepareDebian_ = new QPushButton(QStringLiteral("Подготовить каталог Debian"), settingsPage);
        prepareDebian_->setEnabled(!catalogUpdating_ &&
                                  QFileInfo(QStringLiteral("/bin/debian-apt")).isExecutable());
        settingsLayout->addWidget(prepareDebian_);

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
        connect(updateSynapse_, &QPushButton::clicked, this, [this]() { updateSystemCatalog(false); });
        connect(prepareDebian_, &QPushButton::clicked, this, [this]() { updateSystemCatalog(true); });
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
        flatpakInstalledKnown_ = false;
        synapseAvailableVersions_.clear();
        synapseInstalledKnown_ = false;
        renderCatalog();
        status_->setText(QStringLiteral("Чтение каталогов…"));

        if (showSynapse_->isChecked() && QFileInfo(QStringLiteral("/bin/setup")).isExecutable()) {
            readCatalog(Source::Synapse, QStringLiteral("/bin/setup"),
                        {QStringLiteral("ls"), QStringLiteral("-i")});
        } else if (showSynapse_->isChecked()) {
            log_->appendPlainText(QStringLiteral("Пакет setup не установлен"));
        }

        if (showFlathub_->isChecked() && QFileInfo(QStringLiteral("/bin/flatpak")).isExecutable()) {
            readInstalledFlatpaks();
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
        prepareDebian_->setEnabled(!catalogUpdating_ &&
                                  QFileInfo(QStringLiteral("/bin/debian-apt")).isExecutable());
    }

    // Подготовить системный каталог по запросу администратора
    void updateSystemCatalog(bool debian) {
        if (catalogUpdating_ || installing_) {
            return;
        }

        const QString backend = debian ? QStringLiteral("/bin/debian-apt") : QStringLiteral("/bin/setup");
        if (!QFileInfo(backend).isExecutable()) {
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

        catalogUpdating_ = true;
        updateSynapse_->setEnabled(false);
        prepareDebian_->setEnabled(false);
        settingsStatus_->setText(debian ? QStringLiteral("Подготовка каталога Debian…")
                                       : QStringLiteral("Обновление индекса SynapseOS…"));
        auto *process = new QProcess(this);
        process->setProgram(privileged ? backend : QStringLiteral("/bin/synapse-store-control"));
        const QString action = privileged
                                   ? (debian ? QStringLiteral("catalog") : QStringLiteral("update"))
                                   : (debian ? QStringLiteral("--debian-catalog") : QStringLiteral("--update"));
        process->setArguments({action});
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
                [this, process, secret, debian](int code, QProcess::ExitStatus exitStatus) {
                    clearSecret(*secret);
                    catalogUpdating_ = false;
                    const bool success = exitStatus == QProcess::NormalExit && code == 0;
                    settingsStatus_->setText(
                        success ? (debian ? QStringLiteral("Каталог Debian подготовлен")
                                          : QStringLiteral("Индекс SynapseOS обновлён"))
                                : (debian ? QStringLiteral("Не удалось подготовить каталог Debian")
                                          : QStringLiteral("Не удалось обновить индекс SynapseOS")));
                    const QString output = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
                    if (!output.isEmpty()) {
                        log_->appendPlainText(output);
                    }

                    updateSynapse_->setEnabled(true);
                    prepareDebian_->setEnabled(QFileInfo(QStringLiteral("/bin/debian-apt")).isExecutable());
                    process->deleteLater();
                    if (success) {
                        refreshCatalogs();
                    }
                });
        connect(process, &QProcess::errorOccurred, this,
                [this, process, secret, debian](QProcess::ProcessError error) {
                    if (error != QProcess::FailedToStart) {
                        return;
                    }

                    clearSecret(*secret);
                    catalogUpdating_ = false;
                    settingsStatus_->setText(debian ? QStringLiteral("Не удалось запустить каталог Debian")
                                                   : QStringLiteral("Не удалось запустить setup update"));
                    updateSynapse_->setEnabled(true);
                    prepareDebian_->setEnabled(QFileInfo(QStringLiteral("/bin/debian-apt")).isExecutable());
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
                                     QStringLiteral("--arch=x86_64"),
                                     QStringLiteral("--columns=application:f,name:f,version:f,branch:f,arch:f"),
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

        connect(process, &QProcess::errorOccurred, this,
                [this, process, outputPath, generation](QProcess::ProcessError error) {
                    if (error != QProcess::FailedToStart) {
                        return;
                    }

                    QFile(outputPath).remove();
                    if (generation == debianSearchGeneration_) {
                        log_->appendPlainText(QStringLiteral("Debian: ") + process->errorString());
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
                               geteuid() == 0 ? QStringLiteral("--system") : QStringLiteral("--user"),
                               QStringLiteral("--columns=application:f,name:f,version:f,branch:f,arch:f,origin:f")});

        connect(process, &QProcess::finished, this,
                [this, process, generation](int code, QProcess::ExitStatus exitStatus) {
                    if (generation == catalogGeneration_ && exitStatus == QProcess::NormalExit && code == 0) {
                        flatpakInstalledKnown_ = true;
                        const QString output = QString::fromUtf8(process->readAllStandardOutput());
                        for (const QString &line : output.split(QLatin1Char('\n'))) {
                            const QStringList columns = line.split(QLatin1Char('\t'));
                            if (columns.size() < 6) {
                                continue;
                            }

                            const QString ref = flatpakApplicationRef(columns[0], columns[3], columns[4]);
                            if (ref.isEmpty() || flatpakInstalled_.contains(ref)) {
                                continue;
                            }

                            flatpakInstalled_.insert(ref);
                            const QString version = columns[2].isEmpty()
                                                        ? QStringLiteral("ветка %1").arg(columns[3]) : columns[2];
                            packages_.append({Source::Flathub, columns[0],
                                              columns[1].isEmpty() ? columns[0] : columns[1], version,
                                              QString(), true, true, ref, columns[5]});
                        }

                        renderCatalog();
                    } else if (generation == catalogGeneration_) {
                        log_->appendPlainText(QStringLiteral("Flatpak: не удалось прочитать установленные программы"));
                    }

                    if (generation == catalogGeneration_) {
                        connectFlathub();
                    }

                    process->deleteLater();
                });

        connect(process, &QProcess::errorOccurred, this,
                [this, process, generation](QProcess::ProcessError error) {
                    if (error == QProcess::FailedToStart) {
                        if (generation == catalogGeneration_) {
                            log_->appendPlainText(QStringLiteral("Flatpak: ") + process->errorString());
                        }

                        process->deleteLater();
                    }
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
        const bool installedOnly = source == Source::Synapse &&
                                   arguments == QStringList{QStringLiteral("ls"), QStringLiteral("-i")};
        const bool loadDescriptions = source == Source::Synapse &&
                                      arguments == QStringList{QStringLiteral("ls")};
        const bool descriptions = source == Source::Synapse &&
                                  arguments == QStringList{QStringLiteral("search")};
        process->setProgram(program);
        process->setArguments(arguments);
        process->setProcessChannelMode(QProcess::SeparateChannels);
        process->setStandardOutputFile(outputPath, QIODevice::Truncate);

        connect(process, &QProcess::finished, this,
                [this, process, source, outputPath, generation, program, loadDescriptions, descriptions, installedOnly]
                (int code, QProcess::ExitStatus exitStatus) {
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
                        if (installedOnly) {
                            synapseInstalledKnown_ = true;
                        }

                        parseCatalog(source, QString::fromUtf8(output), descriptions, installedOnly);
                        renderCatalog();
                        if (installedOnly) {
                            readCatalog(source, program, {QStringLiteral("ls")});
                        } else if (loadDescriptions) {
                            readCatalog(source, program, {QStringLiteral("search")});
                        }
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

    // Совместить состояние и описания setup либо разобрать табличный вывод Flatpak
    void parseCatalog(Source source, const QString &output, bool descriptions = false, bool installedOnly = false) {
        const QRegularExpression setupLine(
            QStringLiteral("^(\\S+)\\s+(\\S+)\\s+\\[([^\\]]+)\\]\\s*(.*)$"));
        const QRegularExpression installedLine(QStringLiteral("^(\\S+)\\s+(\\S+)\\s+(\\S+)(?:\\s.*)?$"));

        for (const QString &line : output.split(QLatin1Char('\n'))) {
            if (source == Source::Synapse) {
                const QRegularExpressionMatch match =
                    (installedOnly ? installedLine : setupLine).match(line.trimmed());
                if (!match.hasMatch()) {
                    continue;
                }

                if (!validPackageId(match.captured(1))) {
                    continue;
                }

                if (installedOnly) {
                    packages_.append({source, match.captured(1), match.captured(1), match.captured(2),
                                      QString(), true, true});
                } else if (descriptions) {
                    for (Package &package : packages_) {
                        if (package.source == source && package.id == match.captured(1)) {
                            package.description = match.captured(4).trimmed();
                            break;
                        }
                    }
                } else {
                    synapseAvailableVersions_.insert(match.captured(1), match.captured(2));
                    bool found = false;
                    for (const Package &package : packages_) {
                        if (package.source == source && package.id == match.captured(1)) {
                            found = true;
                            break;
                        }
                    }

                    if (!found) {
                        packages_.append({source, match.captured(1), match.captured(1), match.captured(2),
                                          QString(), false, synapseInstalledKnown_});
                    }
                }
                continue;
            }

            if (source == Source::Flathub) {
                const QStringList columns = line.split(QLatin1Char('\t'));
                if (columns.size() < 5) {
                    continue;
                }

                const QString ref = flatpakApplicationRef(columns[0], columns[3], columns[4]);
                if (ref.isEmpty() || flatpakInstalled_.contains(ref)) {
                    continue;
                }

                const QString version = columns[2].isEmpty()
                                            ? QStringLiteral("ветка %1").arg(columns[3]) : columns[2];
                packages_.append({source, columns[0], columns[1].isEmpty() ? columns[0] : columns[1],
                                  version, QString(), false, flatpakInstalledKnown_, ref, QStringLiteral("flathub")});
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
            const QString origin = package.source == Source::Flathub && package.origin != QStringLiteral("flathub")
                                       ? QStringLiteral("Flatpak") : sourceName(package.source);
            table_->setItem(row, 1, new QTableWidgetItem(origin));
            table_->setItem(row, 2, new QTableWidgetItem(package.version));
            if (!package.ref.isEmpty()) {
                table_->item(row, 2)->setToolTip(package.ref);
            }
            if (package.source == Source::Synapse && package.installed) {
                const QString available = synapseAvailableVersions_.value(package.id);
                if (!available.isEmpty() && available != package.version) {
                    table_->item(row, 2)->setToolTip(
                        QStringLiteral("Установлена: %1\nВ каталоге: %2").arg(package.version, available));
                }
            }

            const QString state = canUpdate(package) ? QStringLiteral("Доступно обновление")
                                  : package.installed ? QStringLiteral("Установлено")
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

    // Предлагать обновление setup только на проверенную более новую версию
    bool canUpdate(const Package &package) const {
        return package.source == Source::Synapse && package.installed && synapseInstalledKnown_ &&
               newerSynapseVersion(synapseAvailableVersions_.value(package.id), package.version);
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
                               (!packages_[visible_[row]].installed || canUpdate(packages_[visible_[row]]));
        const bool update = row >= 0 && row < visible_.size() && canUpdate(packages_[visible_[row]]);
        install_->setText(update ? QStringLiteral("Обновить") : QStringLiteral("Установить"));
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

        const bool update = canUpdate(package);
        if (QMessageBox::question(this, update ? QStringLiteral("Обновление") : QStringLiteral("Установка"),
                                  (update ? QStringLiteral("Обновить %1 из %2?")
                                          : QStringLiteral("Установить %1 из %2?")).arg(package.name,
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

            arguments.append({QStringLiteral("-y"), QStringLiteral("flathub"), package.ref});
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

        auto outputOffset = QSharedPointer<qint64>::create(0);
        auto outputDecoder = QSharedPointer<QStringDecoder>::create(QStringDecoder::Utf8);
        auto *outputTimer = new QTimer(process);
        outputTimer->setInterval(500);

        // Читать новые фрагменты без повторного вывода и сохранять неполные символы UTF-8
        auto readOutput = [this, outputPath, outputOffset, outputDecoder]() {
            constexpr qint64 outputLimit = 8 * 1024 * 1024;
            if (*outputOffset >= outputLimit) {
                return false;
            }

            QFile outputFile(outputPath);
            if (!outputFile.open(QIODevice::ReadOnly) || !outputFile.seek(*outputOffset)) {
                return false;
            }

            const QByteArray bytes = outputFile.read(qMin<qint64>(65536, outputLimit - *outputOffset));
            if (bytes.isEmpty()) {
                return false;
            }

            *outputOffset += bytes.size();
            const QString text = (*outputDecoder)(bytes);
            log_->moveCursor(QTextCursor::End);
            log_->insertPlainText(QString(text).replace(QLatin1Char('\r'), QLatin1Char('\n')));
            log_->ensureCursorVisible();
            return true;
        };

        connect(outputTimer, &QTimer::timeout, this, [readOutput]() { readOutput(); });
        connect(process, &QProcess::started, outputTimer, [outputTimer]() { outputTimer->start(); });

        if (!secret->isEmpty()) {
            connect(process, &QProcess::started, this, [process, secret]() {
                process->write(*secret);
                process->write("\n", 1);
                process->closeWriteChannel();
                clearSecret(*secret);
            });
        }

        connect(process, &QProcess::finished, this,
                [this, process, package, outputPath, outputTimer, readOutput]
                (int code, QProcess::ExitStatus exitStatus) {
                    outputTimer->stop();
                    while (readOutput()) {
                    }

                    const bool success = exitStatus == QProcess::NormalExit && code == 0;
                    QFile(outputPath).remove();

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
                [this, process, package, secret, outputPath, outputTimer](QProcess::ProcessError error) {
                    if (error != QProcess::FailedToStart) {
                        return;
                    }

                    outputTimer->stop();
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
    QPushButton *prepareDebian_ = nullptr;
    bool catalogUpdating_ = false;
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
    bool flatpakInstalledKnown_ = false;
    QHash<QString, QString> synapseAvailableVersions_;
    bool synapseInstalledKnown_ = false;
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
