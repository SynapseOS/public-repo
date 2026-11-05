/*
    main.cpp: Меню программ и графические настройки SynapseOS
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI: synapse-x86_64-v1

    Описание: Запуск установленных программ и изменение общих настроек системы
    Подсистема: рабочий стол
    Первое изменение: 25.10.2026
    Последнее изменение: 01.11.2026
    Доступ: общедоступный
*/

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListView>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMainWindow>
#include <QMessageBox>
#include <QPalette>
#include <QPixmap>
#include <QProcess>
#include <QSet>
#include <QShowEvent>
#include <QStyle>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSize>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <iterator>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

struct ApplicationEntry {
	const char *name;
	const char *category;
	const char *path;
	const char *argument;
	const char *icon;
};

static const ApplicationEntry applications[] = {
	{"Файлы", "Система", "/bin/synapse-files", nullptr, "/usr/share/weston/home.png"},
	{"Терминал", "Система", "/bin/weston-terminal", nullptr, "/usr/share/weston/terminal.png"},
	{"Настройки", "Система", "/bin/synapse-desktop", "--settings", "/usr/share/weston/settings-icon.png"},
	{"Магазин программ", "Система", "/bin/synapse-store", nullptr, "/usr/share/weston/synapse-logo.png"},
	{"Монитор системы", "Система", "/bin/weston-terminal", "--shell=/bin/htop", nullptr},
	{"Mindustry", "Игры", "/bin/mindustry", nullptr, "/usr/share/weston/mindustry-icon.png"},
};

// Прочитать поля приложения из одного файла Flatpak
static QHash<QString, QString> flatpakFields(const QString &path) {
	QHash<QString, QString> fields;
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return fields;
	}

	bool desktopEntry = false;
	while (!file.atEnd()) {
		QString line = QString::fromUtf8(file.readLine()).trimmed();
		if (line.startsWith(QLatin1Char('['))) {
			desktopEntry = line == QStringLiteral("[Desktop Entry]");
			continue;
		}

		if (desktopEntry) {
			const qsizetype separator = line.indexOf(QLatin1Char('='));
			if (separator > 0) {
				const QString key = line.left(separator);
				if (!fields.contains(key)) {
					fields.insert(key, line.mid(separator + 1));
				}
			}
		}
	}

	return fields;
}

// Получить одно поле файла Flatpak при запуске программы
static QString flatpakValue(const QString &path, const QString &key) {
	return flatpakFields(path).value(key);
}

// Найти каталоги, в которых Flatpak публикует программы
static QStringList flatpakExportPaths() {
	return {QDir::homePath() + QStringLiteral("/.local/share/flatpak/exports/share/applications"),
	        QStringLiteral("/var/lib/flatpak/exports/share/applications")};
}

// Собрать существующие каталоги значков Flatpak один раз для списка программ
static QStringList flatpakIconDirectories() {
	const QStringList roots = {
	    QDir::homePath() + QStringLiteral("/.local/share/flatpak/exports/share/icons/hicolor/"),
	    QStringLiteral("/var/lib/flatpak/exports/share/icons/hicolor/")};
	const QStringList sizes = {QStringLiteral("128x128"), QStringLiteral("64x64"),
	                           QStringLiteral("48x48"), QStringLiteral("256x256"),
	                           QStringLiteral("32x32"), QStringLiteral("16x16"),
	                           QStringLiteral("scalable")};
	QStringList directories;
	for (const QString &root : roots) {
		for (const QString &size : sizes) {
			const QString path = root + size + QStringLiteral("/apps/");
			if (QDir(path).exists()) {
				directories.append(path);
			}
		}
	}

	return directories;
}

// Найти значок в том же порядке, что и экспорт Flatpak
static QIcon flatpakIcon(const QString &name, const QStringList &directories) {
	if (name.startsWith(QLatin1Char('/'))) {
		return QIcon(name);
	}

	for (const QString &directory : directories) {
		for (const QString &suffix : {QStringLiteral(".png"), QStringLiteral(".svg")}) {
			const QString path = directory + name + suffix;
			if (QFileInfo::exists(path)) {
				return QIcon(path);
			}
		}
	}

	const QStringList sizes = {QStringLiteral("128x128"), QStringLiteral("64x64"),
	                           QStringLiteral("48x48"), QStringLiteral("256x256"),
	                           QStringLiteral("32x32"), QStringLiteral("16x16"),
	                           QStringLiteral("scalable")};
	const QString active = QStringLiteral("/var/lib/flatpak/app/") + name +
	                       QStringLiteral("/current/active/export/share/icons/hicolor/");
	if (QDir(active).exists()) {
		for (const QString &size : sizes) {
			for (const QString &suffix : {QStringLiteral(".png"), QStringLiteral(".svg")}) {
				const QString path = active + size + QStringLiteral("/apps/") + name + suffix;
				if (QFileInfo::exists(path)) {
					return QIcon(path);
				}
			}
		}
	}

	return QIcon::fromTheme(name);
}

// Прочитать значение из системного INI без промежуточного процесса и канала вывода
static QString readSetting(const QString &key) {
	QString fallback;
	if (key == QStringLiteral("System.Language")) {
		fallback = QStringLiteral("ru");
	} else if (key == QStringLiteral("System.Timezone")) {
		fallback = QStringLiteral("Europe/Moscow");
	} else if (key == QStringLiteral("Boot.DefaultShell")) {
		fallback = QStringLiteral("/bin/sh");
	} else if (key == QStringLiteral("Network.Mode")) {
		fallback = QStringLiteral("DHCP");
	} else if (key == QStringLiteral("DNS.1")) {
		fallback = QStringLiteral("77.88.8.8");
	} else if (key == QStringLiteral("Display.Resolution")) {
		fallback = QStringLiteral("auto");
	}

	QString path = key;
	path.replace(QLatin1Char('.'), QLatin1Char('/'));
	QSettings settings(QStringLiteral("/etc/settings.ini"), QSettings::IniFormat);
	return settings.value(path, fallback).toString().trimmed();
}

// Сохранить настройку через тот же обработчик, что использует консоль
static bool writeSetting(QWidget *parent, const QString &key, const QString &value) {
	QProcess command;
	command.start(QStringLiteral("/bin/settings"), {QStringLiteral("--set"), key, value});

	if (!command.waitForFinished(5000) || command.exitCode() != 0) {
		QString error = QString::fromUtf8(command.readAllStandardError()).trimmed();

		if (error.isEmpty()) {
			error = QStringLiteral("Не удалось сохранить настройку");
		}

		QMessageBox::warning(parent, QStringLiteral("Настройки SynapseOS"), error);
		return false;
	}

	return true;
}

// Подобрать цвета меню и настроек к выбранной теме окон
static void applyDesktopPalette(const QString &theme) {
	const bool light = theme == QStringLiteral("Arc-Clone");
	const bool penumbra = theme == QStringLiteral("Penumbra");
	const bool numix = theme == QStringLiteral("Numix-Clone");
	const QString background = light ? QStringLiteral("#e7e9ed") :
	                           penumbra ? QStringLiteral("#243842") :
	                           numix ? QStringLiteral("#303238") : QStringLiteral("#1b2027");
	const QString base = light ? QStringLiteral("#ffffff") :
	                     penumbra ? QStringLiteral("#1b2e37") :
	                     numix ? QStringLiteral("#26282d") : QStringLiteral("#141920");
	const QString surface = light ? QStringLiteral("#d9e0e7") :
	                        penumbra ? QStringLiteral("#304b57") :
	                        numix ? QStringLiteral("#41434a") : QStringLiteral("#29333d");
	const QString foreground = light ? QStringLiteral("#202a34") : QStringLiteral("#f3f4f5");
	const QString accent = theme == QStringLiteral("Numix-Clone") ?
	                       QStringLiteral("#d65d3a") : theme == QStringLiteral("Penumbra") ?
	                       QStringLiteral("#769fb4") : QStringLiteral("#4388c5");

	QPalette palette;
	palette.setColor(QPalette::Window, QColor(background));
	palette.setColor(QPalette::WindowText, QColor(foreground));
	palette.setColor(QPalette::Base, QColor(base));
	palette.setColor(QPalette::AlternateBase, QColor(surface));
	palette.setColor(QPalette::Text, QColor(foreground));
	palette.setColor(QPalette::Button, QColor(surface));
	palette.setColor(QPalette::ButtonText, QColor(foreground));
	palette.setColor(QPalette::Highlight, QColor(accent));
	palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
	palette.setColor(QPalette::PlaceholderText, QColor(light ? QStringLiteral("#63717d") :
	                                                  QStringLiteral("#aab4be")));
	QApplication::setPalette(palette);

	qApp->setStyleSheet(QStringLiteral(
	    "QMainWindow, QWidget { background-color: %1; color: %4; }"
	    "QLineEdit, QListWidget, QComboBox { background-color: %2; color: %4;"
	    " border: 1px solid %3; border-radius: 7px; padding: 5px; }"
	    "QListWidget::item { border-radius: 5px; padding: 4px; }"
	    "QListWidget::item:selected { background-color: %5; color: white; }"
	    "QPushButton, QToolButton { background-color: %3; color: %4;"
	    " border: 0; border-radius: 7px; padding: 8px; }"
	    "QPushButton:hover, QToolButton:hover { background-color: %5; color: white; }"
	    "QTabBar::tab { background-color: %3; color: %4; padding: 9px 16px; }"
	    "QTabBar::tab:selected { background-color: %5; color: white; }"
	    ).arg(background, base, surface, foreground, accent));
}

// Прочитать тему из настройки Labwc до показа меню
static QString desktopTheme() {
	QString path = QDir::homePath() + QStringLiteral("/.config/labwc/rc.xml");
	if (!QFileInfo::exists(path)) {
		path = QStringLiteral("/etc/xdg/labwc/rc.xml");
	}

	QFile file(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return QStringLiteral("Numix-Clone");
	}

	const QString config = QString::fromUtf8(file.readAll());
	const qsizetype theme = config.indexOf(QStringLiteral("<theme>"));
	const qsizetype start = config.indexOf(QStringLiteral("<name>"), theme);
	const qsizetype end = config.indexOf(QStringLiteral("</name>"), start);
	if (theme < 0 || start < 0 || end < 0) {
		return QStringLiteral("Numix-Clone");
	}

	return config.mid(start + 6, end - start - 6).trimmed();
}

class MenuWindow final : public QMainWindow {
public:
	// Показать системное меню с разделами, поиском и быстрыми действиями
	MenuWindow() {
		setWindowTitle(QStringLiteral("Меню SynapseOS"));
		setWindowIcon(QIcon(QStringLiteral("/usr/share/weston/synapse-logo.png")));
		resize(680, 540);

		QWidget *content = new QWidget(this);
		QVBoxLayout *layout = new QVBoxLayout(content);
		layout->setContentsMargins(16, 14, 16, 14);
		layout->setSpacing(12);

		QHBoxLayout *header = new QHBoxLayout();
		QLabel *logo = new QLabel(content);
		QPixmap logoImage(QStringLiteral("/usr/share/weston/synapse-logo.png"));
		logo->setPixmap(logoImage.scaled(42, 42, Qt::KeepAspectRatio, Qt::SmoothTransformation));
		logo->setFixedSize(46, 46);
		header->addWidget(logo);

		QVBoxLayout *identity = new QVBoxLayout();
		QLabel *heading = new QLabel(QStringLiteral("SynapseOS"), content);
		QFont font = heading->font();
		font.setPointSize(17);
		font.setBold(true);
		heading->setFont(font);
		identity->addWidget(heading);
		identity->addWidget(new QLabel(QStringLiteral("Программы и действия"), content));
		header->addLayout(identity);
		header->addStretch();
		layout->addLayout(header);

		search = new QLineEdit(content);
		search->setPlaceholderText(QStringLiteral("Найти программу…"));
		search->setMinimumHeight(36);
		layout->addWidget(search);

		QLabel *quickTitle = new QLabel(QStringLiteral("Быстрый доступ"), content);
		QFont quickFont = quickTitle->font();
		quickFont.setBold(true);
		quickTitle->setFont(quickFont);
		layout->addWidget(quickTitle);

		QHBoxLayout *quick = new QHBoxLayout();
		quick->setSpacing(8);
		for (int index : {0, 1, 2}) {
			const ApplicationEntry &entry = applications[index];
			if (!QFileInfo(QString::fromUtf8(entry.path)).isExecutable()) {
				continue;
			}

			QToolButton *button = new QToolButton(content);
			button->setText(QString::fromUtf8(entry.name));
			button->setIcon(QIcon(QString::fromUtf8(entry.icon)));
			button->setIconSize(QSize(28, 28));
			button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
			button->setMinimumHeight(46);
			quick->addWidget(button, 1);
			connect(button, &QToolButton::clicked, this, [this, index]() {
				const ApplicationEntry &selected = applications[index];
				QStringList arguments;
				if (selected.argument != nullptr) {
					arguments.append(QString::fromUtf8(selected.argument));
				}

				if (QProcess::startDetached(QString::fromUtf8(selected.path), arguments)) {
					close();
				}
			});
		}
		layout->addLayout(quick);

		QHBoxLayout *browser = new QHBoxLayout();
		category = new QListWidget(content);
		category->setFixedWidth(150);
		category->setSpacing(3);
		category->addItems({QStringLiteral("Все программы"), QStringLiteral("Система"),
		                    QStringLiteral("Игры"), QStringLiteral("Flatpak")});
		category->setCurrentRow(0);
		browser->addWidget(category);

		list = new QListWidget(content);
		list->setViewMode(QListView::ListMode);
		list->setIconSize(QSize(30, 30));
		list->setSpacing(4);
		list->setAlternatingRowColors(true);
		browser->addWidget(list, 1);
		layout->addLayout(browser, 1);

		count = new QLabel(content);
		layout->addWidget(count);

		QHBoxLayout *actions = new QHBoxLayout();
		QPushButton *files = new QPushButton(QStringLiteral("Файлы"), content);
		QPushButton *terminal = new QPushButton(QStringLiteral("Терминал"), content);
		QPushButton *settings = new QPushButton(QStringLiteral("Настройки"), content);
		const bool loginManager = readSetting(QStringLiteral("Desktop.LoginManager")) ==
		                          QStringLiteral("1");
		QPushButton *logout = new QPushButton(loginManager ?
		    QStringLiteral("Сменить пользователя") : QStringLiteral("Завершить сеанс"), content);
		actions->addWidget(files);
		actions->addWidget(terminal);
		actions->addWidget(settings);
		actions->addStretch();
		actions->addWidget(logout);
		layout->addLayout(actions);
		setCentralWidget(content);

		connect(search, &QLineEdit::textChanged, this, [this]() { refresh(); });
		connect(category, &QListWidget::currentRowChanged, this, [this]() { refresh(); });
		connect(list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
			launch(item);
		});
		connect(files, &QPushButton::clicked, this, [this]() {
			QProcess::startDetached(QStringLiteral("/bin/synapse-files"));
			close();
		});
		connect(terminal, &QPushButton::clicked, this, [this]() {
			QProcess::startDetached(QStringLiteral("/bin/weston-terminal"));
			close();
		});
		connect(settings, &QPushButton::clicked, this, [this]() {
			QProcess::startDetached(QStringLiteral("/bin/synapse-desktop"),
			                        {QStringLiteral("--settings")});
			close();
		});
		connect(logout, &QPushButton::clicked, this, [this]() {
			QProcess::startDetached(QStringLiteral("/bin/labwc"),
			                        {QStringLiteral("--exit")});
			close();
		});
		refresh();
		search->setFocus();
	}

	// Проверить наличие хотя бы одной доступной программы
	bool ready() const {
		return list->count() > 0;
	}

protected:
	// Подхватить установленные программы перед показом уже работающего меню
	void showEvent(QShowEvent *event) override {
		QMainWindow::showEvent(event);
		if (updateFlatpakEntries()) {
			refresh();
		}
	}

private:
	struct FlatpakEntry {
		QString id;
		QString title;
		QString path;
		QIcon icon;
		bool userInstall;
	};

	struct ExportStamp {
		bool exists = false;
		dev_t device = 0;
		ino_t inode = 0;
		timespec modified = {};
		timespec changed = {};
	};

	QLineEdit *search = nullptr;
	QListWidget *category = nullptr;
	QListWidget *list = nullptr;
	QLabel *count = nullptr;
	QList<FlatpakEntry> flatpakEntries;
	std::array<ExportStamp, 2> exportStamps = {};
	bool exportsReady = false;

	// Перечитать экспорт Flatpak только после изменения каталога приложений
	bool updateFlatpakEntries() {
		const QStringList paths = flatpakExportPaths();
		std::array<ExportStamp, 2> current = {};
		for (int index = 0; index < 2; index++) {
			struct stat details;
			if (stat(QFile::encodeName(paths[index]).constData(), &details) == 0 &&
			    S_ISDIR(details.st_mode)) {
				current[index] = {true, details.st_dev, details.st_ino,
				                  details.st_mtim, details.st_ctim};
			}
		}

		bool changed = !exportsReady;
		for (int index = 0; index < 2; index++) {
			const ExportStamp &before = exportStamps[index];
			const ExportStamp &after = current[index];
			changed |= before.exists != after.exists || before.device != after.device ||
			           before.inode != after.inode || before.modified.tv_sec != after.modified.tv_sec ||
			           before.modified.tv_nsec != after.modified.tv_nsec ||
			           before.changed.tv_sec != after.changed.tv_sec ||
			           before.changed.tv_nsec != after.changed.tv_nsec;
		}

		if (!changed) {
			return false;
		}

		exportStamps = current;
		exportsReady = true;
		flatpakEntries.clear();
		QSet<QString> listed;
		QStringList iconDirectories;
		bool iconDirectoriesReady = false;
		for (int location = 0; location < paths.size(); location++) {
			const QDir exports(paths[location]);
			for (const QFileInfo &file : exports.entryInfoList({QStringLiteral("*.desktop")}, QDir::Files)) {
				QString id = file.fileName();
				id.chop(QStringLiteral(".desktop").size());
				if (listed.contains(id)) {
					continue;
				}

				listed.insert(id);
				const QString path = file.absoluteFilePath();
				const QHash<QString, QString> fields = flatpakFields(path);
				QString title = fields.value(QStringLiteral("Name[ru]"));
				if (title.isEmpty()) {
					title = fields.value(QStringLiteral("Name"));
				}

				if (title.isEmpty() || fields.value(QStringLiteral("Type")) != QStringLiteral("Application") ||
				    fields.value(QStringLiteral("Terminal")) == QStringLiteral("true") ||
				    fields.value(QStringLiteral("Hidden")) == QStringLiteral("true") ||
				    fields.value(QStringLiteral("NoDisplay")) == QStringLiteral("true")) {
					continue;
				}

				if (!iconDirectoriesReady) {
					iconDirectories = flatpakIconDirectories();
					iconDirectoriesReady = true;
				}

				flatpakEntries.append({id, title, path,
				                       flatpakIcon(fields.value(QStringLiteral("Icon")), iconDirectories),
				                       location == 0});
			}
		}

		return true;
	}

	// Перестроить список после изменения строки поиска
	void refresh() {
		updateFlatpakEntries();
		list->clear();
		QString query = search->text();

		for (int index = 0; index < static_cast<int>(std::size(applications)); index++) {
			const ApplicationEntry &entry = applications[index];
			QString title = QString::fromUtf8(entry.name);
			QString group = QString::fromUtf8(entry.category);

			if (!QFileInfo(QString::fromUtf8(entry.path)).isExecutable() ||
			    !title.contains(query, Qt::CaseInsensitive) ||
			    (category->currentRow() != 0 && category->currentItem()->text() != group)) {
				continue;
			}

			QIcon icon;
			if (entry.icon != nullptr) {
				icon = QIcon(QString::fromUtf8(entry.icon));
			} else if (title == QStringLiteral("Монитор системы")) {
				icon = QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
			}

			QListWidgetItem *item = new QListWidgetItem(
			    icon, title, list);
			item->setToolTip(group);
			item->setData(Qt::UserRole, index);
		}

		if (!QFileInfo(QStringLiteral("/bin/flatpak")).isExecutable() ||
		    (category->currentRow() != 0 && category->currentRow() != 3)) {
			count->setText(QStringLiteral("Программ: %1").arg(list->count()));
			return;
		}

		for (const FlatpakEntry &entry : flatpakEntries) {
			if (!entry.title.contains(query, Qt::CaseInsensitive)) {
				continue;
			}
			QListWidgetItem *item = new QListWidgetItem(
			    entry.icon, entry.title, list);
			item->setToolTip(QStringLiteral("Flatpak: %1").arg(entry.id));
			item->setData(Qt::UserRole, entry.id);
			item->setData(Qt::UserRole + 1, entry.path);
			item->setData(Qt::UserRole + 2, entry.userInstall);
		}

		count->setText(QStringLiteral("Программ: %1").arg(list->count()));
	}

	// Запустить выбранную программу отдельным процессом
	void launch(QListWidgetItem *item) {
		bool isBuiltIn = false;
		int index = item->data(Qt::UserRole).toInt(&isBuiltIn);
		if (!isBuiltIn) {
			QString id = item->data(Qt::UserRole).toString();
			QString path = item->data(Qt::UserRole + 1).toString();
			bool userInstall = item->data(Qt::UserRole + 2).toBool();
			QStringList command = QProcess::splitCommand(flatpakValue(path, QStringLiteral("Exec")));
			QStringList arguments = {QStringLiteral("run"),
			                         userInstall ? QStringLiteral("--user") : QStringLiteral("--system"),
			                         QStringLiteral("--socket=session-bus"),
			                         QStringLiteral("--no-a11y-bus"), id};
			int appIndex = command.indexOf(id);

			for (int part = appIndex + 1; appIndex >= 0 && part < command.size(); part++) {
				if (command[part].startsWith(QLatin1Char('%')) ||
				    command[part].startsWith(QStringLiteral("@@"))) {
					continue;
				}

				arguments.append(command[part]);
			}

			QProcess process;
			QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
			if (!environment.contains(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"))) {
				const QString runtime = environment.value(QStringLiteral("XDG_RUNTIME_DIR"));
				const QString bus = runtime.isEmpty() ?
				                    QStringLiteral("/var/run/labwc-runtime/bus") :
				                    runtime + QStringLiteral("/bus");
				environment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
				                   QStringLiteral("unix:path=") + bus);
			}

			process.setProcessEnvironment(environment);
			if (!process.startDetached(QStringLiteral("/bin/flatpak"), arguments)) {
				QMessageBox::warning(this, QStringLiteral("SynapseOS"),
				                     QStringLiteral("Не удалось запустить %1").arg(id));
				return;
			}

			close();
			return;
		}

		if (index < 0 || index >= static_cast<int>(std::size(applications))) {
			return;
		}

		const ApplicationEntry &entry = applications[index];
		QStringList arguments;

		if (entry.argument != nullptr) {
			arguments.append(QString::fromUtf8(entry.argument));
		}

		if (!QProcess::startDetached(QString::fromUtf8(entry.path), arguments)) {
			QMessageBox::warning(this, QStringLiteral("SynapseOS"),
			                     QStringLiteral("Не удалось запустить %1").arg(QString::fromUtf8(entry.name)));
			return;
		}

		close();
	}
};

class SettingsWindow final : public QMainWindow {
public:
	// Построить графические разделы поверх существующей программы settings
	SettingsWindow() {
		setWindowTitle(QStringLiteral("Настройки SynapseOS"));
		setWindowIcon(QIcon(QStringLiteral("/usr/share/weston/settings-icon.png")));
		resize(690, 480);

		QTabWidget *tabs = new QTabWidget(this);
		setCentralWidget(tabs);
		addDisplayPage(tabs);
		addAppearancePage(tabs);
		addSystemPage(tabs);
		addNetworkPage(tabs);
		addAboutPage(tabs);
	}

	// Убедиться, что системная программа настроек доступна
	bool ready() const {
		return QFileInfo(QStringLiteral("/bin/settings")).isExecutable() &&
		       !readSetting(QStringLiteral("Display.Resolution")).isEmpty();
	}

private:
	// Прочитать выбранную тему из пользовательского или системного файла Labwc
	QString currentTheme() const {
		return desktopTheme();
	}

	// Записать личную тему и попросить Labwc перечитать настройки
	bool applyTheme(const QString &name) {
		if (name != QStringLiteral("Numix-Clone") && name != QStringLiteral("Arc-Clone") &&
		    name != QStringLiteral("Penumbra") && name != QStringLiteral("SynapseOS-Dark")) {
			return false;
		}

		const QString userPath = QDir::homePath() + QStringLiteral("/.config/labwc/rc.xml");
		const QString sourcePath = QFileInfo::exists(userPath) ? userPath :
		                           QStringLiteral("/etc/xdg/labwc/rc.xml");
		QFile source(sourcePath);
		if (!source.open(QIODevice::ReadOnly | QIODevice::Text)) {
			return false;
		}

		QString config = QString::fromUtf8(source.readAll());
		const qsizetype theme = config.indexOf(QStringLiteral("<theme>"));
		const qsizetype start = config.indexOf(QStringLiteral("<name>"), theme);
		const qsizetype end = config.indexOf(QStringLiteral("</name>"), start);
		if (theme < 0 || start < 0 || end < 0) {
			return false;
		}

		config.replace(start + 6, end - start - 6, name);
		if (!QDir().mkpath(QFileInfo(userPath).absolutePath())) {
			return false;
		}

		QSaveFile target(userPath);
		if (!target.open(QIODevice::WriteOnly | QIODevice::Text)) {
			return false;
		}

		const QByteArray data = config.toUtf8();
		if (target.write(data) != data.size() || !target.commit()) {
			return false;
		}

		QProcess refresh;
		refresh.start(QStringLiteral("/bin/labwc"), {QStringLiteral("--reconfigure")});
		if (!refresh.waitForFinished(5000) || refresh.exitCode() != 0) {
			return false;
		}

		applyDesktopPalette(name);
		return true;
	}

	// Добавить готовые темы Labwc с переключением в текущем сеансе
	void addAppearancePage(QTabWidget *tabs) {
		QWidget *page = new QWidget(tabs);
		QVBoxLayout *layout = new QVBoxLayout(page);
		QFormLayout *form = new QFormLayout();
		QComboBox *theme = new QComboBox(page);
		theme->addItems({QStringLiteral("Numix-Clone"), QStringLiteral("Arc-Clone"),
		                 QStringLiteral("Penumbra"), QStringLiteral("SynapseOS-Dark")});
		theme->setCurrentText(currentTheme());
		form->addRow(QStringLiteral("Тема рабочего стола"), theme);
		layout->addLayout(form);

		QPushButton *apply = new QPushButton(QStringLiteral("Применить тему"), page);
		layout->addWidget(apply);
		layout->addStretch();
		connect(apply, &QPushButton::clicked, this, [this, theme]() {
			if (!applyTheme(theme->currentText())) {
				QMessageBox::warning(this, QStringLiteral("Настройки SynapseOS"),
				                     QStringLiteral("Не удалось применить тему Labwc"));
			}
		});
		tabs->addTab(page, QStringLiteral("Оформление"));
	}

	// Добавить выбор разрешения с применением в текущем сеансе
	void addDisplayPage(QTabWidget *tabs) {
		QWidget *page = new QWidget(tabs);
		QVBoxLayout *layout = new QVBoxLayout(page);
		QFormLayout *form = new QFormLayout();
		QComboBox *resolution = new QComboBox(page);
		resolution->addItem(QStringLiteral("Автоматически"), QStringLiteral("auto"));
		resolution->addItems({QStringLiteral("800x600"),
		                      QStringLiteral("1024x768"), QStringLiteral("1280x720"),
		                      QStringLiteral("1280x800"), QStringLiteral("1600x900"),
		                      QStringLiteral("1920x1080"), QStringLiteral("2560x1440"),
		                      QStringLiteral("3840x2160")});
		const QString configuredResolution = readSetting(QStringLiteral("Display.Resolution"));
		int selectedResolution = resolution->findText(configuredResolution);
		resolution->setCurrentIndex(selectedResolution < 0 ? 0 : selectedResolution);
		form->addRow(QStringLiteral("Разрешение"), resolution);
		layout->addLayout(form);
		QPushButton *apply = new QPushButton(QStringLiteral("Применить разрешение"), page);
		layout->addWidget(apply);
		layout->addWidget(new QLabel(QStringLiteral("Поддерживаемые видеоустройства меняют разрешение без перезагрузки."), page));
		layout->addStretch();
		connect(apply, &QPushButton::clicked, this, [this, resolution]() {
			writeSetting(this, QStringLiteral("Display.Resolution"), resolution->currentIndex() == 0
			                                                        ? QStringLiteral("auto") : resolution->currentText());
		});
		tabs->addTab(page, QStringLiteral("Экран"));
	}

	// Добавить язык, часовую зону и оболочку новых терминалов
	void addSystemPage(QTabWidget *tabs) {
		QWidget *page = new QWidget(tabs);
		QVBoxLayout *layout = new QVBoxLayout(page);
		QFormLayout *form = new QFormLayout();
		QComboBox *language = new QComboBox(page);
		language->addItem(QStringLiteral("Русский"), QStringLiteral("ru"));
		language->addItem(QStringLiteral("Английский"), QStringLiteral("en"));
		int selectedLanguage = language->findData(readSetting(QStringLiteral("System.Language")));
		language->setCurrentIndex(selectedLanguage < 0 ? 0 : selectedLanguage);
		form->addRow(QStringLiteral("Язык"), language);

		QComboBox *shell = new QComboBox(page);
		shell->addItems({QStringLiteral("/bin/zsh"), QStringLiteral("/bin/bash"),
		                 QStringLiteral("/bin/sh")});
		shell->setCurrentText(readSetting(QStringLiteral("Boot.DefaultShell")));
		form->addRow(QStringLiteral("Оболочка терминала"), shell);

		QLineEdit *timezone = new QLineEdit(readSetting(QStringLiteral("System.Timezone")), page);
		form->addRow(QStringLiteral("Часовой пояс"), timezone);
		layout->addLayout(form);

		QPushButton *apply = new QPushButton(QStringLiteral("Сохранить настройки системы"), page);
		layout->addWidget(apply);
		layout->addWidget(new QLabel(
		    QStringLiteral("Язык применяется в новых сеансах. Часовой пояс применяется после перезагрузки."), page));
		layout->addStretch();
		connect(apply, &QPushButton::clicked, this, [this, language, shell, timezone]() {
			if (!writeSetting(this, QStringLiteral("System.Language"), language->currentData().toString())) {
				return;
			}

			if (!writeSetting(this, QStringLiteral("Boot.DefaultShell"), shell->currentText())) {
				return;
			}

			writeSetting(this, QStringLiteral("System.Timezone"), timezone->text());
		});
		tabs->addTab(page, QStringLiteral("Система"));
	}

	// Добавить основные параметры сети без изменения текущего подключения
	void addNetworkPage(QTabWidget *tabs) {
		QWidget *page = new QWidget(tabs);
		QVBoxLayout *layout = new QVBoxLayout(page);
		QFormLayout *form = new QFormLayout();
		QComboBox *mode = new QComboBox(page);
		mode->addItem(QStringLiteral("DHCP"), QStringLiteral("DHCP"));
		mode->addItem(QStringLiteral("Статический"), QStringLiteral("Static"));
		int selectedMode = mode->findData(readSetting(QStringLiteral("Network.Mode")));
		mode->setCurrentIndex(selectedMode < 0 ? 0 : selectedMode);
		form->addRow(QStringLiteral("Режим"), mode);

		QLineEdit *address = new QLineEdit(readSetting(QStringLiteral("Network.IP")), page);
		QLineEdit *subnet = new QLineEdit(readSetting(QStringLiteral("Network.Subnet")), page);
		QLineEdit *gateway = new QLineEdit(readSetting(QStringLiteral("Network.Gateway")), page);
		QLineEdit *dns = new QLineEdit(readSetting(QStringLiteral("DNS.1")), page);
		for (QLineEdit *field : {address, subnet, gateway, dns}) {
			field->setMinimumWidth(260);
		}
		form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		dns->setToolTip(dns->text());
		form->addRow(QStringLiteral("Адрес IPv4"), address);
		form->addRow(QStringLiteral("Подсеть"), subnet);
		form->addRow(QStringLiteral("Шлюз"), gateway);
		form->addRow(QStringLiteral("Сервер DNS"), dns);
		layout->addLayout(form);

		QPushButton *apply = new QPushButton(QStringLiteral("Сохранить настройки сети"), page);
		layout->addWidget(apply);
		layout->addWidget(new QLabel(QStringLiteral("Настройки сети применяются после перезагрузки."), page));
		layout->addStretch();
		connect(apply, &QPushButton::clicked, this, [this, mode, address, subnet, gateway, dns]() {
			if (!writeSetting(this, QStringLiteral("Network.Mode"), mode->currentData().toString())) {
				return;
			}

			if (mode->currentData().toString() == QStringLiteral("Static")) {
				if (!writeSetting(this, QStringLiteral("Network.IP"), address->text()) ||
				    !writeSetting(this, QStringLiteral("Network.Subnet"), subnet->text()) ||
				    !writeSetting(this, QStringLiteral("Network.Gateway"), gateway->text())) {
					return;
				}
			}

			writeSetting(this, QStringLiteral("DNS.1"), dns->text());
		});
		tabs->addTab(page, QStringLiteral("Сеть"));
	}

	// Показать краткую справку о доступных действиях на рабочем столе
	void addAboutPage(QTabWidget *tabs) {
		QWidget *page = new QWidget(tabs);
		QVBoxLayout *layout = new QVBoxLayout(page);
		layout->addWidget(new QLabel(QStringLiteral("Рабочий стол SynapseOS\nLabwc и Qt 6"), page));
		layout->addWidget(new QLabel(
		    QStringLiteral("Print Screen сохраняет PNG в домашнем каталоге.\n"
		                   "Alt+Tab переключает окна. Alt+Shift меняет раскладку клавиатуры."), page));
		layout->addStretch();
		tabs->addTab(page, QStringLiteral("О системе"));
	}
};

// Передать переключение уже запущенному меню без загрузки графической библиотеки
static bool notifyMenu(char command) {
	const char *runtime = std::getenv("XDG_RUNTIME_DIR");
	if (runtime == nullptr || runtime[0] == 0) {
		return false;
	}

	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	if (std::snprintf(address.sun_path, sizeof(address.sun_path), "%s/synapse-desktop-menu",
	                  runtime) >= static_cast<int>(sizeof(address.sun_path))) {
		return false;
	}

	int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
	if (descriptor < 0) {
		return false;
	}

	bool delivered = connect(descriptor, reinterpret_cast<sockaddr *>(&address),
	                         sizeof(address)) == 0 &&
	                 send(descriptor, &command, 1, MSG_NOSIGNAL) == 1;
	close(descriptor);
	return delivered;
}

// Выбрать меню программ либо окно настроек и проверить его в госте
int main(int argc, char **argv) {
	bool settingsMode = false;
	bool selfTest = false;
	bool serviceMode = false;
	for (int index = 1; index < argc; index++) {
		settingsMode |= std::strcmp(argv[index], "--settings") == 0;
		selfTest |= std::strcmp(argv[index], "--self-test") == 0;
		serviceMode |= std::strcmp(argv[index], "--menu-service") == 0;
	}

	if (!settingsMode && !selfTest && !serviceMode && notifyMenu('T')) {
		return 0;
	}

	QApplication application(argc, argv);
	application.setStyle(QStringLiteral("Fusion"));
	applyDesktopPalette(desktopTheme());
	QLocalServer menuServer;
	if (!settingsMode && !selfTest) {
		const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR");
		if (!runtime.isEmpty()) {
			const QString socket = QString::fromLocal8Bit(runtime) +
			                       QStringLiteral("/synapse-desktop-menu");
			if (!menuServer.listen(socket) &&
			    notifyMenu(serviceMode ? 'P' : 'T')) {
				return 0;
			}

			if (!menuServer.isListening()) {
				QLocalServer::removeServer(socket);
				menuServer.listen(socket);
			}
		}
	}

	bool persistent = menuServer.isListening();
	if (serviceMode && !persistent) {
		return 1;
	}
	application.setQuitOnLastWindowClosed(!persistent);
	QMainWindow *window = settingsMode ? static_cast<QMainWindow *>(new SettingsWindow())
	                                 : static_cast<QMainWindow *>(new MenuWindow());
	if (!serviceMode) {
		window->show();
	}
	QObject::connect(&menuServer, &QLocalServer::newConnection, &application,
	                 [&menuServer, window]() {
		while (menuServer.hasPendingConnections()) {
			QLocalSocket *client = menuServer.nextPendingConnection();
			client->waitForReadyRead(25);
			const QByteArray request = client->readAll();
			if (request.contains('T')) {
				if (window->isVisible()) {
					window->hide();
				} else {
					window->show();
					window->raise();
					window->activateWindow();
				}
			}

			client->disconnectFromServer();
			client->deleteLater();
		}
	});

	if (selfTest) {
		QTimer::singleShot(1500, &application, [&application, window, settingsMode]() {
			bool ready = settingsMode ? static_cast<SettingsWindow *>(window)->ready()
			                          : static_cast<MenuWindow *>(window)->ready();

			if (!ready) {
				std::puts("SYNAPSE_DESKTOP_FAIL");
				application.exit(1);
				return;
			}

			std::puts("SYNAPSE_DESKTOP_PASS");
			application.quit();
		});
	}

	return application.exec();
}
