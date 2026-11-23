/*
    main.cpp: Меню программ и графические настройки SynapseOS
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI: synapse-x86_64-v1

    Описание: Запуск установленных программ и изменение общих настроек системы
    Подсистема: рабочий стол
    Первое изменение: 25.10.2026
    Последнее изменение: 23.11.2026
    Доступ: общедоступный
*/

#include <ctime>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QFontComboBox>
#include <QDateTimeEdit>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListView>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSocketNotifier>
#include <QMainWindow>
#include <QMessageBox>
#include <QPalette>
#include <QPixmap>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QShowEvent>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStyle>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QSlider>
#include <QPlainTextEdit>
#include <QSysInfo>
#include <QSettings>
#include <QSize>
#include <QStackedWidget>
#include <QTabBar>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <cstring>
#include <fcntl.h>
#include <cerrno>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>

#define SYS_SYN_GA_VGPU_SET_MAX_FPS 104100L
#define SYS_SYN_GA_VGPU_GET_MAX_FPS 104101L
#include <lumine/wifi.h>
#include <synapse/gpu.h>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <iterator>
#include <pwd.h>
#include <grp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static bool runPrivileged(QWidget *parent, const QString &program, const QStringList &arguments,
                          const QByteArray &input, QString &output);

#include "subsystems.h"

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
	{"Системный монитор", "Система", "/bin/synapse-desktop", "--monitor", "/usr/share/weston/utilities-system-monitor.png"},
	{"Midnight Commander", "Система", "/bin/weston-terminal", "--shell=/bin/mc", nullptr},
	{"Редактор nano", "Система", "/bin/weston-terminal", "--shell=/bin/nano", nullptr},
	{"Занятое место (ncdu)", "Система", "/bin/weston-terminal", "--shell=/bin/ncdu", nullptr},
	{"Python 3", "Система", "/bin/weston-terminal", "--shell=/bin/python3", nullptr},
	{"Lua", "Система", "/bin/weston-terminal", "--shell=/bin/lua", nullptr},
	{"SQLite", "Система", "/bin/weston-terminal", "--shell=/bin/sqlite3", nullptr},
	{"Оболочка Zsh", "Система", "/bin/weston-terminal", "--shell=/bin/zsh", nullptr},
	{"Mindustry", "Игры", "/bin/mindustry", nullptr, "/usr/share/weston/mindustry-icon.png"},
	{"VLC медиаплеер", "Мультимедиа", "/bin/vlc", nullptr, "/usr/share/icons/hicolor/48x48/apps/vlc.png"},
	{"Запись экрана", "Мультимедиа", "/bin/recorder-desktop", nullptr, "/usr/share/icons/hicolor/128x128/apps/synapse-recorder.png"},
	{"Камера", "Мультимедиа", "/bin/synapse-camera", nullptr, "/usr/share/icons/hicolor/128x128/apps/synapse-camera.png"},
	{"Диктофон", "Мультимедиа", "/bin/synapse-dictaphone", nullptr, "/usr/share/icons/hicolor/128x128/apps/synapse-dictaphone.png"},
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

struct PrivilegedCommand {
	QString program;
	QStringList arguments;
	QByteArray input;
};

static QByteArray sudoPassword;

// Подготовить одну операцию root-службы без shell и без сохранения пароля в файлы
static bool preparePrivileged(QWidget *parent, const QString &program, const QStringList &arguments,
                              const QByteArray &input, PrivilegedCommand &command) {
	if (geteuid() == 0) {
		command = {program, arguments, input};
		return true;
	}
	if (sudoPassword.isEmpty()) {
		bool accepted = false;
		QString password = QInputDialog::getText(parent, QStringLiteral("Нужны права администратора"),
		    QStringLiteral("Пароль администратора"), QLineEdit::Password, QString(), &accepted);
		if (!accepted || password.isEmpty()) {
			password.fill(QChar(0));
			return false;
		}
		sudoPassword = password.toUtf8();
		password.fill(QChar(0));
		QTimer::singleShot(60000, parent, []() {
			sudoPassword.fill(0);
			sudoPassword.clear();
		});
	}
	command.program = QStringLiteral("/bin/synapse-store-control");
	if (program == QStringLiteral("/bin/settings") && arguments.size() == 3) {
		command.arguments = {QStringLiteral("--setting"), arguments[1] + QStringLiteral("=") + arguments[2]};
	} else if (program == QStringLiteral("/bin/tee") && arguments.size() == 1 &&
	           arguments[0] == QStringLiteral("/proc/power-profile")) {
		command.arguments = {QStringLiteral("--power-profile"), QString::fromUtf8(input).trimmed()};
	} else if (program == QStringLiteral("/bin/setup")) {
		command.arguments = {QStringLiteral("--setup")};
		command.arguments.append(arguments);
	} else if (program == QStringLiteral("/bin/useradd") && arguments.size() == 4) {
		command.arguments = {QStringLiteral("--useradd"), arguments[3], arguments[2]};
	} else if (program == QStringLiteral("/bin/usermod") && arguments.size() == 3) {
		command.arguments = {QStringLiteral("--usermod-shell"), arguments[2], arguments[1]};
	} else if (program == QStringLiteral("/bin/passwd") && arguments.size() == 2) {
		command.arguments = {QStringLiteral("--passwd"), arguments[1]};
	} else {
		return false;
	}
	command.input = sudoPassword + "\n" + input;
	return true;
}

// Выполнить ограниченную привилегированную операцию с выводом ошибки
static bool runPrivileged(QWidget *parent, const QString &program, const QStringList &arguments,
                          const QByteArray &input, QString &output) {
	PrivilegedCommand prepared;
	if (!preparePrivileged(parent, program, arguments, input, prepared)) {
		return false;
	}
	QProcess command;
	command.setProcessChannelMode(QProcess::MergedChannels);
	command.start(prepared.program, prepared.arguments);
	if (!command.waitForStarted(5000)) {
		output = command.errorString();
		return false;
	}
	if (!prepared.input.isEmpty()) {
		command.write(prepared.input);
		command.closeWriteChannel();
	}
	prepared.input.fill(0);
	if (!command.waitForFinished(120000)) {
		command.kill();
		command.waitForFinished(1000);
		output = QStringLiteral("Превышено время ожидания операции");
		return false;
	}
	output = QString::fromUtf8(command.readAllStandardOutput()).trimmed();
	return command.exitStatus() == QProcess::NormalExit && command.exitCode() == 0;
}

// Сохранить настройку через тот же обработчик, что использует консоль
static bool writeSetting(QWidget *parent, const QString &key, const QString &value) {
	QString output;
	if (!runPrivileged(parent, QStringLiteral("/bin/settings"), {QStringLiteral("--set"), key, value}, {}, output)) {
		if (output.isEmpty()) {
			output = QStringLiteral("Не удалось сохранить настройку или подтвердить доступ");
		}
		QMessageBox::warning(parent, QStringLiteral("Настройки SynapseOS"), output);
		return false;
	}
	return true;
}

struct DesktopThemeSpec {
	const char *name;
	const char *background;
	const char *base;
	const char *surface;
	const char *foreground;
	const char *accent;
	bool light;
};

static const DesktopThemeSpec desktopThemes[] = {
	{"synapse-classic", "#f4f3ec", "#ffffff", "#e3e8e9", "#17333c", "#006f8f", true},
	{"Synapse-Midnight", "#10161e", "#151e28", "#223142", "#edf4fb", "#4c9aff", false},
	{"Synapse-Graphite", "#202124", "#292a2d", "#3c4043", "#f1f3f4", "#a8c7fa", false},
	{"Synapse-Ocean", "#102a34", "#153642", "#1e4b5a", "#ecfeff", "#38bdf8", false},
	{"Synapse-Light", "#f3f5f8", "#ffffff", "#e1e7ef", "#172033", "#2563eb", true},
	{"Synapse-Dracula", "#282a36", "#1e1f29", "#44475a", "#f8f8f2", "#bd93f9", false},
	{"Numix-Clone", "#303238", "#26282d", "#41434a", "#f3f4f5", "#d65d3a", false},
	{"Arc-Clone", "#e7e9ed", "#ffffff", "#d9e0e7", "#202a34", "#4388c5", true},
	{"Penumbra", "#243842", "#1b2e37", "#304b57", "#f3f4f5", "#769fb4", false},
	{"SynapseOS-Dark", "#1b2027", "#141920", "#29333d", "#f3f4f5", "#4388c5", false},
};

static const DesktopThemeSpec &desktopThemeSpec(const QString &name) {
	for (const auto &theme : desktopThemes) {
		if (name == QLatin1String(theme.name)) {
			return theme;
		}
	}
	return desktopThemes[0];
}

// Применить единую палитру и читаемые размеры элементов Qt
static void applyDesktopPalette(const QString &theme) {
	const auto &spec = desktopThemeSpec(theme);
	const QSettings preferences(QStringLiteral("SynapseOS"), QStringLiteral("Desktop"));
	auto color = [&preferences](const QString &name, const char *fallback) {
		const QString value = preferences.value(QStringLiteral("appearance/") + name,
		    QString::fromLatin1(fallback)).toString();
		return QColor(value).isValid() ? QColor(value).name() : QString::fromLatin1(fallback);
	};
	const QString background = color(QStringLiteral("background"), spec.background);
	const QString base = color(QStringLiteral("base"), spec.base);
	const QString surface = color(QStringLiteral("surface"), spec.surface);
	const QString foreground = color(QStringLiteral("foreground"), spec.foreground);
	const QString accent = color(QStringLiteral("accent"), spec.accent);
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
	const QColor disabled(spec.light ? QStringLiteral("#596872") : QStringLiteral("#9aa8b4"));
	palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
	palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
	palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
	palette.setColor(QPalette::PlaceholderText, QColor(spec.light ? QStringLiteral("#63717d") :
	                                                  QStringLiteral("#aab4be")));
	QApplication::setPalette(palette);
	QFont font = QApplication::font();
	font.setFamily(preferences.value(QStringLiteral("appearance/font-family"), font.family()).toString());
	font.setPointSize(preferences.value(QStringLiteral("appearance/font-size"), 10).toInt());
	QApplication::setFont(font);

	const QString radius = preferences.value(QStringLiteral("appearance/corner-radius"),
	    theme == QStringLiteral("synapse-classic") ? 0 : 7).toString();
	const QString padding = preferences.value(QStringLiteral("appearance/padding"), 6).toString();
	qApp->setStyleSheet(QStringLiteral(
	    "QMainWindow, QWidget, QDialog { background-color: %1; color: %4; }"
	    "QLineEdit, QListWidget, QComboBox, QSpinBox, QDoubleSpinBox, QDateTimeEdit, QPlainTextEdit, QTableWidget, QTreeView {"
	    " background-color: %2; color: %4; border: 1px solid %3; border-radius: %6px; padding: %7px; }"
	    "QListWidget::item { border-radius: %6px; padding: %7px; }"
	    "QListWidget::item:selected, QTableWidget::item:selected { background-color: %5; color: white; }"
	    "QPushButton, QToolButton { background-color: %3; color: %4; border: 1px solid %3;"
	    " border-radius: %6px; padding: %7px 12px; min-height: 20px; }"
	    "QPushButton:hover, QToolButton:hover { background-color: %5; color: white; }"
	    "QPushButton:disabled, QToolButton:disabled, QComboBox:disabled { color: #7f8b98; background-color: %1; }"
	    "QTabWidget::pane { border: 1px solid %3; border-radius: %6px; }"
	    "QTabBar::tab { background-color: %3; color: %4; padding: 9px 16px; border-top-left-radius: %6px;"
	    " border-top-right-radius: %6px; }"
	    "QTabBar::tab:selected { background-color: %5; color: white; }"
	    "QGroupBox { border: 1px solid %3; border-radius: %6px; margin-top: 12px; padding: 12px 8px 8px 8px; }"
	    "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 6px; font-weight: bold; }"
	    "QHeaderView::section { background-color: %3; color: %4; border: 0; padding: 7px; }"
	    "QProgressBar { background-color: %2; border: 1px solid %3; border-radius: %6px; text-align: center; }"
	    "QProgressBar::chunk { background-color: %5; border-radius: %6px; }"
	    "QWidget#settingsSidebar { background-color: %2; border-right: 1px solid %3; }"
	    "QListWidget#settingsSections { background-color: %2; border: 0; border-radius: 0; padding: 0; }"
	    "QListWidget#settingsSections::item { border-radius: %6px; padding: 7px 9px; }"
	    "QTabWidget#settingsPages, QScrollArea { border: 0; }"
	    "QWidget#notificationPopup { background-color: %2; border: 1px solid %5; border-radius: %6px; }"
	    "QToolTip { background-color: %2; color: %4; border: 1px solid %5; padding: 5px; }"
	    ).arg(background, base, surface, foreground, accent, radius, padding));
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
	QPointer<QMainWindow> settingsWindow;
	void openSettings();

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
				if (std::strcmp(selected.path, "/bin/synapse-desktop") == 0) {
					openSettings();
					return;
				}
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
		                    QStringLiteral("Мультимедиа"), QStringLiteral("Игры"), QStringLiteral("Flatpak")});
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
		connect(settings, &QPushButton::clicked, this, &MenuWindow::openSettings);
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
			    (entry.argument && std::strncmp(entry.argument, "--shell=", 8) == 0 &&
			     !QFileInfo(QString::fromUtf8(entry.argument + 8)).isExecutable()) ||
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
		if (std::strcmp(entry.path, "/bin/synapse-desktop") == 0 &&
		    (entry.argument == nullptr || std::strcmp(entry.argument, "--monitor") != 0)) {
			openSettings();
			return;
		}
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

// Прочитать небольшой системный отчёт без запуска внешней программы
static QByteArray monitorFile(const QString &path, qint64 limit = 4 * 1024 * 1024) {
	QFile file(path);
	return file.open(QIODevice::ReadOnly) ? file.read(limit) : QByteArray();
}

// Представить байты в удобных единицах измерения
static QString formatByteCount(qulonglong bytes) {
	if (bytes >= 1024ULL * 1024 * 1024 * 1024) {
		return QStringLiteral("%1 ТиБ").arg(bytes / (1024.0 * 1024 * 1024 * 1024), 0, 'f', 1);
	}
	if (bytes >= 1024ULL * 1024 * 1024) {
		return QStringLiteral("%1 ГиБ").arg(bytes / (1024.0 * 1024 * 1024), 0, 'f', 1);
	}
	if (bytes >= 1024ULL * 1024) {
		return QStringLiteral("%1 МиБ").arg(bytes / (1024.0 * 1024), 0, 'f', 1);
	}
	return QStringLiteral("%1 КиБ").arg(bytes / 1024.0, 0, 'f', 1);
}

// Сравнивать числовые значения таблицы без лексикографического порядка строк
class NumericTableItem final : public QTableWidgetItem {
public:
	NumericTableItem(const QString &text, double value) : QTableWidgetItem(text), value_(value) {}

	bool operator<(const QTableWidgetItem &other) const override {
		const auto *numeric = dynamic_cast<const NumericTableItem *>(&other);
		return numeric ? value_ < numeric->value_ : QTableWidgetItem::operator<(other);
	}

private:
	double value_;
};

// График последних показаний системного монитора
class MetricGraph final : public QWidget {
public:
	explicit MetricGraph(const QString &title, QWidget *parent = nullptr) : QWidget(parent), title_(title) {
		setMinimumHeight(150);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	}

	void addValue(double value) {
		values_.append(qBound(0.0, value, 100.0));
		while (values_.size() > 120) {
			values_.removeFirst();
		}
		update();
	}

protected:
	void paintEvent(QPaintEvent *event) override {
		Q_UNUSED(event);
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		const QRectF area = rect().adjusted(12, 32, -12, -24);
		painter.fillRect(rect(), palette().color(QPalette::Base));
		painter.setPen(palette().color(QPalette::WindowText));
		QFont font = painter.font();
		font.setBold(true);
		painter.setFont(font);
		painter.drawText(14, 22, title_);
		painter.setPen(palette().color(QPalette::Mid));
		for (int line = 0; line <= 4; ++line) {
			const qreal y = area.top() + area.height() * line / 4;
			painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
		}
		painter.drawText(area.left(), height() - 7, QStringLiteral("2 минуты назад"));
		painter.drawText(area.right() - 45, height() - 7, QStringLiteral("сейчас"));
		if (values_.isEmpty()) {
			return;
		}
		QPainterPath path;
		for (int index = 0; index < values_.size(); ++index) {
			const qreal x = area.left() + (values_.size() == 1 ? 0 :
			    area.width() * index / (values_.size() - 1));
			const qreal y = area.bottom() - area.height() * values_[index] / 100;
			if (index == 0) {
				path.moveTo(x, y);
			} else {
				path.lineTo(x, y);
			}
		}
		painter.setPen(QPen(QColor(QStringLiteral("#4da3ff")), 2));
		painter.drawPath(path);
		painter.drawText(area.right() - 65, area.top() + 18,
		    QStringLiteral("%1%").arg(values_.last(), 0, 'f', 1));
	}

private:
	QString title_;
	QVector<double> values_;
};

// Стандартный сервис уведомлений рабочего стола без генератора кода D-Bus
class DesktopNotifications final : public QDBusVirtualObject {
public:
	explicit DesktopNotifications(QObject *parent = nullptr) : QDBusVirtualObject(parent) {}

	bool start() {
		auto bus = QDBusConnection::sessionBus();
		if (!bus.isConnected() || !bus.registerVirtualObject(QStringLiteral("/org/freedesktop/Notifications"), this)) {
			return false;
		}
		if (!bus.registerService(QStringLiteral("org.freedesktop.Notifications"))) {
			bus.unregisterObject(QStringLiteral("/org/freedesktop/Notifications"));
			return false;
		}
		return true;
	}

	QString introspect(const QString &) const override {
		return QStringLiteral(
		    "<node><interface name='org.freedesktop.Notifications'>"
		    "<method name='Notify'><arg type='s'/><arg type='u'/><arg type='s'/><arg type='s'/><arg type='s'/>"
		    "<arg type='as'/><arg type='a{sv}'/><arg type='i'/><arg type='u' direction='out'/></method>"
		    "<method name='CloseNotification'><arg type='u'/></method>"
		    "<method name='GetCapabilities'><arg type='as' direction='out'/></method>"
		    "<method name='GetServerInformation'><arg type='s' direction='out'/><arg type='s' direction='out'/>"
		    "<arg type='s' direction='out'/><arg type='s' direction='out'/></method>"
		    "<signal name='NotificationClosed'><arg type='u'/><arg type='u'/></signal>"
		    "<signal name='ActionInvoked'><arg type='u'/><arg type='s'/></signal>"
		    "</interface></node>");
	}

	bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override {
		const QString method = message.member();
		const QVariantList arguments = message.arguments();
		if (method == QStringLiteral("GetCapabilities")) {
			connection.send(message.createReply(QVariantList{QStringList{QStringLiteral("actions"), QStringLiteral("body"),
			    QStringLiteral("body-plaintext"), QStringLiteral("persistence")}}));
			return true;
		}
		if (method == QStringLiteral("GetServerInformation")) {
			connection.send(message.createReply(QVariantList{QStringLiteral("SynapseOS"),
			    QStringLiteral("SynapseOS"), QStringLiteral("1.0"), QStringLiteral("1.2")}));
			return true;
		}
		if (method == QStringLiteral("CloseNotification") && !arguments.isEmpty()) {
			close(arguments[0].toUInt(), 3, connection);
			connection.send(message.createReply());
			return true;
		}
		if (method != QStringLiteral("Notify") || arguments.size() < 8) {
			return false;
		}
		const quint32 id = arguments[1].toUInt() ? arguments[1].toUInt() : nextId_++;
		show(id, arguments[3].toString(), arguments[4].toString(), arguments[5].toStringList(),
		     arguments[7].toInt(), connection);
		connection.send(message.createReply(QVariantList{id}));
		return true;
	}

private:
	quint32 nextId_ = 1;
	QHash<quint32, QPointer<QWidget>> popups_;

	void close(quint32 id, quint32 reason, const QDBusConnection &connection) {
		if (QWidget *popup = popups_.take(id)) {
			popup->close();
		}
		auto signal = QDBusMessage::createSignal(QStringLiteral("/org/freedesktop/Notifications"),
		    QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("NotificationClosed"));
		signal.setArguments({id, reason});
		connection.send(signal);
	}

	void show(quint32 id, const QString &title, const QString &body, const QStringList &actions,
	          int timeout, const QDBusConnection &connection) {
		QSettings preferences(QStringLiteral("SynapseOS"), QStringLiteral("Desktop"));
		QStringList history = preferences.value(QStringLiteral("notifications/history")).toStringList();
		history.prepend(QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") +
		                title + QStringLiteral(": ") + body);
		while (history.size() > 50) {
			history.removeLast();
		}
		preferences.setValue(QStringLiteral("notifications/history"), history);
		if (!preferences.value(QStringLiteral("notifications/enabled"), true).toBool() ||
		    preferences.value(QStringLiteral("notifications/quiet"), false).toBool()) {
			return;
		}
		if (QWidget *previous = popups_.take(id)) {
			previous->close();
		}
		auto *popup = new QFrame(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
		popup->setObjectName(QStringLiteral("notificationPopup"));
		popup->setWindowTitle(QStringLiteral("Synapse Notification"));
		popup->setMinimumSize(390, 120);
		popup->setAttribute(Qt::WA_DeleteOnClose);
		auto *layout = new QVBoxLayout(popup);
		layout->setContentsMargins(14, 12, 14, 12);
		auto *header = new QHBoxLayout();
		auto *icon = new QLabel(popup);
		icon->setPixmap(QApplication::style()->standardIcon(QStyle::SP_MessageBoxInformation).pixmap(32, 32));
		header->addWidget(icon);
		auto *heading = new QLabel(title.isEmpty() ? QStringLiteral("SynapseOS") : title, popup);
		QFont font = heading->font();
		font.setBold(true);
		font.setPointSize(font.pointSize() + 1);
		heading->setFont(font);
		header->addWidget(heading, 1);
		auto *time = new QLabel(QTime::currentTime().toString(QStringLiteral("HH:mm")), popup);
		header->addWidget(time);
		auto *dismiss = new QToolButton(popup);
		dismiss->setText(QStringLiteral("×"));
		dismiss->setToolTip(QStringLiteral("Закрыть уведомление"));
		dismiss->setFixedSize(28, 28);
		header->addWidget(dismiss);
		layout->addLayout(header);
		auto *text = new QLabel(body, popup);
		text->setWordWrap(true);
		text->setMaximumWidth(360);
		text->setTextInteractionFlags(Qt::TextSelectableByMouse);
		layout->addWidget(text);
		connect(dismiss, &QToolButton::clicked, popup, [this, id, connection]() { close(id, 2, connection); });

		if (actions.size() > 1) {
			auto *buttons = new QHBoxLayout();
			buttons->addStretch();
			for (int index = 0; index + 1 < actions.size(); index += 2) {
				auto *action = new QPushButton(actions[index + 1], popup);
				buttons->addWidget(action);
				const QString actionId = actions[index];
				connect(action, &QPushButton::clicked, popup, [this, id, actionId, connection]() {
					auto signal = QDBusMessage::createSignal(QStringLiteral("/org/freedesktop/Notifications"),
					    QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("ActionInvoked"));
					signal.setArguments({id, actionId});
					connection.send(signal);
					close(id, 2, connection);
				});
			}
			layout->addLayout(buttons);
		}
		auto *progress = new QProgressBar(popup);
		progress->setRange(0, 1000);
		progress->setValue(1000);
		progress->setTextVisible(false);
		progress->setMaximumHeight(4);
		layout->addWidget(progress);
		popup->adjustSize();
		const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
		popup->move(available.right() - popup->width() - 24, available.bottom() - popup->height() - 24);
		popups_.insert(id, popup);
		popup->show();
		const int delay = timeout > 0 ? timeout :
		    preferences.value(QStringLiteral("notifications/duration"), 7).toInt() * 1000;
		auto *animation = new QTimer(popup);
		animation->setInterval(50);
		connect(animation, &QTimer::timeout, popup,
		    [this, id, connection, animation, progress, delay, elapsed = 0]() mutable {
			elapsed += 50;
			progress->setValue(qMax(0, 1000 - elapsed * 1000 / delay));
			if (elapsed >= delay) {
				animation->stop();
				close(id, 1, connection);
			}
		});
		animation->start();
	}
};

// Наглядный монитор процессов, памяти, накопителей и сети
class MonitorWindow final : public QMainWindow {
public:
	MonitorWindow() {
		setWindowTitle(QStringLiteral("Системный монитор SynapseOS"));
		resize(1120, 720);
		setMinimumSize(800, 520);
		auto *central = new QWidget(this);
		auto *layout = new QVBoxLayout(central);
		auto *controls = new QHBoxLayout();
		auto *title = new QLabel(QStringLiteral("Системный монитор"), central);
		QFont font = title->font();
		font.setPointSize(font.pointSize() + 5);
		font.setBold(true);
		title->setFont(font);
		controls->addWidget(title);
		controls->addStretch();
		controls->addWidget(new QLabel(QStringLiteral("Интервал"), central));
		auto *interval = new QComboBox(central);
		interval->addItem(QStringLiteral("1 секунда"), 1000);
		interval->addItem(QStringLiteral("2 секунды"), 2000);
		interval->addItem(QStringLiteral("5 секунд"), 5000);
		controls->addWidget(interval);
		auto *refreshButton = new QPushButton(QStringLiteral("Обновить"), central);
		controls->addWidget(refreshButton);
		layout->addLayout(controls);
		tabs_ = new QTabWidget(central);
		layout->addWidget(tabs_, 1);
		buildOverview();
		buildProcesses();
		buildPerformance();
		buildStorage();
		buildNetwork();
		buildSubsystems();
		setCentralWidget(central);
		timer_ = new QTimer(this);
		connect(timer_, &QTimer::timeout, this, &MonitorWindow::refresh);
		connect(interval, &QComboBox::currentIndexChanged, this, [this, interval]() {
			timer_->start(interval->currentData().toInt());
		});
		connect(refreshButton, &QPushButton::clicked, this, &MonitorWindow::refresh);
		refresh();
		timer_->start(1000);
	}

	bool ready() const {
		return tabs_ && tabs_->count() == 6 && processTable_ && processTable_->columnCount() == 8;
	}

private:
	QTabWidget *tabs_ = nullptr;
	QTimer *timer_ = nullptr;
	QLabel *cpuLabel_ = nullptr;
	QLabel *memoryLabel_ = nullptr;
	QLabel *systemLabel_ = nullptr;
	MetricGraph *cpuGraph_ = nullptr;
	MetricGraph *memoryGraph_ = nullptr;
	QTableWidget *processTable_ = nullptr;
	QLineEdit *processSearch_ = nullptr;
	QPlainTextEdit *cpuDetails_ = nullptr;
	QTableWidget *coreTable_ = nullptr;
	QTableWidget *storageTable_ = nullptr;
	QTableWidget *networkTable_ = nullptr;
	QPlainTextEdit *subsystems_ = nullptr;
	quint64 previousTotal_ = 0;
	quint64 previousIdle_ = 0;
	QHash<int, quint64> processTimes_;
	QHash<QString, quint64> networkTimes_;

	static QTableWidget *makeTable(QWidget *parent, const QStringList &columns) {
		auto *table = new QTableWidget(parent);
		table->setColumnCount(columns.size());
		table->setHorizontalHeaderLabels(columns);
		table->verticalHeader()->hide();
		table->setAlternatingRowColors(true);
		table->setShowGrid(false);
		table->setSelectionBehavior(QAbstractItemView::SelectRows);
		table->setSelectionMode(QAbstractItemView::SingleSelection);
		table->setEditTriggers(QAbstractItemView::NoEditTriggers);
		table->setSortingEnabled(true);
		table->horizontalHeader()->setStretchLastSection(true);
		return table;
	}

	void buildOverview() {
		auto *page = new QWidget(tabs_);
		auto *layout = new QVBoxLayout(page);
		systemLabel_ = new QLabel(page);
		systemLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
		layout->addWidget(systemLabel_);
		auto *graphs = new QHBoxLayout();
		auto *cpuBox = new QGroupBox(QStringLiteral("Процессор"), page);
		auto *cpuLayout = new QVBoxLayout(cpuBox);
		cpuLabel_ = new QLabel(cpuBox);
		cpuLayout->addWidget(cpuLabel_);
		cpuGraph_ = new MetricGraph(QStringLiteral("Загрузка CPU"), cpuBox);
		cpuLayout->addWidget(cpuGraph_);
		graphs->addWidget(cpuBox, 1);
		auto *memoryBox = new QGroupBox(QStringLiteral("Память"), page);
		auto *memoryLayout = new QVBoxLayout(memoryBox);
		memoryLabel_ = new QLabel(memoryBox);
		memoryLayout->addWidget(memoryLabel_);
		memoryGraph_ = new MetricGraph(QStringLiteral("Использование ОЗУ"), memoryBox);
		memoryLayout->addWidget(memoryGraph_);
		graphs->addWidget(memoryBox, 1);
		layout->addLayout(graphs, 1);
		tabs_->addTab(page, QStringLiteral("Обзор"));
	}

	void buildProcesses() {
		auto *page = new QWidget(tabs_);
		auto *layout = new QVBoxLayout(page);
		auto *controls = new QHBoxLayout();
		processSearch_ = new QLineEdit(page);
		processSearch_->setPlaceholderText(QStringLiteral("Поиск по PID, имени или пользователю"));
		processSearch_->setClearButtonEnabled(true);
		controls->addWidget(processSearch_, 1);
		auto *terminate = new QPushButton(QStringLiteral("Завершить процесс"), page);
		controls->addWidget(terminate);
		layout->addLayout(controls);
		processTable_ = makeTable(page, {QStringLiteral("PID"), QStringLiteral("Пользователь"),
		    QStringLiteral("Процесс"), QStringLiteral("Состояние"), QStringLiteral("CPU, %"),
		    QStringLiteral("ОЗУ"), QStringLiteral("Потоки"), QStringLiteral("Виртуальная память")});
		processTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
		layout->addWidget(processTable_, 1);
		connect(processSearch_, &QLineEdit::textChanged, this, &MonitorWindow::refreshProcesses);
		connect(processTable_, &QTableWidget::itemActivated, this, [this](QTableWidgetItem *item) {
			showProcessDetails(item->text().toInt());
		});
		connect(terminate, &QPushButton::clicked, this, [this]() {
			const int row = processTable_->currentRow();
			if (row < 0) {
				return;
			}
			const int pid = processTable_->item(row, 0)->text().toInt();
			const QString name = processTable_->item(row, 2)->text();
			if (pid <= 1 || QMessageBox::question(this, QStringLiteral("Завершение процесса"),
			    QStringLiteral("Отправить SIGTERM процессу %1 (%2)?").arg(name).arg(pid)) != QMessageBox::Yes) {
				return;
			}
			if (kill(pid, SIGTERM) != 0) {
				QMessageBox::warning(this, QStringLiteral("Завершение процесса"),
				    QString::fromLocal8Bit(strerror(errno)));
			}
			QTimer::singleShot(300, this, &MonitorWindow::refreshProcesses);
		});
		tabs_->addTab(page, QStringLiteral("Процессы"));
	}

	// Показать подробную карточку процесса без переменных окружения и секретов
	void showProcessDetails(int pid) {
		if (pid <= 0) {
			return;
		}
		const QString base = QStringLiteral("/proc/%1").arg(pid);
		auto *dialog = new QDialog(this);
		dialog->setAttribute(Qt::WA_DeleteOnClose);
		dialog->setWindowTitle(QStringLiteral("Процесс %1").arg(pid));
		dialog->resize(760, 560);
		auto *layout = new QFormLayout(dialog);
		auto add = [dialog, layout](const QString &name, const QString &value) {
			auto *label = new QLabel(value.isEmpty() ? QStringLiteral("Недоступно") : value, dialog);
			label->setWordWrap(true);
			label->setTextInteractionFlags(Qt::TextSelectableByMouse);
			layout->addRow(name, label);
		};
		add(QStringLiteral("Исполняемый файл"), QFileInfo(base + QStringLiteral("/exe")).symLinkTarget());
		add(QStringLiteral("Рабочий каталог"), QFileInfo(base + QStringLiteral("/cwd")).symLinkTarget());
		add(QStringLiteral("Командная строка"),
		    QString::fromUtf8(monitorFile(base + QStringLiteral("/cmdline")).replace('\0', ' ')).trimmed());
		add(QStringLiteral("Состояние и память"), QString::fromUtf8(monitorFile(base + QStringLiteral("/status"))).trimmed());
		add(QStringLiteral("Ожидание"), QString::fromUtf8(monitorFile(base + QStringLiteral("/wchan"))).trimmed());
		add(QStringLiteral("Дисковый I/O"), QString::fromUtf8(monitorFile(base + QStringLiteral("/io"))).trimmed());
		dialog->show();
	}

	void buildPerformance() {
		auto *page = new QWidget(tabs_);
		auto *layout = new QVBoxLayout(page);
		coreTable_ = makeTable(page, {QStringLiteral("Процессор"), QStringLiteral("User"),
		    QStringLiteral("System"), QStringLiteral("Idle"), QStringLiteral("I/O wait")});
		coreTable_->setMaximumHeight(250);
		layout->addWidget(coreTable_);
		cpuDetails_ = new QPlainTextEdit(page);
		cpuDetails_->setReadOnly(true);
		layout->addWidget(cpuDetails_, 1);
		tabs_->addTab(page, QStringLiteral("Ресурсы"));
	}

	void buildStorage() {
		auto *page = new QWidget(tabs_);
		auto *layout = new QVBoxLayout(page);
		storageTable_ = makeTable(page, {QStringLiteral("Устройство"), QStringLiteral("Точка монтирования"),
		    QStringLiteral("Файловая система"), QStringLiteral("Размер"), QStringLiteral("Занято"),
		    QStringLiteral("Свободно"), QStringLiteral("Использовано")});
		storageTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
		layout->addWidget(storageTable_, 1);
		tabs_->addTab(page, QStringLiteral("Диски"));
	}

	void buildNetwork() {
		auto *page = new QWidget(tabs_);
		auto *layout = new QVBoxLayout(page);
		networkTable_ = makeTable(page, {QStringLiteral("Интерфейс"), QStringLiteral("Принято"),
		    QStringLiteral("Приём, КиБ/с"), QStringLiteral("Отправлено"), QStringLiteral("Передача, КиБ/с"),
		    QStringLiteral("Пакеты RX/TX"), QStringLiteral("Ошибки RX/TX")});
		layout->addWidget(networkTable_, 1);
		tabs_->addTab(page, QStringLiteral("Сеть"));
	}

	void buildSubsystems() {
		auto *page = new QWidget(tabs_);
		auto *layout = new QVBoxLayout(page);
		subsystems_ = new QPlainTextEdit(page);
		subsystems_->setReadOnly(true);
		layout->addWidget(subsystems_, 1);
		tabs_->addTab(page, QStringLiteral("Подсистемы и оборудование"));
	}

	static QHash<QByteArray, qulonglong> memoryValues() {
		QHash<QByteArray, qulonglong> values;
		for (const QByteArray &line : monitorFile(QStringLiteral("/proc/meminfo")).split('\n')) {
			const QList<QByteArray> fields = line.simplified().split(' ');
			if (fields.size() >= 2) {
				values.insert(fields[0], fields[1].toULongLong() * 1024);
			}
		}
		return values;
	}

	void refresh() {
		quint64 total = 0, idle = 0;
		const QList<QByteArray> rows = monitorFile(QStringLiteral("/proc/stat")).split('\n');
		if (!rows.isEmpty()) {
			const QList<QByteArray> fields = rows[0].simplified().split(' ');
			for (int index = 1; index < fields.size(); ++index) {
				total += fields[index].toULongLong();
			}
			if (fields.size() > 4) {
				idle = fields[4].toULongLong() + fields.value(5).toULongLong();
			}
		}
		double cpu = 0;
		if (previousTotal_ && total > previousTotal_) {
			cpu = 100.0 * (total - previousTotal_ - (idle - previousIdle_)) / (total - previousTotal_);
		}
		previousTotal_ = total;
		previousIdle_ = idle;
		cpuGraph_->addValue(cpu);
		cpuLabel_->setText(QStringLiteral("Общая загрузка: %1%").arg(cpu, 0, 'f', 1));
		const auto memory = memoryValues();
		const qulonglong ram = memory.value("MemTotal:");
		const qulonglong available = memory.value("MemAvailable:");
		const double used = ram ? 100.0 * (ram - available) / ram : 0;
		memoryGraph_->addValue(used);
		const qulonglong kernel = memory.value("KernelHeap:") + memory.value("PageTables:") +
		    memory.value("Drivers:") + memory.value("OtherUsed:");
		memoryLabel_->setText(QStringLiteral(
		    "Занято %1 из %2, доступно %3\nПроцессы: %4  |  Ядро и драйверы: %5\nКэш: %6  |  Общие страницы: %7\nSwap: %8")
		    .arg(formatByteCount(ram - available), formatByteCount(ram), formatByteCount(available))
		    .arg(formatByteCount(memory.value("UserAnon:")), formatByteCount(kernel))
		    .arg(formatByteCount(memory.value("Cached:") + memory.value("Buffers:")),
		         formatByteCount(memory.value("Shmem:")),
		         formatByteCount(memory.value("SwapTotal:") - memory.value("SwapFree:"))));
		unsigned cores = 0;
		QByteArray model;
		for (const QByteArray &line : monitorFile(QStringLiteral("/proc/cpuinfo")).split('\n')) {
			if (line.startsWith("processor")) {
				++cores;
			}
			if (line.startsWith("model name")) {
				model = line.mid(line.indexOf(':') + 1).trimmed();
			}
		}
		const QStringList uptime = QString::fromUtf8(monitorFile(QStringLiteral("/proc/uptime"))).split(QLatin1Char(' '));
		const double seconds = uptime.value(0).toDouble();
		systemLabel_->setText(QStringLiteral("%1\nЛогических CPU: %2  |  Время работы: %3 ч %4 мин  |  Процессов: %5")
		    .arg(QString::fromUtf8(model)).arg(cores).arg(static_cast<int>(seconds / 3600))
		    .arg(static_cast<int>(seconds) / 60 % 60).arg(processTable_->rowCount()));
		refreshProcesses();
		refreshCores(rows);
		refreshStorage();
		refreshNetwork();
		QStringList state;
		for (const QString &path : {QStringLiteral("/proc/power-profile"), QStringLiteral("/proc/media"),
		     QStringLiteral("/proc/storage"), QStringLiteral("/proc/thermal"),
		     QStringLiteral("/synapse/subsys/android/enabled"),
		     QStringLiteral("/synapse/subsys/android/api-profile")}) {
			const QByteArray data = monitorFile(path, 65536);
			if (!data.isEmpty()) {
				state << QStringLiteral("=== %1 ===").arg(path) << QString::fromUtf8(data).trimmed();
			}
		}
		subsystems_->setPlainText(state.join(QStringLiteral("\n\n")));
	}

	void refreshProcesses() {
		const QString query = processSearch_->text().trimmed();
		QSet<int> running;
		const QDir proc(QStringLiteral("/proc"));
		const long pageSize = sysconf(_SC_PAGE_SIZE);
		const long ticks = sysconf(_SC_CLK_TCK);
		const quint64 elapsed = previousTotal_;
		QHash<int, quint64> currentTimes;
		processTable_->setSortingEnabled(false);
		processTable_->setRowCount(0);
		for (const QString &entry : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
			bool number = false;
			const int pid = entry.toInt(&number);
			if (!number || pid <= 0) {
				continue;
			}
			const QByteArray stat = monitorFile(QStringLiteral("/proc/%1/stat").arg(pid));
			const int end = stat.lastIndexOf(") ");
			const int begin = stat.indexOf('(');
			if (begin < 0 || end < begin) {
				continue;
			}
			const QList<QByteArray> fields = stat.mid(end + 2).simplified().split(' ');
			if (fields.size() < 22) {
				continue;
			}
			const quint64 work = fields[11].toULongLong() + fields[12].toULongLong();
			currentTimes.insert(pid, work);
			running.insert(pid);
			double cpu = 0;
			if (processTimes_.contains(pid) && elapsed > 0 && ticks > 0) {
				cpu = 100.0 * (work - processTimes_.value(pid)) / elapsed;
			}
			const QString name = QString::fromUtf8(stat.mid(begin + 1, end - begin - 1));
			const QString user = QFileInfo(QStringLiteral("/proc/%1").arg(pid)).owner();
			if (!query.isEmpty() && !entry.contains(query) && !name.contains(query, Qt::CaseInsensitive) &&
			    !user.contains(query, Qt::CaseInsensitive)) {
				continue;
			}
			const int row = processTable_->rowCount();
			processTable_->insertRow(row);
			processTable_->setItem(row, 0, new NumericTableItem(entry, pid));
			processTable_->setItem(row, 1, new QTableWidgetItem(user));
			processTable_->setItem(row, 2, new QTableWidgetItem(name));
			processTable_->setItem(row, 3, new QTableWidgetItem(QString::fromLatin1(fields[0])));
			processTable_->setItem(row, 4, new NumericTableItem(QString::number(cpu, 'f', 1), cpu));
			processTable_->setItem(row, 5, new NumericTableItem(formatByteCount(fields[21].toULongLong() * pageSize),
			    fields[21].toULongLong() * pageSize));
			processTable_->setItem(row, 6, new NumericTableItem(QString::fromUtf8(fields[17]), fields[17].toInt()));
			processTable_->setItem(row, 7, new NumericTableItem(formatByteCount(fields[20].toULongLong()),
			    fields[20].toULongLong()));
		}
		processTimes_ = currentTimes;
		processTable_->setSortingEnabled(true);
	}

	void refreshCores(const QList<QByteArray> &rows) {
		coreTable_->setRowCount(0);
		for (const QByteArray &row : rows) {
			if (!row.startsWith("cpu") || row.startsWith("cpu ")) {
				continue;
			}
			const QList<QByteArray> fields = row.simplified().split(' ');
			if (fields.size() < 6) {
				continue;
			}
			const int index = coreTable_->rowCount();
			coreTable_->insertRow(index);
			for (int column = 0; column < 5; ++column) {
				coreTable_->setItem(index, column, new QTableWidgetItem(QString::fromUtf8(fields[column])));
			}
		}
		cpuDetails_->setPlainText(QString::fromUtf8(monitorFile(QStringLiteral("/proc/cpuinfo"))));
	}

	void refreshStorage() {
		storageTable_->setRowCount(0);
		for (const QStorageInfo &volume : QStorageInfo::mountedVolumes()) {
			if (!volume.isReady() || volume.bytesTotal() <= 0) {
				continue;
			}
			const int row = storageTable_->rowCount();
			storageTable_->insertRow(row);
			const qulonglong total = volume.bytesTotal();
			const qulonglong available = volume.bytesAvailable();
			storageTable_->setItem(row, 0, new QTableWidgetItem(QString::fromUtf8(volume.device())));
			storageTable_->setItem(row, 1, new QTableWidgetItem(volume.rootPath()));
			storageTable_->setItem(row, 2, new QTableWidgetItem(QString::fromUtf8(volume.fileSystemType())));
			storageTable_->setItem(row, 3, new QTableWidgetItem(formatByteCount(total)));
			storageTable_->setItem(row, 4, new QTableWidgetItem(formatByteCount(total - available)));
			storageTable_->setItem(row, 5, new QTableWidgetItem(formatByteCount(available)));
			auto *bar = new QProgressBar(storageTable_);
			bar->setRange(0, 100);
			bar->setValue(static_cast<int>((total - available) * 100 / total));
			storageTable_->setCellWidget(row, 6, bar);
		}
	}

	void refreshNetwork() {
		const QList<QByteArray> rows = monitorFile(QStringLiteral("/proc/net/dev")).split('\n').mid(2);
		networkTable_->setRowCount(0);
		QHash<QString, quint64> current;
		for (const QByteArray &row : rows) {
			const int colon = row.indexOf(':');
			if (colon < 0) {
				continue;
			}
			const QString name = QString::fromUtf8(row.left(colon).trimmed());
			const QList<QByteArray> fields = row.mid(colon + 1).simplified().split(' ');
			if (fields.size() < 16) {
				continue;
			}
			const quint64 rx = fields[0].toULongLong();
			const quint64 tx = fields[8].toULongLong();
			const QString rxKey = name + QStringLiteral(":rx");
			const QString txKey = name + QStringLiteral(":tx");
			const double seconds = qMax(1, timer_->interval()) / 1000.0;
			const double rxRate = networkTimes_.contains(rxKey) ? (rx - networkTimes_.value(rxKey)) / seconds : 0;
			const double txRate = networkTimes_.contains(txKey) ? (tx - networkTimes_.value(txKey)) / seconds : 0;
			current.insert(rxKey, rx);
			current.insert(txKey, tx);
			const int index = networkTable_->rowCount();
			networkTable_->insertRow(index);
			networkTable_->setItem(index, 0, new QTableWidgetItem(name));
			networkTable_->setItem(index, 1, new QTableWidgetItem(formatByteCount(rx)));
			networkTable_->setItem(index, 2, new QTableWidgetItem(QString::number(rxRate / 1024.0, 'f', 1)));
			networkTable_->setItem(index, 3, new QTableWidgetItem(formatByteCount(tx)));
			networkTable_->setItem(index, 4, new QTableWidgetItem(QString::number(txRate / 1024.0, 'f', 1)));
			networkTable_->setItem(index, 5, new QTableWidgetItem(QStringLiteral("%1 / %2")
			    .arg(fields[1].toULongLong()).arg(fields[9].toULongLong())));
			networkTable_->setItem(index, 6, new QTableWidgetItem(QStringLiteral("%1 / %2")
			    .arg(fields[2].toULongLong()).arg(fields[10].toULongLong())));
		}
		networkTimes_ = current;
	}
};

class SettingsWindow final : public QMainWindow {
public:
	// Построить графические разделы поверх существующей программы settings
	explicit SettingsWindow(const QString &subsystem = {}) {
		setWindowTitle(QStringLiteral("Настройки SynapseOS"));
		setWindowIcon(QIcon(QStringLiteral("/usr/share/weston/settings-icon.png")));
		resize(1040, 680);
		setMinimumSize(760, 480);

		auto *content = new QWidget(this);
		auto *layout = new QHBoxLayout(content);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(0);
		auto *sidebar = new QWidget(content);
		sidebar->setObjectName(QStringLiteral("settingsSidebar"));
		sidebar->setFixedWidth(QSettings(QStringLiteral("SynapseOS"), QStringLiteral("Desktop"))
		    .value(QStringLiteral("appearance/sidebar-width"), 250).toInt());
		auto *sideLayout = new QVBoxLayout(sidebar);
		sideLayout->setContentsMargins(14, 14, 10, 14);
		sideLayout->setSpacing(10);
		auto *search = new QLineEdit(sidebar);
		search->setPlaceholderText(QStringLiteral("Поиск..."));
		search->setClearButtonEnabled(true);
		sideLayout->addWidget(search);
		sections_ = new QListWidget(sidebar);
		sections_->setObjectName(QStringLiteral("settingsSections"));
		sections_->setAccessibleName(QStringLiteral("Разделы настроек"));
		sections_->setUniformItemSizes(true);
		sideLayout->addWidget(sections_, 1);
		layout->addWidget(sidebar);
		tabs_ = new QTabWidget(content);
		tabs_->setObjectName(QStringLiteral("settingsPages"));
		auto *tabs = tabs_;
		tabs->tabBar()->hide();
		layout->addWidget(tabs, 1);
		setCentralWidget(content);
		addDisplayPage(tabs);
		addOnDemand(tabs, QStringLiteral("Оформление"), &SettingsWindow::buildAppearancePage);
		addOnDemand(tabs, QStringLiteral("Уведомления"), &SettingsWindow::buildNotificationsPage);
		addOnDemand(tabs, QStringLiteral("Терминал"), &SettingsWindow::buildTerminalPage);
		addOnDemand(tabs, QStringLiteral("Система"), &SettingsWindow::buildSystemPage);
		addOnDemand(tabs, QStringLiteral("Пользователи"), &SettingsWindow::buildUsersPage);
		addOnDemand(tabs, QStringLiteral("Память"), &SettingsWindow::buildMemoryPage);
		addOnDemand(tabs, QStringLiteral("Дата и время"), &SettingsWindow::buildDateTimePage);
		addOnDemand(tabs, QStringLiteral("Обновления"), &SettingsWindow::buildUpdatesPage);
		addOnDemand(tabs, QStringLiteral("Synapse Plus"), &SettingsWindow::buildPlusPage);
		addOnDemand(tabs, QStringLiteral("Службы"), &SettingsWindow::buildServicesPage);
		addOnDemand(tabs, QStringLiteral("Приватность"), &SettingsWindow::buildPrivacyPage);
		discoverSubsystems(tabs, subsystem);
		addOnDemand(tabs, QStringLiteral("Сеть"), &SettingsWindow::buildNetworkPage);
		addOnDemand(tabs, QStringLiteral("Локальная сеть"), &SettingsWindow::buildLanPage);
		addWifiPage(tabs);
		addOnDemand(tabs, QStringLiteral("Электропитание"), &SettingsWindow::buildPowerPage);
		addOnDemand(tabs, QStringLiteral("Звук"), &SettingsWindow::buildSoundPage);
		addOnDemand(tabs, QStringLiteral("Отчёт"), &SettingsWindow::buildReportPage);
		addOnDemand(tabs, QStringLiteral("О системе"), &SettingsWindow::buildAboutPage);
		QString currentGroup;
		for (int index = 0; index < tabs->count(); ++index) {
			const QString group = sectionGroup(tabs->tabText(index));
			if (group != currentGroup) {
				currentGroup = group;
				addSectionGroup(group);
			}
			addSectionItem(tabs->tabText(index), index);
		}
		filterSections(QString());
		for (int row = 0; row < sections_->count(); ++row) {
			if (sections_->item(row)->data(Qt::UserRole).toInt() == tabs->currentIndex() &&
			    sections_->item(row)->flags() & Qt::ItemIsEnabled) {
				sections_->setCurrentRow(row);
				break;
			}
		}
		connect(sections_, &QListWidget::currentRowChanged, tabs, [this](int row) {
			auto *item = sections_->item(row);
			if (item && item->flags() & Qt::ItemIsEnabled) {
				tabs_->setCurrentIndex(item->data(Qt::UserRole).toInt());
			}
		});
		connect(tabs, &QTabWidget::currentChanged, this, [this](int index) {
			for (int row = 0; row < sections_->count(); ++row) {
				auto *item = sections_->item(row);
				if ((item->flags() & Qt::ItemIsEnabled) && item->data(Qt::UserRole).toInt() == index) {
					sections_->setCurrentRow(row);
					break;
				}
			}
		});
		connect(search, &QLineEdit::textChanged, this, &SettingsWindow::filterSections);
	}

	// Открыть все вкладки при самопроверке, включая те, что создаются при первом переходе
	bool ready() const {
		auto *tabs = tabs_;
		if (!tabs || !tabs->count()) {
			return false;
		}
		for (int index = 0; index < tabs->count(); ++index) {
			tabs->setCurrentIndex(index);
			if (!tabs->widget(index)->layout() && !tabs->widget(index)->property("settingsBuilt").toBool()) {
				return false;
			}
		}
		return QFileInfo(QStringLiteral("/bin/settings")).isExecutable() &&
		       !readSetting(QStringLiteral("Display.Resolution")).isEmpty();
	}

private:
	QTabWidget *tabs_ = nullptr;
	QListWidget *sections_ = nullptr;

	// Назначить разделу одну из постоянных групп боковой панели
	static QString sectionGroup(const QString &title) {
		if (title == QStringLiteral("Сеть") || title == QStringLiteral("Wi-Fi") ||
		    title == QStringLiteral("Локальная сеть")) {
			return QStringLiteral("Сеть и связь");
		}
		if (title == QStringLiteral("Электропитание") || title == QStringLiteral("Звук")) {
			return QStringLiteral("Оборудование");
		}
		if (title == QStringLiteral("POSIX") || title == QStringLiteral("Android") ||
		    title == QStringLiteral("WinAPI") || title == QStringLiteral("DOS")) {
			return QStringLiteral("Подсистемы");
		}
		if (title == QStringLiteral("О системе") || title == QStringLiteral("Отчёт")) {
			return QStringLiteral("Сведения и диагностика");
		}
		if (title == QStringLiteral("Экран") || title == QStringLiteral("Оформление") ||
		    title == QStringLiteral("Уведомления") || title == QStringLiteral("Терминал")) {
			return QStringLiteral("Рабочий стол");
		}
		return QStringLiteral("Система");
	}

	// Добавить заголовок группы, который нельзя выбрать как страницу
	void addSectionGroup(const QString &title) {
		auto *item = new QListWidgetItem(title, sections_);
		item->setFlags(Qt::NoItemFlags);
		item->setData(Qt::UserRole, -1);
		item->setData(Qt::UserRole + 1, true);
		QFont font = item->font();
		font.setBold(true);
		item->setFont(font);
		item->setSizeHint(QSize(0, 38));
	}

	// Добавить обычную страницу с индексом в общем стеке
	void addSectionItem(const QString &title, int index) {
		auto *item = new QListWidgetItem(QStringLiteral("  ") + title, sections_);
		item->setData(Qt::UserRole, index);
		item->setData(Qt::UserRole + 1, false);
		item->setSizeHint(QSize(0, 38));
	}

	// Оставить только подходящие страницы и заголовки непустых групп
	void filterSections(const QString &needle) {
		const QString query = needle.trimmed();
		for (int row = 0; row < sections_->count(); ++row) {
			auto *item = sections_->item(row);
			if (!item->data(Qt::UserRole + 1).toBool()) {
				item->setHidden(!query.isEmpty() && !item->text().contains(query, Qt::CaseInsensitive));
			}
		}
		for (int row = 0; row < sections_->count(); ++row) {
			auto *group = sections_->item(row);
			if (!group->data(Qt::UserRole + 1).toBool()) {
				continue;
			}
			bool visible = false;
			for (int next = row + 1; next < sections_->count() &&
			     !sections_->item(next)->data(Qt::UserRole + 1).toBool(); ++next) {
				visible |= !sections_->item(next)->isHidden();
			}
			group->setHidden(!visible);
		}
	}

	// Прочитать настройки вкладки один раз при первом переходе на неё
	void addOnDemand(QTabWidget *tabs, const QString &title, void (SettingsWindow::*build)(QWidget *)) {
		auto *page = new QWidget;
		auto *scroll = new QScrollArea(tabs);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setWidget(page);
		const int index = tabs->addTab(scroll, title);
		connect(tabs, &QTabWidget::currentChanged, page, [this, page, scroll, index, build](int current) {
			if (current == index && !page->layout()) {
				(this->*build)(page);
				scroll->setProperty("settingsBuilt", true);
			}
		});
	}

	// Настроить оболочку, шрифт и дополнительные консоли
	void buildTerminalPage(QWidget *page) {
		auto *form = new QFormLayout(page);
		auto *shell = new QComboBox(page);
		shell->addItems({QStringLiteral("/bin/zsh"), QStringLiteral("/bin/bash"), QStringLiteral("/bin/sh")});
		shell->setCurrentText(readSetting(QStringLiteral("Boot.DefaultShell")));
		form->addRow(QStringLiteral("Оболочка по умолчанию"), shell);

		auto *font = new QComboBox(page);
		static const char *fonts[] = {"dejavu-mono", "terminus", "hack", "noto-mono", "liberation-mono", "ubuntu-mono"};
		static const char *names[] = {"DejaVu Sans Mono", "Terminus", "Hack", "Noto Mono", "Liberation Mono", "Ubuntu Mono"};
		const QString current = readSetting(QStringLiteral("Terminal.Font"));
		for (int index = 0; index < 6; ++index) {
			const QString path = QStringLiteral("/assets/fonts/") + fonts[index] + ".psf";
			font->addItem(QString::fromUtf8(names[index]), path);
			if (path == current) {
				font->setCurrentIndex(index);
			}
		}
		form->addRow(QStringLiteral("Шрифт консоли"), font);

		auto *serial = new QComboBox(page);
		serial->addItems({QStringLiteral("0"), QStringLiteral("1"), QStringLiteral("2")});
		serial->setCurrentText(readSetting(QStringLiteral("Terminal.SerialConsoles")));
		form->addRow(QStringLiteral("Консоли на COM-портах"), serial);

		auto *apply = new QPushButton(QStringLiteral("Сохранить настройки терминала"), page);
		form->addRow(apply);
		connect(apply, &QPushButton::clicked, page, [=]() {
			if (!writeSetting(page, QStringLiteral("Boot.DefaultShell"), shell->currentText()) ||
			    !writeSetting(page, QStringLiteral("Terminal.Font"), font->currentData().toString()) ||
			    !writeSetting(page, QStringLiteral("Terminal.SerialConsoles"), serial->currentText())) {
				return;
			}
		});
	}

	// Настроить часовой пояс, сетевое и ручное время
	void buildDateTimePage(QWidget *page) {
		auto *form = new QFormLayout(page);
		auto *zone = new QComboBox(page);
		static const char *zones[] = {"UTC", "Europe/Moscow", "Europe/London", "Europe/Berlin",
		                              "Asia/Yekaterinburg", "Asia/Novosibirsk", "Asia/Vladivostok",
		                              "Asia/Tokyo", "America/New_York", "America/Los_Angeles"};
		zone->addItems(QStringList(std::begin(zones), std::end(zones)));
		zone->setCurrentText(readSetting(QStringLiteral("System.Timezone")));
		form->addRow(QStringLiteral("Часовой пояс"), zone);

		auto *manual = new QDateTimeEdit(QDateTime::currentDateTime(), page);
		manual->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
		manual->setCalendarPopup(true);
		form->addRow(QStringLiteral("Дата и время"), manual);
		auto *applyTime = new QPushButton(QStringLiteral("Применить дату, время и пояс"), page);
		auto *status = new QLabel(page);
		status->setWordWrap(true);
		form->addRow(applyTime);
		form->addRow(status);

		auto *ntp = new QPushButton(QStringLiteral("Синхронизировать по NTP"), page);
		form->addRow(ntp);
		connect(applyTime, &QPushButton::clicked, page, [=]() {
			if (!writeSetting(page, QStringLiteral("System.Timezone"), zone->currentText())) {
				return;
			}
			const qint64 seconds = manual->dateTime().toSecsSinceEpoch();
			const timespec requested {static_cast<time_t>(seconds), 0};
			if (clock_settime(CLOCK_REALTIME, &requested) != 0) {
				status->setText(QString::fromLocal8Bit(strerror(errno)));
				return;
			}
			QProcess::startDetached(QStringLiteral("/bin/hwclock"), {QStringLiteral("-w")});
			status->setText(QStringLiteral("Дата, время и часовой пояс сохранены"));
		});
		connect(ntp, &QPushButton::clicked, page, [=]() {
			auto *job = new QProcess(page);
			job->setProcessChannelMode(QProcess::MergedChannels);
			ntp->setEnabled(false);
			status->setText(QStringLiteral("Синхронизация с pool.ntp.org..."));
			connect(job, &QProcess::finished, page, [=](int code, QProcess::ExitStatus result) {
				ntp->setEnabled(true);
				if (code == 0 && result == QProcess::NormalExit) {
					writeSetting(page, QStringLiteral("System.Timezone"), zone->currentText());
					QProcess::startDetached(QStringLiteral("/bin/hwclock"), {QStringLiteral("-w")});
					manual->setDateTime(QDateTime::currentDateTime());
				}
				status->setText(code == 0 && result == QProcess::NormalExit ?
				                QStringLiteral("Время синхронизировано") :
				                QString::fromUtf8(job->readAllStandardOutput()).trimmed());
				job->deleteLater();
			});
			job->start(QStringLiteral("/bin/ntpdate"), {QStringLiteral("-s"), QStringLiteral("pool.ntp.org")});
		});
	}

	// Управлять проверкой и установкой системных обновлений
	void buildUpdatesPage(QWidget *page) {
		auto *form = new QFormLayout(page);
		auto *check = new QCheckBox(QStringLiteral("Проверять обновления при запуске"), page);
		check->setChecked(readSetting(QStringLiteral("Setup.CheckUpdates")) != QStringLiteral("0"));
		form->addRow(check);
		auto *update = new QPushButton(QStringLiteral("Проверить и установить обновления"), page);
		auto *store = new QPushButton(QStringLiteral("Открыть магазин программ"), page);
		auto *status = new QLabel(page);
		status->setWordWrap(true);
		form->addRow(update);
		form->addRow(store);
		form->addRow(status);
		connect(check, &QCheckBox::toggled, page, [=](bool enabled) {
			writeSetting(page, QStringLiteral("Setup.CheckUpdates"), enabled ? QStringLiteral("1") : QStringLiteral("0"));
		});
		connect(store, &QPushButton::clicked, page, [=]() {
			QProcess::startDetached(QStringLiteral("/bin/synapse-store"), {});
		});
		connect(update, &QPushButton::clicked, page, [=]() {
			auto *job = new QProcess(page);
			job->setProcessChannelMode(QProcess::MergedChannels);
			update->setEnabled(false);
			status->setText(QStringLiteral("Выполняется setup auto..."));
			connect(job, &QProcess::finished, page, [=](int code, QProcess::ExitStatus result) {
				update->setEnabled(true);
				const QString output = QString::fromUtf8(job->readAllStandardOutput()).trimmed();
				status->setText(code == 0 && result == QProcess::NormalExit ?
				                QStringLiteral("Обновление завершено") : output.right(1200));
				job->deleteLater();
			});
			job->start(QStringLiteral("/bin/setup"), {QStringLiteral("auto")});
		});
	}

	// Показать параметры телеметрии и состояние согласия отдельно от служб
	void buildPrivacyPage(QWidget *page) {
		auto *form = new QFormLayout(page);
		auto *enabled = new QCheckBox(QStringLiteral("Отправлять диагностические данные"), page);
		enabled->setChecked(readSetting(QStringLiteral("stats.stats")) == QStringLiteral("1"));
		form->addRow(enabled);
		auto *interval = new QSpinBox(page);
		interval->setRange(60, 86400);
		interval->setSuffix(QStringLiteral(" с"));
		interval->setValue(readSetting(QStringLiteral("stats.statsDelay")).toInt());
		form->addRow(QStringLiteral("Интервал отчётов"), interval);
		auto *apply = new QPushButton(QStringLiteral("Сохранить параметры отправки"), page);
		auto *consent = new QPushButton(QStringLiteral("Показать состояние согласия"), page);
		auto *status = new QLabel(page);
		status->setWordWrap(true);
		status->setTextInteractionFlags(Qt::TextSelectableByMouse);
		form->addRow(apply);
		form->addRow(consent);
		form->addRow(status);
		connect(apply, &QPushButton::clicked, page, [=]() {
			if (!writeSetting(page, QStringLiteral("stats.stats"), enabled->isChecked() ? QStringLiteral("1") : QStringLiteral("0")) ||
			    !writeSetting(page, QStringLiteral("stats.statsDelay"), QString::number(interval->value()))) {
				return;
			}
			status->setText(QStringLiteral("Параметры сохранены"));
		});
		connect(consent, &QPushButton::clicked, page, [=]() {
			auto *job = new QProcess(page);
			job->setProcessChannelMode(QProcess::MergedChannels);
			connect(job, &QProcess::finished, page, [=](int, QProcess::ExitStatus) {
				status->setText(QString::fromUtf8(job->readAllStandardOutput()).trimmed());
				job->deleteLater();
			});
			job->start(QStringLiteral("/bin/stats"), {QStringLiteral("--consent-status")});
		});
	}

	// Настроить локальный центр уведомлений рабочего стола
	void buildNotificationsPage(QWidget *page) {
		auto *form = new QFormLayout(page);
		QSettings preferences(QStringLiteral("SynapseOS"), QStringLiteral("Desktop"));
		auto *enabled = new QCheckBox(QStringLiteral("Показывать уведомления на рабочем столе"), page);
		auto *quiet = new QCheckBox(QStringLiteral("Режим «Не беспокоить»"), page);
		auto *updates = new QCheckBox(QStringLiteral("Сообщать о доступных обновлениях"), page);
		auto *duration = new QSpinBox(page);
		enabled->setChecked(preferences.value(QStringLiteral("notifications/enabled"), true).toBool());
		quiet->setChecked(preferences.value(QStringLiteral("notifications/quiet"), false).toBool());
		updates->setChecked(preferences.value(QStringLiteral("notifications/updates"), true).toBool());
		duration->setRange(3, 30);
		duration->setSuffix(QStringLiteral(" с"));
		duration->setValue(preferences.value(QStringLiteral("notifications/duration"), 7).toInt());
		form->addRow(enabled);
		form->addRow(quiet);
		form->addRow(updates);
		form->addRow(QStringLiteral("Время показа"), duration);
		auto *test = new QPushButton(QStringLiteral("Показать пробное уведомление"), page);
		form->addRow(test);
		auto *clear = new QPushButton(QStringLiteral("Очистить историю"), page);
		form->addRow(clear);
		auto *history = new QListWidget(page);
		history->setMaximumHeight(170);
		history->setAlternatingRowColors(true);
		auto refreshHistory = [=]() {
			history->clear();
			const QStringList entries = QSettings(QStringLiteral("SynapseOS"), QStringLiteral("Desktop"))
			    .value(QStringLiteral("notifications/history")).toStringList();
			history->addItems(entries.first(qMin(10, entries.size())));
		};
		refreshHistory();
		form->addRow(QStringLiteral("Последние события"), history);
		auto *note = new QLabel(QStringLiteral(
		    "Системный монитор и проверка обновлений используют эти параметры. Режим «Не беспокоить» сохраняет события в журнале настроек"), page);
		note->setWordWrap(true);
		form->addRow(note);
		auto save = [=]() {
			QSettings values(QStringLiteral("SynapseOS"), QStringLiteral("Desktop"));
			values.setValue(QStringLiteral("notifications/enabled"), enabled->isChecked());
			values.setValue(QStringLiteral("notifications/quiet"), quiet->isChecked());
			values.setValue(QStringLiteral("notifications/updates"), updates->isChecked());
			values.setValue(QStringLiteral("notifications/duration"), duration->value());
		};
		connect(enabled, &QCheckBox::toggled, page, save);
		connect(quiet, &QCheckBox::toggled, page, save);
		connect(updates, &QCheckBox::toggled, page, save);
		connect(duration, &QSpinBox::valueChanged, page, save);
		connect(test, &QPushButton::clicked, page, [=]() {
			save();
			auto request = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"),
			    QStringLiteral("/org/freedesktop/Notifications"), QStringLiteral("org.freedesktop.Notifications"),
			    QStringLiteral("Notify"));
			request.setArguments({QStringLiteral("synapse-settings"), 0U, QString(),
			    QStringLiteral("Настройки SynapseOS"), QStringLiteral("Системные уведомления работают"),
			    QStringList(), QVariantMap(), -1});
			const QDBusMessage reply = QDBusConnection::sessionBus().call(request, QDBus::Block, 3000);
			if (reply.type() != QDBusMessage::ReplyMessage) {
				note->setText(reply.errorMessage());
			}
			refreshHistory();
		});
		connect(clear, &QPushButton::clicked, page, [=]() {
			QSettings(QStringLiteral("SynapseOS"), QStringLiteral("Desktop"))
			    .setValue(QStringLiteral("notifications/history"), QStringList());
			refreshHistory();
		});
	}

	// Управлять аккаунтом и приватным каталогом Synapse Plus
	void buildPlusPage(QWidget *page) {
		auto *layout = new QVBoxLayout(page);
		auto *title = new QLabel(QStringLiteral("Подписка и пакеты Synapse Plus"), page);
		QFont font = title->font();
		font.setPointSize(font.pointSize() + 3);
		font.setBold(true);
		title->setFont(font);
		layout->addWidget(title);
		auto *description = new QLabel(QStringLiteral(
		    "Plus открывает приватный каталог пакетов и ранний доступ. Установленные программы сохраняются после отключения или окончания подписки"), page);
		description->setWordWrap(true);
		layout->addWidget(description);
		auto *status = new QPlainTextEdit(page);
		status->setReadOnly(true);
		status->setMaximumHeight(150);
		layout->addWidget(status);
		auto *login = new QPushButton(QStringLiteral("Войти по одноразовому коду"), page);
		auto *check = new QPushButton(QStringLiteral("Проверить состояние"), page);
		auto *enable = new QPushButton(QStringLiteral("Включить каталог Plus"), page);
		auto *disable = new QPushButton(QStringLiteral("Отключить каталог Plus"), page);
		auto *logout = new QPushButton(QStringLiteral("Выйти из аккаунта"), page);
		auto *store = new QPushButton(QStringLiteral("Открыть каталог в магазине"), page);
		for (auto *button : {login, check, enable, disable, logout, store}) {
			layout->addWidget(button);
		}
		layout->addStretch();
		const bool administrator = geteuid() == 0;
		const bool available = QFileInfo(QStringLiteral("/bin/setup")).isExecutable();
		for (auto *button : {login, check, enable, disable, logout}) {
			button->setEnabled(available);
		}
		if (!administrator) {
			status->setPlainText(QStringLiteral("При управлении Plus будет запрошен пароль sudo"));
		}
		auto run = [=](const QStringList &arguments, const QByteArray &secret = QByteArray()) {
			PrivilegedCommand prepared;
			if (!preparePrivileged(page, QStringLiteral("/bin/setup"), arguments, secret, prepared)) {
				status->setPlainText(QStringLiteral("Операция отменена"));
				return;
			}
			auto *job = new QProcess(page);
			job->setProcessChannelMode(QProcess::MergedChannels);
			for (auto *button : {login, check, enable, disable, logout}) {
				button->setEnabled(false);
			}
			status->setPlainText(QStringLiteral("Выполняется setup %1…").arg(arguments.join(QLatin1Char(' '))));
			connect(job, &QProcess::started, page, [job, prepared]() mutable {
				if (!prepared.input.isEmpty()) {
					job->write(prepared.input);
					job->closeWriteChannel();
					prepared.input.fill(0);
				}
			});
			connect(job, &QProcess::finished, page, [=](int code, QProcess::ExitStatus result) {
				const QString output = QString::fromUtf8(job->readAllStandardOutput()).trimmed();
				status->setPlainText(output + (code == 0 && result == QProcess::NormalExit ?
				    QString() : QStringLiteral("\nКод ошибки: %1").arg(code)));
				for (auto *button : {login, check, enable, disable, logout}) {
					button->setEnabled(true);
				}
				job->deleteLater();
			});
			connect(job, &QProcess::errorOccurred, page, [=](QProcess::ProcessError error) {
				if (error == QProcess::FailedToStart) {
					status->setPlainText(job->errorString());
					for (auto *button : {login, check, enable, disable, logout}) {
						button->setEnabled(true);
					}
					job->deleteLater();
				}
			});
			job->start(prepared.program, prepared.arguments);
		};
		connect(check, &QPushButton::clicked, page, [=]() { run({QStringLiteral("auth"), QStringLiteral("status")}); });
		connect(enable, &QPushButton::clicked, page, [=]() {
			bool accepted = false;
			QString key = QInputDialog::getText(page, QStringLiteral("Ключ Synapse Plus"),
			    QStringLiteral("43-значный ключ доступа из кабинета"), QLineEdit::Password, QString(), &accepted);
			key = key.trimmed();
			if (!accepted) {
				return;
			}
			if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{43}$")).match(key).hasMatch()) {
				QMessageBox::warning(page, QStringLiteral("Synapse Plus"), QStringLiteral("Неверный формат ключа"));
				key.fill(QChar(0));
				return;
			}
			const QByteArray secret = key.toUtf8();
			key.fill(QChar(0));
			run({QStringLiteral("plus"), QStringLiteral("enable")}, secret);
		});
		connect(disable, &QPushButton::clicked, page, [=]() { run({QStringLiteral("plus"), QStringLiteral("disable")}); });
		connect(logout, &QPushButton::clicked, page, [=]() {
			if (QMessageBox::question(page, QStringLiteral("Synapse Plus"),
			    QStringLiteral("Отозвать сессию setup на этом компьютере?")) == QMessageBox::Yes) {
				run({QStringLiteral("auth"), QStringLiteral("logout")});
			}
		});
		connect(login, &QPushButton::clicked, page, [=]() {
			bool accepted = false;
			QString code = QInputDialog::getText(page, QStringLiteral("Вход в Synapse Plus"),
			    QStringLiteral("12-значный код из https://synapseos.ru/lk/devices"), QLineEdit::Password,
			    QString(), &accepted);
			code = code.trimmed();
			if (!accepted) {
				return;
			}
			if (!QRegularExpression(QStringLiteral("^[0-9]{12}$")).match(code).hasMatch()) {
				QMessageBox::warning(page, QStringLiteral("Synapse Plus"), QStringLiteral("Код должен содержать 12 цифр"));
				code.fill(QChar(0));
				return;
			}
			const QByteArray secret = code.toUtf8();
			code.fill(QChar(0));
			run({QStringLiteral("auth")}, secret);
		});
		connect(store, &QPushButton::clicked, page, []() {
			QProcess::startDetached(QStringLiteral("/bin/synapse-store"), {});
		});
		if (administrator) {
			run({QStringLiteral("auth"), QStringLiteral("status")});
		}
	}

	// Показать локальные учётные записи и вызвать безопасные системные утилиты
	void buildUsersPage(QWidget *page) {
		auto *layout = new QVBoxLayout(page);
		auto *table = new QTableWidget(page);
		table->setColumnCount(5);
		table->setHorizontalHeaderLabels({QStringLiteral("Пользователь"), QStringLiteral("UID"),
		    QStringLiteral("GID"), QStringLiteral("Домашний каталог"), QStringLiteral("Оболочка")});
		table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
		table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
		table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
		table->verticalHeader()->hide();
		table->setSelectionBehavior(QAbstractItemView::SelectRows);
		table->setSelectionMode(QAbstractItemView::SingleSelection);
		table->setEditTriggers(QAbstractItemView::NoEditTriggers);
		layout->addWidget(table, 1);
		auto *create = new QPushButton(QStringLiteral("Создать пользователя"), page);
		auto *password = new QPushButton(QStringLiteral("Изменить пароль"), page);
		auto *shell = new QPushButton(QStringLiteral("Изменить оболочку"), page);
		auto *buttons = new QHBoxLayout();
		buttons->addWidget(create);
		buttons->addWidget(password);
		buttons->addWidget(shell);
		buttons->addStretch();
		layout->addLayout(buttons);
		auto *status = new QLabel(page);
		status->setWordWrap(true);
		layout->addWidget(status);
		auto refresh = [=]() {
			table->setRowCount(0);
			setpwent();
			while (passwd *entry = getpwent()) {
				const int row = table->rowCount();
				table->insertRow(row);
				auto *name = new QTableWidgetItem(QString::fromUtf8(entry->pw_name));
				name->setData(Qt::UserRole, name->text());
				table->setItem(row, 0, name);
				table->setItem(row, 1, new QTableWidgetItem(QString::number(entry->pw_uid)));
				table->setItem(row, 2, new QTableWidgetItem(QString::number(entry->pw_gid)));
				table->setItem(row, 3, new QTableWidgetItem(QString::fromUtf8(entry->pw_dir)));
				table->setItem(row, 4, new QTableWidgetItem(QString::fromUtf8(entry->pw_shell)));
			}
			endpwent();
			status->setText(QStringLiteral("При изменении учётных записей будет запрошен пароль sudo"));
		};
		auto selected = [=]() -> QString {
			auto *item = table->currentItem();
			return item ? table->item(table->currentRow(), 0)->data(Qt::UserRole).toString() : QString();
		};
		auto setPassword = [=](const QString &name, const QString &value) {
			QByteArray secret = value.toUtf8() + "\n";
			QString output;
			const bool okay = runPrivileged(page, QStringLiteral("/bin/passwd"),
			    {QStringLiteral("--stdin"), name}, secret, output);
			secret.fill(0);
			if (!okay) {
				QMessageBox::warning(page, QStringLiteral("Учётные записи"), output);
			}
			return okay;
		};
		connect(create, &QPushButton::clicked, page, [=]() {
			QDialog dialog(page);
			dialog.setWindowTitle(QStringLiteral("Новый пользователь"));
			auto *form = new QFormLayout(&dialog);
			auto *name = new QLineEdit(&dialog);
			auto *secret = new QLineEdit(&dialog);
			auto *confirmation = new QLineEdit(&dialog);
			auto *loginShell = new QComboBox(&dialog);
			secret->setEchoMode(QLineEdit::Password);
			confirmation->setEchoMode(QLineEdit::Password);
			loginShell->addItems({QStringLiteral("/bin/zsh"), QStringLiteral("/bin/bash"), QStringLiteral("/bin/sh")});
			form->addRow(QStringLiteral("Имя"), name);
			form->addRow(QStringLiteral("Пароль"), secret);
			form->addRow(QStringLiteral("Повтор пароля"), confirmation);
			form->addRow(QStringLiteral("Оболочка"), loginShell);
			auto *accept = new QPushButton(QStringLiteral("Создать"), &dialog);
			form->addRow(accept);
			connect(accept, &QPushButton::clicked, &dialog, [&]() {
				const QString user = name->text().trimmed();
				if (!QRegularExpression(QStringLiteral("^[a-zA-Z][a-zA-Z0-9_-]{0,31}$")).match(user).hasMatch()) {
					QMessageBox::warning(&dialog, QStringLiteral("Пользователь"), QStringLiteral("Недопустимое имя пользователя"));
					return;
				}
				if (secret->text().size() < 8 || secret->text() != confirmation->text()) {
					QMessageBox::warning(&dialog, QStringLiteral("Пользователь"),
					    QStringLiteral("Пароли должны совпадать и содержать не менее 8 символов"));
					return;
				}
				QString output;
				if (!runPrivileged(&dialog, QStringLiteral("/bin/useradd"),
				    {QStringLiteral("-m"), QStringLiteral("-s"), loginShell->currentText(), user}, {}, output)) {
					QMessageBox::warning(&dialog, QStringLiteral("Пользователь"), output);
					return;
				}
				if (!setPassword(user, secret->text())) {
					return;
				}
				dialog.accept();
				refresh();
			});
			dialog.exec();
		});
		connect(password, &QPushButton::clicked, page, [=]() {
			const QString user = selected();
			if (user.isEmpty()) {
				return;
			}
			bool accepted = false;
			QString value = QInputDialog::getText(page, QStringLiteral("Новый пароль"),
			    QStringLiteral("Пароль для %1, минимум 8 символов").arg(user), QLineEdit::Password,
			    QString(), &accepted);
			if (accepted && value.size() >= 8 && setPassword(user, value)) {
				status->setText(QStringLiteral("Пароль пользователя %1 изменён").arg(user));
			} else if (accepted && value.size() < 8) {
				QMessageBox::warning(page, QStringLiteral("Пароль"), QStringLiteral("Пароль слишком короткий"));
			}
			value.fill(QChar(0));
		});
		connect(shell, &QPushButton::clicked, page, [=]() {
			const QString user = selected();
			if (user.isEmpty()) {
				return;
			}
			bool accepted = false;
			const QString value = QInputDialog::getItem(page, QStringLiteral("Оболочка"),
			    QStringLiteral("Оболочка для %1").arg(user),
			    {QStringLiteral("/bin/zsh"), QStringLiteral("/bin/bash"), QStringLiteral("/bin/sh")}, 0,
			    false, &accepted);
			if (!accepted) {
				return;
			}
			QString output;
			if (!runPrivileged(page, QStringLiteral("/bin/usermod"), {QStringLiteral("-s"), value, user}, {}, output)) {
				QMessageBox::warning(page, QStringLiteral("Оболочка"), output);
			}
			refresh();
		});
		create->setEnabled(QFileInfo(QStringLiteral("/bin/useradd")).isExecutable());
		password->setEnabled(QFileInfo(QStringLiteral("/bin/passwd")).isExecutable());
		shell->setEnabled(QFileInfo(QStringLiteral("/bin/usermod")).isExecutable());
		refresh();
	}

	// Управлять выделением памяти для всех программ нового сеанса
	void buildMemoryPage(QWidget *page) {
		auto *layout = new QVBoxLayout(page);
		auto *details = new QTableWidget(page);
		details->setColumnCount(2);
		details->setHorizontalHeaderLabels({QStringLiteral("Состав памяти"), QStringLiteral("Размер")});
		details->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
		details->verticalHeader()->hide();
		details->setEditTriggers(QAbstractItemView::NoEditTriggers);
		details->setMaximumHeight(285);
		layout->addWidget(details);
		auto *bar = new QProgressBar(page);
		bar->setRange(0, 100);
		layout->addWidget(bar);
		auto refresh = [=]() {
			const QList<QPair<QByteArray, QString>> names = {
			    {"MemTotal:", QStringLiteral("Всего ОЗУ")}, {"MemAvailable:", QStringLiteral("Доступно программам")},
			    {"MemFree:", QStringLiteral("Свободно без учёта кэша")}, {"UserAnon:", QStringLiteral("Анонимная память процессов")},
			    {"Cached:", QStringLiteral("Файловый кэш")}, {"Shmem:", QStringLiteral("Разделяемая память")},
			    {"KernelHeap:", QStringLiteral("Куча ядра")}, {"PageTables:", QStringLiteral("Таблицы страниц")},
			    {"Drivers:", QStringLiteral("Драйверы")}, {"OtherUsed:", QStringLiteral("Прочее использование")},
			    {"SwapTotal:", QStringLiteral("Swap всего")}, {"SwapFree:", QStringLiteral("Swap свободно")}
			};
			QHash<QByteArray, qulonglong> values;
			for (const QByteArray &line : readReport(QStringLiteral("/proc/meminfo")).split('\n')) {
				const QList<QByteArray> fields = line.simplified().split(' ');
				if (fields.size() >= 2) {
					values.insert(fields[0], fields[1].toULongLong());
				}
			}
			details->setRowCount(names.size());
			for (int row = 0; row < names.size(); ++row) {
				details->setItem(row, 0, new QTableWidgetItem(names[row].second));
				details->setItem(row, 1, new QTableWidgetItem(QStringLiteral("%1 МиБ").arg(values[names[row].first] / 1024)));
			}
			const qulonglong total = values["MemTotal:"];
			const qulonglong available = values["MemAvailable:"];
			bar->setValue(total ? static_cast<int>((total - available) * 100 / total) : 0);
			bar->setFormat(QStringLiteral("Занято %1 МиБ из %2 МиБ").arg((total - available) / 1024).arg(total / 1024));
		};

		const posix_profile profile = read_posix_profile(QFile::encodeName(QDir::homePath()).constData());
		auto *form = new QFormLayout();
		form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		auto *mode = new QComboBox(page);
		mode->addItem(QStringLiteral("Ленивое выделение при первом обращении к странице"), 0);
		mode->addItem(QStringLiteral("Предварительное выделение всего отображения"), 1);
		mode->setCurrentIndex(profile.populate ? 1 : 0);
		form->addRow(QStringLiteral("Выделение памяти в системе"), mode);
		auto *limit = new QSpinBox(page);
		limit->setRange(0, 1048576);
		limit->setSpecialValueText(QStringLiteral("Без отдельного ограничения"));
		limit->setSuffix(QStringLiteral(" МиБ"));
		limit->setValue(static_cast<int>(profile.memory_mib));
		form->addRow(QStringLiteral("Адресное пространство на процесс"), limit);
		auto *stack = new QSpinBox(page);
		stack->setRange(0, 4096);
		stack->setSpecialValueText(QStringLiteral("Наследовать системный предел"));
		stack->setSuffix(QStringLiteral(" МиБ"));
		stack->setValue(static_cast<int>(profile.stack_mib));
		form->addRow(QStringLiteral("Стек на процесс"), stack);
		layout->addLayout(form);
		auto *note = new QLabel(QStringLiteral(
		    "Эти параметры наследуют все программы нового графического сеанса. RSS процессов не складывается с общей ОЗУ напрямую, поскольку включает общие библиотеки и файловые страницы"), page);
		note->setWordWrap(true);
		layout->addWidget(note);

		auto *swap = new QGroupBox(QStringLiteral("Подкачка"), page);
		auto *swapForm = new QFormLayout(swap);
		auto *swapType = new QComboBox(swap);
		swapType->addItems({QStringLiteral("Отключена"), QStringLiteral("Файл"), QStringLiteral("Раздел")});
		swapType->setEnabled(false);
		swapForm->addRow(QStringLiteral("Тип"), swapType);
		auto *swapSize = new QSpinBox(swap);
		swapSize->setRange(128, 1048576);
		swapSize->setSuffix(QStringLiteral(" МиБ"));
		swapSize->setEnabled(false);
		swapForm->addRow(QStringLiteral("Размер"), swapSize);
		auto *swapReason = new QLabel(QStringLiteral("Ядро пока не предоставляет backend swap, поэтому параметры доступны только для просмотра"), swap);
		swapReason->setWordWrap(true);
		swapForm->addRow(swapReason);
		layout->addWidget(swap);

		auto *apply = new QPushButton(QStringLiteral("Сохранить для всех программ следующего сеанса"), page);
		auto *refreshButton = new QPushButton(QStringLiteral("Обновить показания"), page);
		layout->addWidget(apply);
		layout->addWidget(refreshButton);
		auto *status = new QLabel(page);
		status->setWordWrap(true);
		layout->addWidget(status);
		layout->addStretch();
		connect(refreshButton, &QPushButton::clicked, page, refresh);
		connect(apply, &QPushButton::clicked, page, [=]() {
			char path[128];
			posix_profile_path(path);
			QSaveFile file(QString::fromLatin1(path));
			const QByteArray data = "mask=" + QByteArray::number(profile.mask) + "\nfiles=" +
			    QByteArray::number(profile.files) + "\nmemory=" + QByteArray::number(limit->value()) +
			    "\npopulate=" + QByteArray::number(mode->currentData().toInt()) + "\nnice=" +
			    QByteArray::number(profile.nice) + "\nstack=" + QByteArray::number(stack->value()) + "\n";
			if (!file.open(QIODevice::WriteOnly) ||
			    !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
			    file.write(data) != data.size() || !file.commit()) {
				status->setText(QStringLiteral("Не удалось сохранить параметры памяти"));
				return;
			}
			status->setText(QStringLiteral("Настройки памяти сохранены для всего следующего графического сеанса"));
		});
		refresh();
	}

	// Собрать подробный локальный отчёт без паролей и ключей доступа
	void buildReportPage(QWidget *page) {
		auto *layout = new QVBoxLayout(page);
		auto *description = new QLabel(QStringLiteral(
		    "Отчёт содержит оборудование, состояние памяти и подсистем, файловые системы и установленные пакеты. Пароли, ключи Plus и токены не включаются"), page);
		description->setWordWrap(true);
		layout->addWidget(description);
		auto *logs = new QCheckBox(QStringLiteral("Добавить последние строки системных журналов"), page);
		layout->addWidget(logs);
		auto *preview = new QPlainTextEdit(page);
		preview->setReadOnly(true);
		layout->addWidget(preview, 1);
		auto *generate = new QPushButton(QStringLiteral("Сформировать отчёт"), page);
		auto *save = new QPushButton(QStringLiteral("Сохранить в домашний каталог"), page);
		auto *open = new QPushButton(QStringLiteral("Открыть каталог отчётов"), page);
		auto *row = new QHBoxLayout();
		row->addWidget(generate);
		row->addWidget(save);
		row->addWidget(open);
		layout->addLayout(row);
		auto *status = new QLabel(page);
		status->setWordWrap(true);
		layout->addWidget(status);
		auto redacted = [](const QByteArray &input) {
			QStringList output;
			for (const QByteArray &line : input.split('\n')) {
				const QString text = QString::fromUtf8(line);
				if (text.contains(QStringLiteral("password"), Qt::CaseInsensitive) ||
				    text.contains(QStringLiteral("token"), Qt::CaseInsensitive) ||
				    text.contains(QStringLiteral("secret"), Qt::CaseInsensitive)) {
					continue;
				}
				output.append(text);
			}
			return output.join(QLatin1Char('\n'));
		};
		auto collect = [=]() {
			QStringList report;
			report << QStringLiteral("Диагностический отчёт SynapseOS")
			       << QDateTime::currentDateTime().toString(Qt::ISODate)
			       << QSysInfo::prettyProductName() << QSysInfo::currentCpuArchitecture();
			for (const QString &path : {QStringLiteral("/proc/version"), QStringLiteral("/proc/cpuinfo"),
			     QStringLiteral("/proc/meminfo"), QStringLiteral("/proc/power-profile"),
			     QStringLiteral("/proc/power"), QStringLiteral("/proc/media"),
			     QStringLiteral("/proc/storage"), QStringLiteral("/proc/thermal"),
			     QStringLiteral("/synapse/subsys/android/enabled"),
			     QStringLiteral("/synapse/subsys/android/api-profile")}) {
				const QByteArray data = readReport(path);
				if (!data.isEmpty()) {
					report << QString() << QStringLiteral("=== %1 ===").arg(path) << QString::fromUtf8(data).trimmed();
				}
			}
			report << QString() << QStringLiteral("=== Файловые системы ===");
			for (const QStorageInfo &volume : QStorageInfo::mountedVolumes()) {
				if (!volume.isReady() || (volume.isReadOnly() && !volume.isRoot())) {
					continue;
				}
				report << QStringLiteral("%1: %2, %3, свободно %4 МиБ из %5 МиБ")
				    .arg(volume.rootPath(), QString::fromUtf8(volume.fileSystemType()),
				         QString::fromUtf8(volume.device()))
				    .arg(volume.bytesAvailable() / 1048576).arg(volume.bytesTotal() / 1048576);
			}
			QProcess packages;
			packages.start(QStringLiteral("/bin/setup"), {QStringLiteral("ls"), QStringLiteral("-i")});
			if (packages.waitForFinished(8000)) {
				report << QString() << QStringLiteral("=== Установленные пакеты ===")
				       << redacted(packages.readAllStandardOutput());
			}
			const QByteArray settings = readReport(QStringLiteral("/etc/settings.ini"));
			if (!settings.isEmpty()) {
				report << QString() << QStringLiteral("=== Настройки без секретов ===") << redacted(settings);
			}
			if (logs->isChecked()) {
				const QDir directory(QStringLiteral("/var/log"));
				const auto entries = directory.entryInfoList({QStringLiteral("*.log")}, QDir::Files, QDir::Name);
				for (const QFileInfo &entry : entries.first(qMin(12, entries.size()))) {
					QFile file(entry.absoluteFilePath());
					if (file.open(QIODevice::ReadOnly)) {
						report << QString() << QStringLiteral("=== %1 ===").arg(entry.fileName())
						       << redacted(file.read(65536));
					}
				}
			}
			return report.join(QLatin1Char('\n'));
		};
		connect(generate, &QPushButton::clicked, page, [=]() {
			preview->setPlainText(collect());
			save->setEnabled(true);
			status->setText(QStringLiteral("Отчёт сформирован локально и никуда не отправлен"));
		});
		connect(save, &QPushButton::clicked, page, [=]() {
			if (preview->toPlainText().isEmpty()) {
				preview->setPlainText(collect());
			}
			const QString directory = QDir::homePath() + QStringLiteral("/Documents/SynapseOS Reports");
			if (!QDir().mkpath(directory)) {
				status->setText(QStringLiteral("Не удалось создать каталог отчётов"));
				return;
			}
			const QString path = directory + QStringLiteral("/report-%1.txt")
			    .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
			QSaveFile file(path);
			const QByteArray data = preview->toPlainText().toUtf8();
			if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
				status->setText(file.errorString());
				return;
			}
			status->setText(QStringLiteral("Сохранено: %1").arg(path));
		});
		connect(open, &QPushButton::clicked, page, []() {
			QProcess::startDetached(QStringLiteral("/bin/synapse-files"),
			    {QDir::homePath() + QStringLiteral("/Documents/SynapseOS Reports")});
		});
		save->setEnabled(false);
	}

	// Прочитать выбранную тему из пользовательского или системного файла Labwc
	QString currentTheme() const {
		return desktopTheme();
	}

	// Найти композитор по его исполняемому файлу при запуске настроек вне сеанса
	static QString runningLabwcPid() {
		const QDir proc(QStringLiteral("/proc"));
		for (const QString &entry : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
			bool number = false;
			entry.toInt(&number);
			if (number && QFileInfo(QStringLiteral("/proc/%1/exe").arg(entry)).symLinkTarget() ==
			    QStringLiteral("/usr/libexec/labwc/labwc")) {
				return entry;
			}
		}
		return QString();
	}

	// Записать личную тему и попросить Labwc перечитать настройки
	bool applyTheme(const QString &name) {
		bool known = false;
		for (const auto &theme : desktopThemes) {
			known |= name == QLatin1String(theme.name);
		}
		if (!known) {
			return false;
		}

		if (name.startsWith(QStringLiteral("Synapse-")) || name == QStringLiteral("synapse-classic")) {
			const auto &spec = desktopThemeSpec(name);
			const QString directory = QDir::homePath() + QStringLiteral("/.config/labwc/themes/") +
			    name + QStringLiteral("/openbox-3");
			if (!QDir().mkpath(directory)) {
				return false;
			}
			QSettings preferences(QStringLiteral("SynapseOS"), QStringLiteral("Desktop"));
			auto configured = [&preferences](const QString &key, const char *fallback) {
				const QString value = preferences.value(QStringLiteral("appearance/") + key,
				    QString::fromLatin1(fallback)).toString();
				return QColor(value).isValid() ? QColor(value).name() : QString::fromLatin1(fallback);
			};
			const QString corner = preferences.value(QStringLiteral("appearance/corner-radius"),
			    name == QStringLiteral("synapse-classic") ? 0 : 6).toString();
			const QString background = configured(QStringLiteral("background"), spec.background);
			const QString base = configured(QStringLiteral("base"), spec.base);
			const QString surface = configured(QStringLiteral("surface"), spec.surface);
			const QString foreground = configured(QStringLiteral("foreground"), spec.foreground);
			const QString accent = configured(QStringLiteral("accent"), spec.accent);
			const QByteArray themeData = QStringLiteral(
			    "border.width: 1\nwindow.active.border.color: %5\nwindow.inactive.border.color: %3\n"
			    "window.active.title.bg.color: %1\nwindow.inactive.title.bg.color: %2\n"
			    "window.*.title.bg: Solid\nwindow.active.label.text.color: %4\n"
			    "window.inactive.label.text.color: #63717d\nwindow.label.text.justify: left\n"
			    "window.button.width: 30\nwindow.button.height: 30\nwindow.button.spacing: 3\n"
			    "window.button.hover.bg.color: %5\nwindow.button.hover.bg.corner-radius: %6\n"
			    "window.active.button.unpressed.image.color: %4\nwindow.inactive.button.unpressed.image.color: #63717d\n"
			    "menu.border.width: 1\nmenu.border.color: %5\nmenu.items.bg.color: %2\n"
			    "menu.items.text.color: %4\nmenu.items.active.bg.color: %5\nmenu.items.active.text.color: #ffffff\n"
			    "menu.title.bg.color: %1\nmenu.title.text.color: %4\nosd.bg.color: %2\n"
			    "osd.border.color: %5\nosd.label.text.color: %4\n")
			    .arg(background, base, surface, foreground, accent, corner).toUtf8();
			QSaveFile themeFile(directory + QStringLiteral("/themerc"));
			if (!themeFile.open(QIODevice::WriteOnly) || themeFile.write(themeData) != themeData.size() ||
			    !themeFile.commit()) {
				return false;
			}
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
		auto environment = QProcessEnvironment::systemEnvironment();
		if (!environment.contains(QStringLiteral("LABWC_PID")) && !runningLabwcPid().isEmpty()) {
			environment.insert(QStringLiteral("LABWC_PID"), runningLabwcPid());
		}
		if (!environment.contains(QStringLiteral("WAYLAND_DISPLAY"))) {
			environment.insert(QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("wayland-0"));
		}
		refresh.setProcessEnvironment(environment);
		refresh.start(QStringLiteral("/bin/labwc"), {QStringLiteral("--reconfigure")});
		if (!refresh.waitForFinished(5000) || refresh.exitCode() != 0) {
			return false;
		}

		applyDesktopPalette(name);
		return true;
	}

	// Показать текущий профиль ядра и сохранить выбор для следующих загрузок
	void buildPowerPage(QWidget *page) {
		auto *form = new QFormLayout(page);
		auto *choice = new QComboBox(page);
		choice->addItem(QStringLiteral("Сбалансированный"), QStringLiteral("balanced"));
		choice->addItem(QStringLiteral("Экономия энергии"), QStringLiteral("powersave"));
		choice->addItem(QStringLiteral("Производительность"), QStringLiteral("performance"));
		choice->addItem(QStringLiteral("Управление прошивкой"), QStringLiteral("firmware"));
		form->addRow(QStringLiteral("Профиль процессора"), choice);
		auto *apply = new QPushButton(QStringLiteral("Применить и сохранить"), page);
		auto *status = new QLabel(page);
		status->setWordWrap(true);
		form->addRow(apply);
		form->addRow(status);
		auto refresh = [=]() {
			QFile report(QStringLiteral("/proc/power-profile"));
			QByteArray current, backend;
			if (report.open(QIODevice::ReadOnly)) {
				for (const auto &field : report.read(512).simplified().split(' ')) {
					if (field.startsWith("profile=")) current = field.mid(8);
					if (field.startsWith("backend=")) backend = field.mid(8);
				}
			}
			if (current.isEmpty()) {
				current = readSetting(QStringLiteral("Power.Profile")).toLatin1();
			}
			const bool supported = !backend.isEmpty() && backend != "unavailable";
			choice->setProperty("powerSupported", supported);
			choice->setCurrentIndex(qMax(0, choice->findData(QString::fromLatin1(current))));
			status->setText(supported ? QStringLiteral("Текущий драйвер: %1").arg(QString::fromLatin1(backend)) :
			    QStringLiteral("Аппаратный драйвер недоступен. Выбор сохраняется и будет применён при появлении поддержки"));
			return current;
		};
		refresh();
		connect(apply, &QPushButton::clicked, this, [=, this]() {
			const QByteArray value = choice->currentData().toByteArray();
			const QByteArray before = refresh();
			QString output;
			if (choice->property("powerSupported").toBool() &&
			    !runPrivileged(page, QStringLiteral("/bin/tee"), {QStringLiteral("/proc/power-profile")}, value, output)) {
				status->setText(QStringLiteral("Не удалось применить профиль: %1").arg(output));
				return;
			}
			if (!writeSetting(page, QStringLiteral("Power.Profile"), QString::fromLatin1(value)) &&
			    !before.isEmpty() && choice->property("powerSupported").toBool()) {
				runPrivileged(page, QStringLiteral("/bin/tee"), {QStringLiteral("/proc/power-profile")}, before, output);
				return;
			}
			refresh();
			status->setText(status->text() + QStringLiteral("\nПрофиль %1 сохранён").arg(choice->currentText()));
		});
	}

	// Настроить и переносить оформление всех штатных программ
	void buildAppearancePage(QWidget *page) {
		auto *layout = new QVBoxLayout(page);
		auto *form = new QFormLayout();
		form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		auto *theme = new QComboBox(page);
		for (const auto &item : desktopThemes) {
			theme->addItem(QLatin1String(item.name));
		}
		theme->setCurrentText(currentTheme());
		form->addRow(QStringLiteral("Базовая тема"), theme);
		QSettings preferences(QStringLiteral("SynapseOS"), QStringLiteral("Desktop"));
		const auto spec = desktopThemeSpec(theme->currentText());

		auto *fontFamily = new QFontComboBox(page);
		fontFamily->setCurrentFont(QFont(preferences.value(QStringLiteral("appearance/font-family"),
		    QApplication::font().family()).toString()));
		form->addRow(QStringLiteral("Шрифт интерфейса"), fontFamily);
		auto *fontSize = new QSpinBox(page);
		fontSize->setRange(8, 24);
		fontSize->setSuffix(QStringLiteral(" пт"));
		fontSize->setValue(preferences.value(QStringLiteral("appearance/font-size"), 10).toInt());
		form->addRow(QStringLiteral("Размер текста"), fontSize);
		auto *radius = new QSpinBox(page);
		radius->setRange(0, 24);
		radius->setSuffix(QStringLiteral(" пикс."));
		radius->setValue(preferences.value(QStringLiteral("appearance/corner-radius"),
		    theme->currentText() == QStringLiteral("synapse-classic") ? 0 : 7).toInt());
		form->addRow(QStringLiteral("Скругление углов"), radius);
		auto *padding = new QSpinBox(page);
		padding->setRange(2, 16);
		padding->setSuffix(QStringLiteral(" пикс."));
		padding->setValue(preferences.value(QStringLiteral("appearance/padding"), 6).toInt());
		form->addRow(QStringLiteral("Внутренние отступы"), padding);
		auto *sidebar = new QSpinBox(page);
		sidebar->setRange(180, 380);
		sidebar->setSuffix(QStringLiteral(" пикс."));
		sidebar->setValue(preferences.value(QStringLiteral("appearance/sidebar-width"), 250).toInt());
		form->addRow(QStringLiteral("Ширина списка разделов"), sidebar);
		auto *panelHeight = new QSpinBox(page);
		panelHeight->setRange(32, 96);
		panelHeight->setSuffix(QStringLiteral(" пикс."));
		panelHeight->setValue(preferences.value(QStringLiteral("appearance/panel-height"), 44).toInt());
		panelHeight->setToolTip(QStringLiteral("Новая высота применяется после перезапуска нижней панели"));
		form->addRow(QStringLiteral("Высота нижней панели"), panelHeight);

		const QStringList colorKeys = {QStringLiteral("background"), QStringLiteral("base"),
		    QStringLiteral("surface"), QStringLiteral("foreground"), QStringLiteral("accent")};
		const QStringList colorNames = {QStringLiteral("Фон окна"), QStringLiteral("Фон полей"),
		    QStringLiteral("Фон кнопок"), QStringLiteral("Основной текст"), QStringLiteral("Акцентный цвет")};
		const QStringList defaults = {QString::fromLatin1(spec.background), QString::fromLatin1(spec.base),
		    QString::fromLatin1(spec.surface), QString::fromLatin1(spec.foreground), QString::fromLatin1(spec.accent)};
		QList<QPushButton *> colors;
		for (int index = 0; index < colorKeys.size(); ++index) {
			auto *button = new QPushButton(page);
			button->setProperty("color", preferences.value(QStringLiteral("appearance/") + colorKeys[index],
			    defaults[index]).toString());
			button->setText(button->property("color").toString());
			form->addRow(colorNames[index], button);
			colors.append(button);
			connect(button, &QPushButton::clicked, page, [page, button]() {
				const QColor selected = QColorDialog::getColor(QColor(button->property("color").toString()), page);
				if (selected.isValid()) {
					button->setProperty("color", selected.name());
					button->setText(selected.name());
				}
			});
		}
		connect(theme, &QComboBox::currentTextChanged, page, [=](const QString &name) {
			const auto selected = desktopThemeSpec(name);
			const QStringList values = {QString::fromLatin1(selected.background), QString::fromLatin1(selected.base),
			    QString::fromLatin1(selected.surface), QString::fromLatin1(selected.foreground),
			    QString::fromLatin1(selected.accent)};
			for (int index = 0; index < colors.size(); ++index) {
				colors[index]->setProperty("color", values[index]);
				colors[index]->setText(values[index]);
			}
			radius->setValue(name == QStringLiteral("synapse-classic") ? 0 : 7);
		});

		auto *wallpaper = new QComboBox(page);
		wallpaper->addItem(QStringLiteral("Тёмный фон SynapseOS"),
		    QStringLiteral("/usr/share/backgrounds/synapse/Desktop-dark.png"));
		wallpaper->addItem(QStringLiteral("Светлый фон SynapseOS"),
		    QStringLiteral("/usr/share/backgrounds/synapse/Desktop-light.png"));
		form->addRow(QStringLiteral("Обои"), wallpaper);
		layout->addLayout(form);
		auto *preview = new QLabel(page);
		preview->setMinimumHeight(150);
		preview->setAlignment(Qt::AlignCenter);
		layout->addWidget(preview);
		auto updatePreview = [=]() {
			preview->setPixmap(QPixmap(wallpaper->currentData().toString()).scaled(520, 200,
			    Qt::KeepAspectRatio, Qt::SmoothTransformation));
		};
		connect(wallpaper, &QComboBox::currentIndexChanged, page, updatePreview);
		updatePreview();

		auto *apply = new QPushButton(QStringLiteral("Применить оформление"), page);
		auto *save = new QPushButton(QStringLiteral("Экспортировать тему в файл…"), page);
		auto *load = new QPushButton(QStringLiteral("Импортировать тему из файла…"), page);
		auto *buttons = new QHBoxLayout();
		buttons->addWidget(apply);
		buttons->addWidget(save);
		buttons->addWidget(load);
		layout->addLayout(buttons);
		auto *status = new QLabel(page);
		status->setWordWrap(true);
		layout->addWidget(status);
		layout->addStretch();

		auto saveValues = [=](QSettings &target) {
			target.setValue(QStringLiteral("theme"), theme->currentText());
			target.setValue(QStringLiteral("font-family"), fontFamily->currentFont().family());
			target.setValue(QStringLiteral("font-size"), fontSize->value());
			target.setValue(QStringLiteral("corner-radius"), radius->value());
			target.setValue(QStringLiteral("padding"), padding->value());
			target.setValue(QStringLiteral("sidebar-width"), sidebar->value());
			target.setValue(QStringLiteral("panel-height"), panelHeight->value());
			target.setValue(QStringLiteral("wallpaper"), wallpaper->currentData().toString());
			for (int index = 0; index < colors.size(); ++index) {
				target.setValue(colorKeys[index], colors[index]->property("color").toString());
			}
		};
		connect(apply, &QPushButton::clicked, this, [=, this]() {
			QSettings values(QStringLiteral("SynapseOS"), QStringLiteral("Desktop"));
			values.beginGroup(QStringLiteral("appearance"));
			saveValues(values);
			values.endGroup();
			values.sync();
			const QString configDirectory = QDir::homePath() + QStringLiteral("/.config/synapse");
			QSaveFile background(configDirectory + QStringLiteral("/wallpaper"));
			const QByteArray selected = wallpaper->currentData().toString().toUtf8() + "\n";
			if (!QDir().mkpath(configDirectory) || !background.open(QIODevice::WriteOnly) ||
			    background.write(selected) != selected.size() || !background.commit() ||
			    !applyTheme(theme->currentText())) {
				QMessageBox::warning(this, QStringLiteral("Настройки SynapseOS"),
				    QStringLiteral("Не удалось полностью применить оформление"));
				return;
			}
			status->setText(QStringLiteral("Оформление применено ко всем новым штатным программам"));
		});
		connect(save, &QPushButton::clicked, page, [=]() {
			QString path = QFileDialog::getSaveFileName(page, QStringLiteral("Экспорт темы"),
			    QDir::homePath() + QStringLiteral("/Documents/synapse-theme.ini"),
			    QStringLiteral("Тема SynapseOS (*.ini)"));
			if (path.isEmpty()) {
				return;
			}
			if (!path.endsWith(QStringLiteral(".ini"), Qt::CaseInsensitive)) {
				path += QStringLiteral(".ini");
			}
			QSettings file(path, QSettings::IniFormat);
			saveValues(file);
			file.sync();
			status->setText(file.status() == QSettings::NoError ? QStringLiteral("Тема сохранена: %1").arg(path) :
			    QStringLiteral("Не удалось сохранить тему"));
		});
		connect(load, &QPushButton::clicked, page, [=]() {
			const QString path = QFileDialog::getOpenFileName(page, QStringLiteral("Импорт темы"),
			    QDir::homePath(), QStringLiteral("Тема SynapseOS (*.ini)"));
			if (path.isEmpty()) {
				return;
			}
			QSettings file(path, QSettings::IniFormat);
			const int importedTheme = theme->findText(file.value(QStringLiteral("theme")).toString());
			if (importedTheme < 0) {
				status->setText(QStringLiteral("В файле нет известной базовой темы"));
				return;
			}
			theme->setCurrentIndex(importedTheme);
			fontFamily->setCurrentFont(QFont(file.value(QStringLiteral("font-family"),
			    fontFamily->currentFont().family()).toString()));
			fontSize->setValue(file.value(QStringLiteral("font-size"), fontSize->value()).toInt());
			radius->setValue(file.value(QStringLiteral("corner-radius"), radius->value()).toInt());
			padding->setValue(file.value(QStringLiteral("padding"), padding->value()).toInt());
			sidebar->setValue(file.value(QStringLiteral("sidebar-width"), sidebar->value()).toInt());
			panelHeight->setValue(file.value(QStringLiteral("panel-height"), panelHeight->value()).toInt());
			const QString importedWallpaper = file.value(QStringLiteral("wallpaper")).toString();
			if (wallpaper->findData(importedWallpaper) >= 0) {
				wallpaper->setCurrentIndex(wallpaper->findData(importedWallpaper));
			}
			for (int index = 0; index < colors.size(); ++index) {
				const QString value = file.value(colorKeys[index], colors[index]->property("color")).toString();
				if (!QColor(value).isValid()) {
					status->setText(QStringLiteral("Некорректный цвет в файле темы"));
					return;
				}
				colors[index]->setProperty("color", value);
				colors[index]->setText(value);
			}
			status->setText(QStringLiteral("Тема загружена. Нажмите «Применить оформление»"));
		});
	}

	// Настроить размер экрана и общий предел кадров
	void addDisplayPage(QTabWidget *tabs) {
		auto *page = new QWidget(tabs);
		auto *layout = new QVBoxLayout(page);
		auto *form = new QFormLayout();
		auto *resolution = new QComboBox(page);
		resolution->addItem(QStringLiteral("Автоматически"), QStringLiteral("auto"));
		resolution->addItems({QStringLiteral("800x600"), QStringLiteral("1024x768"),
		    QStringLiteral("1280x720"), QStringLiteral("1280x800"), QStringLiteral("1600x900"),
		    QStringLiteral("1920x1080")});
		struct syn_gpu_info gpu = {};
		int fd = ::open("/dev/fb0", O_RDONLY | O_CLOEXEC);
		const bool pi3 = fd >= 0 && ioctl(fd, SYN_FB_GPU_INFO, &gpu) == 0 &&
		    !strcmp(gpu.name, "BCM2837 VideoCore IV");
		if (fd >= 0) {
			::close(fd);
		}
		if (!pi3) {
			resolution->addItems({QStringLiteral("2560x1440"), QStringLiteral("3840x2160")});
		}
		const int selectedResolution = resolution->findText(readSetting(QStringLiteral("Display.Resolution")));
		resolution->setCurrentIndex(selectedResolution < 0 ? 0 : selectedResolution);
		form->addRow(QStringLiteral("Разрешение"), resolution);

		auto *fps = new QComboBox(page);
		for (int value : {24, 30, 50, 60, 75, 90, 120, 144, 165, 240}) {
			fps->addItem(QStringLiteral("%1 FPS").arg(value), value);
		}
		const long currentFps = syscall(SYS_SYN_GA_VGPU_GET_MAX_FPS);
		const int configuredFps = readSetting(QStringLiteral("Display.FPS")).toInt();
		const int selectedFps = fps->findData(currentFps > 0 ? static_cast<int>(currentFps) : configuredFps);
		fps->setCurrentIndex(selectedFps < 0 ? fps->findData(30) : selectedFps);
		fps->setToolTip(QStringLiteral("Ограничение отрисовки виртуального видеоадаптера, применяется сразу"));
		form->addRow(QStringLiteral("Частота кадров"), fps);

		auto *desktopEnabled = new QCheckBox(QStringLiteral("Запускать графическую сессию при входе"), page);
		desktopEnabled->setChecked(readSetting(QStringLiteral("Desktop.Enabled")) == QStringLiteral("1"));
		form->addRow(QStringLiteral("Рабочий стол"), desktopEnabled);
		auto *login = new QCheckBox(QStringLiteral("Запрашивать пароль при запуске Desktop"), page);
		login->setToolTip(QStringLiteral("Менеджер входа запускает сеанс выбранного пользователя"));
		login->setChecked(readSetting(QStringLiteral("Desktop.LoginManager")) != QStringLiteral("0"));
		login->setEnabled(desktopEnabled->isChecked());
		connect(desktopEnabled, &QCheckBox::toggled, login, &QWidget::setEnabled);
		form->addRow(QStringLiteral("Вход"), login);

		layout->addLayout(form);
		auto *apply = new QPushButton(QStringLiteral("Применить"), page);
		layout->addWidget(apply);
		auto *status = new QLabel(QStringLiteral("Разрешение зависит от видеоустройства. FPS сохраняется для следующих загрузок"), page);
		status->setWordWrap(true);
		layout->addWidget(status);
		layout->addStretch();
		connect(apply, &QPushButton::clicked, this, [=, this]() {
			if (syscall(SYS_SYN_GA_VGPU_SET_MAX_FPS, fps->currentData().toInt()) < 0) {
				status->setText(QStringLiteral("Ядро не поддерживает изменение FPS: %1").arg(QString::fromLocal8Bit(strerror(errno))));
				return;
			}
			if (!writeSetting(this, QStringLiteral("Display.FPS"), fps->currentData().toString()) ||
			    !writeSetting(this, QStringLiteral("Display.Resolution"), resolution->currentIndex() == 0 ?
			        QStringLiteral("auto") : resolution->currentText()) ||
			    !writeSetting(this, QStringLiteral("Desktop.Enabled"), desktopEnabled->isChecked() ? QStringLiteral("1") : QStringLiteral("0")) ||
			    !writeSetting(this, QStringLiteral("Desktop.LoginManager"), login->isChecked() ? QStringLiteral("1") : QStringLiteral("0"))) {
				return;
			}
			status->setText(QStringLiteral("Параметры экрана сохранены, установлено %1 FPS").arg(fps->currentData().toInt()));
		});
		tabs->addTab(page, QStringLiteral("Экран"));
	}

	// Добавить имя компьютера и язык системы
	void buildSystemPage(QWidget *page) {
		QVBoxLayout *layout = new QVBoxLayout(page);
		QFormLayout *form = new QFormLayout();

		QLineEdit *hostname = new QLineEdit(readSetting(QStringLiteral("System.Hostname")), page);
		hostname->setMinimumWidth(240);
		form->addRow(QStringLiteral("Имя компьютера"), hostname);

		QComboBox *language = new QComboBox(page);
		language->addItem(QStringLiteral("Русский"), QStringLiteral("ru"));
		language->addItem(QStringLiteral("Английский"), QStringLiteral("en"));
		int selectedLanguage = language->findData(readSetting(QStringLiteral("System.Language")));
		language->setCurrentIndex(selectedLanguage < 0 ? 0 : selectedLanguage);
		form->addRow(QStringLiteral("Язык"), language);

		form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		layout->addLayout(form);

		QPushButton *apply = new QPushButton(QStringLiteral("Сохранить настройки системы"), page);
		layout->addWidget(apply);
		layout->addWidget(new QLabel(QStringLiteral("Язык применяется в новых сеансах, имя компьютера после перезагрузки."), page));
		layout->addStretch();
		connect(apply, &QPushButton::clicked, this, [this, hostname, language]() {
			if (!hostname->text().trimmed().isEmpty() &&
			    !writeSetting(this, QStringLiteral("System.Hostname"), hostname->text().trimmed())) {
				return;
			}
			writeSetting(this, QStringLiteral("System.Language"), language->currentData().toString());
		});
	}

	// Управление системными службами (SSH, FTP, GuestAgent, телеметрия)
	void buildServicesPage(QWidget *page) {
		QVBoxLayout *layout = new QVBoxLayout(page);
		QFormLayout *form = new QFormLayout();

		QCheckBox *sshOn = new QCheckBox(QStringLiteral("Включить сервер SSH"), page);
		sshOn->setChecked(readSetting(QStringLiteral("SSH.Enabled")) == QStringLiteral("1"));
		QLineEdit *sshPort = new QLineEdit(readSetting(QStringLiteral("SSH.Port")), page);
		form->addRow(QStringLiteral("SSH"), sshOn);
		form->addRow(QStringLiteral("Порт SSH"), sshPort);

		QCheckBox *ftpOn = new QCheckBox(QStringLiteral("Включить сервер FTP"), page);
		ftpOn->setChecked(readSetting(QStringLiteral("FTP.Enabled")) == QStringLiteral("1"));
		QLineEdit *ftpPort = new QLineEdit(readSetting(QStringLiteral("FTP.Port")), page);
		form->addRow(QStringLiteral("FTP"), ftpOn);
		form->addRow(QStringLiteral("Порт FTP"), ftpPort);

		QCheckBox *gaOn = new QCheckBox(QStringLiteral("Включить QEMU Guest Agent"), page);
		gaOn->setChecked(readSetting(QStringLiteral("GuestAgent.Enabled")) == QStringLiteral("1"));
		form->addRow(QStringLiteral("Гостевой агент"), gaOn);

		form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		layout->addLayout(form);

		QPushButton *apply = new QPushButton(QStringLiteral("Сохранить службы"), page);
		layout->addWidget(apply);
		layout->addWidget(new QLabel(QStringLiteral("Параметры служб применяются после перезагрузки."), page));
		layout->addStretch();

		connect(apply, &QPushButton::clicked, this, [this, sshOn, sshPort, ftpOn, ftpPort, gaOn]() {
			if (!writeSetting(this, QStringLiteral("SSH.Enabled"), sshOn->isChecked() ? QStringLiteral("1") : QStringLiteral("0")) ||
			    !writeSetting(this, QStringLiteral("SSH.Port"), sshPort->text().trimmed()) ||
			    !writeSetting(this, QStringLiteral("FTP.Enabled"), ftpOn->isChecked() ? QStringLiteral("1") : QStringLiteral("0")) ||
			    !writeSetting(this, QStringLiteral("FTP.Port"), ftpPort->text().trimmed()) ||
			    !writeSetting(this, QStringLiteral("GuestAgent.Enabled"), gaOn->isChecked() ? QStringLiteral("1") : QStringLiteral("0"))) {
				return;
			}
		});
	}

	// Добавить основные параметры сети без изменения текущего подключения
	void buildNetworkPage(QWidget *page) {
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
		QLineEdit *dnsSecond = new QLineEdit(readSetting(QStringLiteral("DNS.2")), page);
		for (QLineEdit *field : {address, subnet, gateway, dns, dnsSecond}) {
			field->setMinimumWidth(260);
		}
		form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		dns->setToolTip(dns->text());
		dnsSecond->setToolTip(dnsSecond->text());
		form->addRow(QStringLiteral("Адрес IPv4"), address);
		form->addRow(QStringLiteral("Подсеть"), subnet);
		form->addRow(QStringLiteral("Шлюз"), gateway);
		form->addRow(QStringLiteral("Основной сервер DNS"), dns);
		form->addRow(QStringLiteral("Дополнительный сервер DNS"), dnsSecond);
		layout->addLayout(form);

		QPushButton *apply = new QPushButton(QStringLiteral("Сохранить настройки сети"), page);
		layout->addWidget(apply);
		layout->addWidget(new QLabel(QStringLiteral("Настройки сети применяются после перезагрузки."), page));
		layout->addStretch();
		connect(apply, &QPushButton::clicked, this, [this, mode, address, subnet, gateway, dns, dnsSecond]() {
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

			if (!writeSetting(this, QStringLiteral("DNS.1"), dns->text())) {
				return;
			}
			writeSetting(this, QStringLiteral("DNS.2"), dnsSecond->text());
		});
	}

	// Управление обнаружением устройств и обменом в локальной сети
	void buildLanPage(QWidget *page) {
		QVBoxLayout *layout = new QVBoxLayout(page);
		QCheckBox *enabled = new QCheckBox(QStringLiteral("Обнаружение и обмен с устройствами SynapseOS"), page);
		enabled->setChecked(readSetting(QStringLiteral("LanDiscovery.Enabled")) == QStringLiteral("1"));
		layout->addWidget(enabled);

		QHBoxLayout *buttons = new QHBoxLayout();
		QPushButton *scan = new QPushButton(QStringLiteral("Найти устройства"), page);
		QPushButton *message = new QPushButton(QStringLiteral("Сообщение..."), page);
		QPushButton *sendFile = new QPushButton(QStringLiteral("Отправить файл..."), page);
		QPushButton *inbox = new QPushButton(QStringLiteral("Входящие"), page);
		buttons->addWidget(scan);
		buttons->addWidget(message);
		buttons->addWidget(sendFile);
		buttons->addWidget(inbox);
		layout->addLayout(buttons);

		QListWidget *devices = new QListWidget(page);
		devices->setMinimumHeight(160);
		layout->addWidget(devices);

		QLabel *note = new QLabel(QStringLiteral(
		    "Устройство видно соседям, когда настройка включена на обоих. "
		    "Принятые файлы и сообщения лежат в /var/spool/lan/inbox."), page);
		note->setWordWrap(true);
		layout->addWidget(note);
		layout->addStretch();

		connect(enabled, &QCheckBox::toggled, this, [this](bool on) {
			writeSetting(this, QStringLiteral("LanDiscovery.Enabled"),
			             on ? QStringLiteral("1") : QStringLiteral("0"));
		});

		auto runLan = [](const QStringList &args, QString &out) -> bool {
			QProcess process;
			process.start(QStringLiteral("/bin/lan"), args);
			if (!process.waitForStarted(5000) || !process.waitForFinished(30000)) {
				process.kill();
				return false;
			}
			out = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
			return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
		};

		auto selectedIp = [devices]() -> QString {
			auto *item = devices->currentItem();
			return item ? item->data(Qt::UserRole).toString() : QString();
		};

		connect(scan, &QPushButton::clicked, this, [this, devices, runLan]() {
			devices->clear();
			QString out;
			if (!runLan({QStringLiteral("scan")}, out)) {
				QMessageBox::warning(this, QStringLiteral("Локальная сеть"),
				                     QStringLiteral("Не удалось выполнить поиск устройств"));
				return;
			}
			const QStringList lines = out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
			for (const QString &line : lines) {
				const QString trimmed = line.trimmed();
				const qsizetype split = trimmed.indexOf(QStringLiteral("  "));
				if (split <= 0 || !trimmed.contains(QStringLiteral("SynapseOS"))) {
					continue;
				}
				auto *item = new QListWidgetItem(trimmed, devices);
				item->setData(Qt::UserRole, trimmed.left(split).trimmed());
			}
		});

		connect(message, &QPushButton::clicked, this, [this, selectedIp, runLan]() {
			const QString ip = selectedIp();
			if (ip.isEmpty()) {
				QMessageBox::information(this, QStringLiteral("Локальная сеть"),
				                         QStringLiteral("Сначала найдите устройство"));
				return;
			}
			bool accepted = false;
			QString text = QInputDialog::getText(this, QStringLiteral("Сообщение в локальной сети"),
			    QStringLiteral("Текст для %1:").arg(ip), QLineEdit::Normal, QString(), &accepted);
			if (!accepted || text.isEmpty()) {
				return;
			}
			QString out;
			if (!runLan({QStringLiteral("msg"), ip, text}, out)) {
				QMessageBox::warning(this, QStringLiteral("Локальная сеть"),
				                     QStringLiteral("Сообщение не доставлено"));
			}
		});

		connect(sendFile, &QPushButton::clicked, this, [this, selectedIp, runLan]() {
			const QString ip = selectedIp();
			if (ip.isEmpty()) {
				QMessageBox::information(this, QStringLiteral("Локальная сеть"),
				                         QStringLiteral("Сначала найдите устройство"));
				return;
			}
			const QString path = QFileDialog::getOpenFileName(this,
			    QStringLiteral("Файл для отправки"));
			if (path.isEmpty()) {
				return;
			}
			QString out;
			if (!runLan({QStringLiteral("send"), ip, path}, out)) {
				QMessageBox::warning(this, QStringLiteral("Локальная сеть"),
				                     QStringLiteral("Файл не доставлен"));
			}
		});

		connect(inbox, &QPushButton::clicked, this, [this, runLan]() {
			QString out;
			runLan({QStringLiteral("inbox")}, out);
			QMessageBox::information(this, QStringLiteral("Входящие"),
			                         out.isEmpty() ? QStringLiteral("Пусто") : out);
		});
	}

	// Показать вкладку управления беспроводными сетями напрямую через /dev/wifi
	void addWifiPage(QTabWidget *tabs) {
		QWidget *page = new QWidget(tabs);
		QVBoxLayout *layout = new QVBoxLayout(page);

		auto *adapter = new QComboBox(page);
		layout->addWidget(adapter);
		QLabel *statusLabel = new QLabel(QStringLiteral("Статус: проверка..."), page);
		statusLabel->setWordWrap(true);
		layout->addWidget(statusLabel);

		QListWidget *networks = new QListWidget(page);
		networks->setMinimumHeight(170);
		layout->addWidget(networks);

		QFormLayout *form = new QFormLayout();
		QLineEdit *ssidEdit = new QLineEdit(page);
		QLineEdit *passEdit = new QLineEdit(page);
		passEdit->setEchoMode(QLineEdit::Password);
		form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		form->addRow(QStringLiteral("Имя сети (SSID)"), ssidEdit);
		form->addRow(QStringLiteral("Пароль"), passEdit);
		layout->addLayout(form);

		QHBoxLayout *buttons = new QHBoxLayout();
		QPushButton *scanBtn = new QPushButton(QStringLiteral("Сканировать"), page);
		QPushButton *connectBtn = new QPushButton(QStringLiteral("Подключиться"), page);
		QPushButton *disconnectBtn = new QPushButton(QStringLiteral("Отключиться"), page);
		buttons->addWidget(scanBtn);
		buttons->addWidget(connectBtn);
		buttons->addWidget(disconnectBtn);
		layout->addLayout(buttons);

		auto refreshStatus = [adapter, statusLabel]() {
			int fd = ::open(adapter->currentData().toByteArray().constData(), O_RDWR);
			if (fd < 0) {
				statusLabel->setText(QStringLiteral("Выбранный Wi-Fi адаптер недоступен"));
				return;
			}
			struct syn_wifi_status st{};
			st.version = SYN_WIFI_ABI_VERSION;
			int rc = ::ioctl(fd, SYN_WIFI_IOCTL_STATUS, &st);
			::close(fd);
			if (rc < 0) {
				statusLabel->setText(QStringLiteral("Ошибка запроса состояния адаптера"));
				return;
			}
			if (!st.enabled) {
				statusLabel->setText(QStringLiteral("Адаптер выключен"));
				return;
			}
			if (!st.associated || st.ssid_length == 0 || st.ssid_length > SYN_WIFI_SSID_LIMIT) {
				statusLabel->setText(QStringLiteral("Статус: не подключено (адаптер включён)"));
				return;
			}
			const QString ssid = QString::fromUtf8(reinterpret_cast<const char *>(st.ssid), st.ssid_length);
			const QString ip = QStringLiteral("%1.%2.%3.%4")
			    .arg((st.ipv4 >> 24) & 0xff).arg((st.ipv4 >> 16) & 0xff)
			    .arg((st.ipv4 >> 8) & 0xff).arg(st.ipv4 & 0xff);
			statusLabel->setText(QStringLiteral("Подключено: %1  |  Канал: %2  |  IPv4: %3  |  %4 Mb/s")
			    .arg(ssid).arg(st.channel).arg(ip).arg(st.link_mbps));
		};

		auto *scanTimer = new QTimer(page);
		scanTimer->setInterval(250);
		auto refreshScan = [=]() {
			networks->clear();
			int fd = ::open(adapter->currentData().toByteArray().constData(), O_RDWR | O_NONBLOCK);
			if (fd < 0) {
				scanTimer->stop();
				scanBtn->setEnabled(true);
				refreshStatus();
				return;
			}
			struct syn_wifi_scan sc{};
			sc.version = SYN_WIFI_ABI_VERSION;
			int rc = ::ioctl(fd, SYN_WIFI_IOCTL_SCAN, &sc);
			const int error = errno;
			::close(fd);
			if (rc < 0 && error == EAGAIN) {
				statusLabel->setText(QStringLiteral("Поиск сетей…"));
				scanBtn->setEnabled(false);
				scanTimer->start();
				return;
			}
			scanTimer->stop();
			scanBtn->setEnabled(true);
			if (rc >= 0) {
				const unsigned cnt = sc.count <= SYN_WIFI_SCAN_LIMIT ? sc.count : SYN_WIFI_SCAN_LIMIT;
				for (unsigned i = 0; i < cnt; i++) {
					const struct syn_wifi_bss &b = sc.entries[i];
					if (b.ssid_length == 0 || b.ssid_length > SYN_WIFI_SSID_LIMIT) {
						continue;
					}
					const QString ssid = QString::fromUtf8(reinterpret_cast<const char *>(b.ssid), b.ssid_length);
					const QString sec = b.security == SYN_WIFI_SECURITY_WPA2_PSK ?
					    QStringLiteral("WPA2") : QStringLiteral("Open");
					const QString label = QStringLiteral("%1   (%2 dBm, ch %3, %4)")
					    .arg(ssid).arg(static_cast<int>(b.rssi_dbm)).arg(b.channel).arg(sec);
					QListWidgetItem *item = new QListWidgetItem(label, networks);
					item->setData(Qt::UserRole, ssid);
				}
			} else {
				statusLabel->setText(QStringLiteral("Ошибка сканирования: %1")
				    .arg(QString::fromLocal8Bit(std::strerror(error))));
				return;
			}
			refreshStatus();
		};

		connect(scanTimer, &QTimer::timeout, page, refreshScan);
		connect(networks, &QListWidget::itemClicked, this, [ssidEdit](QListWidgetItem *item) {
			if (item) {
				ssidEdit->setText(item->data(Qt::UserRole).toString());
			}
		});

		connect(scanBtn, &QPushButton::clicked, this, [refreshScan]() {
			refreshScan();
		});

		connect(connectBtn, &QPushButton::clicked, this, [this, adapter, ssidEdit, passEdit, statusLabel, refreshStatus]() {
			const QByteArray ssidBytes = ssidEdit->text().trimmed().toUtf8();
			const QByteArray passBytes = passEdit->text().toUtf8();
			if (ssidBytes.isEmpty() || ssidBytes.size() > static_cast<int>(SYN_WIFI_SSID_LIMIT)) {
				QMessageBox::warning(this, QStringLiteral("Wi-Fi"), QStringLiteral("Некорректное имя сети (SSID)"));
				return;
			}
			if (passBytes.size() > static_cast<int>(SYN_WIFI_PASSWORD_LIMIT)) {
				QMessageBox::warning(this, QStringLiteral("Wi-Fi"), QStringLiteral("Пароль слишком длинный"));
				return;
			}
			int fd = ::open(adapter->currentData().toByteArray().constData(), O_RDWR);
			if (fd < 0) {
				QMessageBox::warning(this, QStringLiteral("Wi-Fi"), QStringLiteral("Выбранный адаптер недоступен"));
				return;
			}
			struct syn_wifi_connect conn{};
			conn.version = SYN_WIFI_ABI_VERSION;
			conn.ssid_length = static_cast<uint8_t>(ssidBytes.size());
			std::memcpy(conn.ssid, ssidBytes.constData(), conn.ssid_length);
			conn.password_length = static_cast<uint8_t>(passBytes.size());
			if (conn.password_length > 0) {
				std::memcpy(conn.password, passBytes.constData(), conn.password_length);
			}
			int rc = ::ioctl(fd, SYN_WIFI_IOCTL_CONNECT, &conn);
			::close(fd);
			if (rc < 0) {
				QMessageBox::warning(this, QStringLiteral("Wi-Fi"), QStringLiteral("Ошибка отправки команды подключения"));
				return;
			}
			// Атомарно сохранить профиль в /etc/network/wifi.conf с правами 0600
			if (!QDir().mkpath(QStringLiteral("/etc/network"))) {
				QMessageBox::warning(this, QStringLiteral("Wi-Fi"),
				                     QStringLiteral("Не удалось создать каталог /etc/network"));
				return;
			}
			const QByteArray hexConf = "ssid=" + ssidBytes.toHex() + "\npassword=" + passBytes.toHex() + "\n";
			const QByteArray profilePath = "/etc/network/" + adapter->currentData().toByteArray().mid(5) + ".conf";
			const QByteArray tmpPath = profilePath + ".tmp." + QByteArray::number(::getpid());
			::unlink(tmpPath.constData());
			int pfd = ::open(tmpPath.constData(), O_WRONLY | O_CREAT | O_EXCL, 0600);
			bool saved = false;
			if (pfd >= 0) {
				size_t written = 0;
				const size_t total = static_cast<size_t>(hexConf.size());
				bool writeOk = true;
				while (written < total) {
					ssize_t n = ::write(pfd, hexConf.constData() + written, total - written);
					if (n <= 0) {
						writeOk = false;
						break;
					}
					written += static_cast<size_t>(n);
				}
				if (writeOk && ::fchmod(pfd, 0600) == 0 && ::fsync(pfd) == 0 && ::close(pfd) == 0) {
					if (::rename(tmpPath.constData(), profilePath.constData()) == 0) {
						int dfd = ::open("/etc/network", O_RDONLY);
						if (dfd >= 0) {
							::fsync(dfd);
							::close(dfd);
						}
						saved = true;
					}
				} else {
					::close(pfd);
				}
				if (!saved) {
					::unlink(tmpPath.constData());
				}
			}
			if (!saved) {
				QMessageBox::warning(this, QStringLiteral("Wi-Fi"),
				                     QStringLiteral("Не удалось сохранить профиль выбранного адаптера"));
			}
			statusLabel->setText(QStringLiteral("Подключение к %1...").arg(QString::fromUtf8(ssidBytes)));
			QTimer::singleShot(2500, this, refreshStatus);
			QTimer::singleShot(5000, this, refreshStatus);
		});

		connect(disconnectBtn, &QPushButton::clicked, this, [adapter, refreshStatus]() {
			int fd = ::open(adapter->currentData().toByteArray().constData(), O_RDWR);
			if (fd >= 0) {
				uint32_t ver = SYN_WIFI_ABI_VERSION;
				::ioctl(fd, SYN_WIFI_IOCTL_DISCONNECT, &ver);
				::close(fd);
			}
			refreshStatus();
		});

		auto refreshAdapters = [=]() {
			const QByteArray selected = adapter->currentData().toByteArray();
			QList<QByteArray> paths;
			for (const QByteArray &path : {QByteArray("/dev/wifi-usb"), QByteArray("/dev/wifi-onboard")}) {
				const int fd = ::open(path.constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
				if (fd >= 0) {
					::close(fd);
					paths.append(path);
				}
			}
			if (paths.isEmpty() && QFileInfo::exists(QStringLiteral("/dev/wifi"))) {
				paths.append("/dev/wifi");
			}
			QList<QByteArray> listed;
			for (int i = 0; i < adapter->count(); ++i) {
				listed.append(adapter->itemData(i).toByteArray());
			}
			if (paths != listed) {
				const QSignalBlocker blocker(adapter);
				adapter->clear();
				for (const QByteArray &path : paths) {
					const QString label = path == "/dev/wifi-usb" ? QStringLiteral("USB Wi-Fi") :
					    path == "/dev/wifi-onboard" ? QStringLiteral("Встроенный Wi-Fi") : QStringLiteral("Wi-Fi");
					adapter->addItem(label, path);
				}
				if (paths.contains(selected)) {
					adapter->setCurrentIndex(adapter->findData(selected));
				} else {
					scanTimer->stop();
					networks->clear();
					ssidEdit->clear();
					passEdit->clear();
				}
			}
			const bool present = !paths.isEmpty();
			scanBtn->setEnabled(present && !scanTimer->isActive());
			connectBtn->setEnabled(present);
			disconnectBtn->setEnabled(present);
			if (!present) {
				statusLabel->setText(QStringLiteral("Wi-Fi адаптеры не обнаружены"));
			} else if (!scanTimer->isActive() && paths != listed) {
				refreshStatus();
			}
		};
		connect(adapter, &QComboBox::currentIndexChanged, page, [=]() {
			scanTimer->stop();
			scanBtn->setEnabled(true);
			networks->clear();
			ssidEdit->clear();
			passEdit->clear();
			refreshStatus();
		});
		auto *deviceTimer = new QTimer(page);
		deviceTimer->setInterval(3000);
		connect(deviceTimer, &QTimer::timeout, page, refreshAdapters);
		const int index = tabs->addTab(page, QStringLiteral("Wi-Fi"));
		connect(tabs, &QTabWidget::currentChanged, page, [=](int current) {
			if (current == index) {
				refreshAdapters();
				if (adapter->count() && !scanTimer->isActive()) refreshStatus();
				deviceTimer->start();
			} else {
				deviceTimer->stop();
			}
		});
	}

	// Показать краткую справку о доступных действиях на рабочем столе
    static QByteArray readReport(const QString &name) {
        QFile source(name);
        return source.open(QIODevice::ReadOnly) ? source.read(16384) : QByteArray();
    }

    void buildSoundPage(QWidget *page) {
        auto *form = new QFormLayout(page);
        auto *device = new QComboBox(page);
        auto *gain = new QSlider(Qt::Horizontal, page);
        gain->setRange(0, 100);
        auto *level = new QLabel(page);
        auto *mute = new QCheckBox(QStringLiteral("Выключить звук"), page);
        auto *apply = new QPushButton(QStringLiteral("Применить"), page);
        auto *status = new QLabel(page);
        status->setWordWrap(true);
        form->addRow(QStringLiteral("Выход"), device);
        auto *volumeRow = new QHBoxLayout;
        volumeRow->addWidget(gain);
        volumeRow->addWidget(level);
        form->addRow(QStringLiteral("Общая громкость"), volumeRow);
        form->addRow(mute);
        form->addRow(apply);
        form->addRow(status);
        auto displayGain = [level](int value) { level->setText(QString::number(value) + "%"); };
        connect(gain, &QSlider::valueChanged, page, displayGain);
        const QString control = QStringLiteral("/proc/audio-settings");
        const auto current = readReport(control).simplified().split(' ');
        if (current.size() == 3) {
            device->addItem(QStringLiteral("Автоматически"), "auto");
            if (QFileInfo::exists("/dev/audio-hdmi")) device->addItem(QStringLiteral("HDMI"), "hdmi");
            if (QFileInfo::exists("/dev/audio-jack")) device->addItem(QStringLiteral("Наушники 3,5 мм"), "jack");
            device->setCurrentIndex(qMax(0, device->findData(QString::fromLatin1(current[0]))));
            gain->setValue(current[1].toInt());
            mute->setChecked(current[2] == "1");
            displayGain(gain->value());
            status->setText(QStringLiteral("Громкость применяется к воспроизведению. Выход меняется при следующем открытии системного звукового устройства."));
            connect(apply, &QPushButton::clicked, page, [=]() {
                const QByteArray value = device->currentData().toByteArray() + " " + QByteArray::number(gain->value()) +
                                         (mute->isChecked() ? " 1\n" : " 0\n");
                QSaveFile saved("/etc/audio.conf");
                if (!saved.open(QIODevice::WriteOnly) || saved.write(value) != value.size()) {
                    status->setText(saved.errorString());
                    return;
                }
                const QByteArray before = readReport(control);
                auto set = [&](const QByteArray &data) {
                    QFile backend(control);
                    return backend.open(QIODevice::WriteOnly | QIODevice::Unbuffered) && backend.write(data) == data.size();
                };
                if (!set(value)) { status->setText(QStringLiteral("Ядро отклонило параметры звука")); return; }
                if (!saved.commit()) { set(before); status->setText(saved.errorString()); return; }
                status->setText(QStringLiteral("Параметры звука применены и сохранены"));
            });
            return;
        }
        // PulseAudio управляет системными выходами на настольных сборках с запущенным сервером
        auto pulse = [](const QStringList &args, QByteArray *output) {
            QProcess cmd;
            auto env = QProcessEnvironment::systemEnvironment();
            env.insert("LC_ALL", "C");
            cmd.setProcessEnvironment(env);
            cmd.start("/bin/pactl", args);
            if (!cmd.waitForFinished(1200) || cmd.exitCode() || cmd.exitStatus() != QProcess::NormalExit) return false;
            if (output) *output = cmd.readAllStandardOutput().trimmed();
            return true;
        };
        QByteArray sinks, selected;
        if (pulse({"list", "short", "sinks"}, &sinks) && pulse({"get-default-sink"}, &selected)) {
            for (const auto &row : sinks.split('\n')) {
                const auto fields = row.split('\t');
                if (fields.size() >= 2) device->addItem(QString::fromUtf8(fields[1]), fields[1]);
            }
            device->setCurrentIndex(qMax(0, device->findData(selected)));
        }
        auto refresh = [=]() {
            QByteArray volume, muted;
            QString sink = device->currentData().toString();
            bool ok = !sink.isEmpty() && pulse({"get-sink-volume", sink}, &volume) && pulse({"get-sink-mute", sink}, &muted);
            if (ok) {
                int pct = volume.indexOf('%');
                int first = pct;
                while (first > 0 && volume[first - 1] >= '0' && volume[first - 1] <= '9') --first;
                ok = pct > first && (muted.endsWith("yes") || muted.endsWith("no"));
                if (ok) { gain->setValue(volume.mid(first, pct - first).toInt()); mute->setChecked(muted.endsWith("yes")); }
            }
            displayGain(gain->value());
            gain->setEnabled(ok); mute->setEnabled(ok); apply->setEnabled(ok);
            status->setText(ok ? QString() : QStringLiteral("Системный звуковой выход недоступен"));
        };
        connect(device, &QComboBox::currentIndexChanged, page, refresh);
        refresh();
        connect(apply, &QPushButton::clicked, page, [=]() {
            QString sink = device->currentData().toString();
            bool ok = pulse({"set-sink-volume", sink, QString::number(gain->value()) + "%"}, nullptr) &&
                      pulse({"set-sink-mute", sink, mute->isChecked() ? "1" : "0"}, nullptr) &&
                      pulse({"set-default-sink", sink}, nullptr);
            refresh();
            status->setText(ok ? QStringLiteral("Параметры звука применены") : QStringLiteral("Ошибка управления PulseAudio"));
        });
    }

    void buildAboutPage(QWidget *page) {
        auto *layout = new QVBoxLayout(page);
        auto *summary = new QPlainTextEdit(page);
        summary->setReadOnly(true);
        summary->setAccessibleName(QStringLiteral("Оборудование и датчики"));
        summary->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        layout->addWidget(summary);
        auto *update = new QPushButton(QStringLiteral("Обновить показания"), page);
        layout->addWidget(update);
        auto refresh = [summary]() {
            QStringList lines;
            lines << QStringLiteral("SynapseOS · Labwc и Qt 6") << QSysInfo::currentCpuArchitecture();
            auto fields = [](const QByteArray &text) {
                QHash<QByteArray, QByteArray> values;
                for (const auto &line : text.split('\n')) {
                    auto items = line.simplified().split(' ');
                    if (items.size() >= 2) values.insert(items[0], items[1]);
                }
                return values;
            };
            unsigned cores = 0;
            QByteArray model, implementer, part;
            for (const auto &row : readReport("/proc/cpuinfo").split('\n')) {
                int colon = row.indexOf(':');
                if (colon < 0) continue;
                QByteArray key = row.left(colon).trimmed(), value = row.mid(colon + 1).trimmed();
                if (key == "processor") ++cores;
                if (key == "model name") model = value;
                if (key == "CPU implementer") implementer = value;
                if (key == "CPU part") part = value;
            }
            if (model.isEmpty() && implementer == "0x41" && part == "0xd03") model = "ARM Cortex-A53";
            if (!model.isEmpty()) lines << QString::fromUtf8(model);
            if (cores) lines << QStringLiteral("Логических процессоров: %1").arg(cores);
            const auto sd = fields(readReport("/proc/storage"));
            if (sd.value("clock_hz").toUInt()) lines << QStringLiteral("microSD: %1 МГц, шина %2 бит")
                .arg(sd.value("clock_hz").toUInt() / 1000000).arg(sd.value("bus_width").toUInt());
            const auto memory = fields(readReport("/proc/meminfo"));
            if (memory.contains("MemTotal:")) {
                lines << QStringLiteral("ОЗУ: %1 МиБ всего, %2 МиБ доступно")
                    .arg(memory["MemTotal:"].toULongLong() / 1024).arg(memory["MemAvailable:"].toULongLong() / 1024);
            }
            lines << "" << QStringLiteral("Датчики температуры");
            const auto zones = readReport("/proc/thermal").trimmed().split('\n');
            if (zones.size() <= 1) lines << QStringLiteral("Нет показаний");
            for (const auto &zone : zones.mid(1)) {
                auto part = zone.simplified().split(' ');
                if (part.size() == 3) {
                    bool valid = false;
                    double degrees = part[1].toLongLong(&valid) / 1000.0;
                    lines << QString::fromUtf8(part[0]) + "   " +
                        (part[2] == "0" && valid ? QString::number(degrees, 'f', 1) + QStringLiteral(" °C") : QStringLiteral("нет показания"));
                }
            }
            const auto clocks = fields(readReport("/proc/power"));
            const auto rate = clocks.value("arm_measured_hz").toULongLong();
            if (rate) lines << QStringLiteral("Частота CPU: %1 МГц (диапазон %2-%3 МГц)")
                .arg(rate / 1000000).arg(clocks.value("arm_min_hz").toULongLong() / 1000000)
                .arg(clocks.value("arm_max_hz").toULongLong() / 1000000);
            if (clocks.value("firmware_throttle_valid") == "1") {
                const auto flags = clocks.value("firmware_throttle_flags").toUInt(nullptr, 0);
                auto yes = [](bool active) { return active ? QStringLiteral("да") : QStringLiteral("нет"); };
                lines << QStringLiteral("Пониженное напряжение: сейчас %1, с момента загрузки %2").arg(yes(flags & 1), yes(flags & (1u << 16)));
                lines << QStringLiteral("Троттлинг: сейчас %1, с момента загрузки %2").arg(yes(flags & 4), yes(flags & (1u << 18)));
            }
            auto media = readReport("/proc/media").trimmed();
            if (!media.isEmpty()) lines << "" << QStringLiteral("Дисплей и звук") << QString::fromUtf8(media);
            lines << "" << QStringLiteral("Print Screen: снимок экрана. Alt+Tab: переключение окон. Alt+Shift: раскладка.");
            summary->setPlainText(lines.join('\n'));
        };
        connect(update, &QPushButton::clicked, page, refresh);
        refresh();
    }
};

// Открыть настройки в уже загруженной Qt-сессии меню и освободить окно при закрытии
void MenuWindow::openSettings() {
	if (!settingsWindow) {
		settingsWindow = new SettingsWindow();
		settingsWindow->setAttribute(Qt::WA_DeleteOnClose);
	}
	settingsWindow->showNormal();
	settingsWindow->raise();
	settingsWindow->activateWindow();
	hide();
}


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
	int error = errno;
	close(descriptor);
	errno = error;
	return delivered;
}

// Открыть сокет меню в каталоге сеанса, сохранив действующую службу при повторном запуске
static int listenMenu(QByteArray &path, char command, bool &delivered) {
	const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR");
	if (runtime.isEmpty()) {
		return -1;
	}

	path = runtime + "/synapse-desktop-menu";
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	if (size_t(path.size()) >= sizeof(address.sun_path)) {
		return -1;
	}
	std::memcpy(address.sun_path, path.constData(), size_t(path.size()) + 1);

	const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -1;
	}
	auto bindSocket = [&]() { return bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)); };
	if (bindSocket() < 0) {
		if (errno != EADDRINUSE && errno != EEXIST) {
			close(fd);
			return -1;
		}
		delivered = notifyMenu(command);
		struct stat st{};
		if (delivered || errno != ECONNREFUSED || lstat(path.constData(), &st) < 0 ||
		    !S_ISSOCK(st.st_mode) || unlink(path.constData()) < 0 || bindSocket() < 0) {
			close(fd);
			return -1;
		}
	}
	if (listen(fd, 8) < 0) {
		unlink(path.constData());
		close(fd);
		return -1;
	}
	return fd;
}

// Qt следит за готовностью соединений, не задерживая отрисовку окна
static void watchMenu(int fd, QMainWindow *window) {
	auto *listener = new QSocketNotifier(fd, QSocketNotifier::Read, window);
	QObject::connect(listener, &QSocketNotifier::activated, window, [fd, window]() {
		int client = accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (client < 0) {
			return;
		}

		auto *input = new QSocketNotifier(client, QSocketNotifier::Read, window);
		QObject::connect(input, &QObject::destroyed, [client]() { close(client); });
		QObject::connect(input, &QSocketNotifier::activated, input, [client, input, window]() {
			char command;
			const ssize_t n = recv(client, &command, 1, 0);
			if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
				return;
			}
			input->setEnabled(false);
			input->deleteLater();
			if (n == 1 && command == 'T') {
				if (window->isVisible()) {
					window->hide();
				} else {
					window->show();
					window->raise();
					window->activateWindow();
				}
			}
		});
		QTimer::singleShot(1000, input, &QObject::deleteLater);
	});
}

// В тесте записываем монотонные отметки и печатаем их только после закрытия окна
static long long startupMicros(bool enabled) {
    if (!enabled) return 0;
    struct timespec stamp = {};
    if (clock_gettime(CLOCK_MONOTONIC, &stamp) != 0) return 0;
    return stamp.tv_sec * 1000000LL + stamp.tv_nsec / 1000;
}

// Выбрать меню программ либо окно настроек и проверить его в госте
int main(int argc, char **argv) {
	const bool timing = std::getenv("SYNAPSE_PI_WINDOW_TIMING") != nullptr;
	const auto entered = startupMicros(timing);
	const bool notificationClient = argc == 4 && std::strcmp(argv[1], "--notify") == 0;
	bool settingsMode = false;
	bool monitorMode = false;
	QString subsystem;
	bool selfTest = false;
	bool serviceMode = false;
	for (int index = 1; index < argc; index++) {
		settingsMode |= std::strcmp(argv[index], "--settings") == 0;
		monitorMode |= std::strcmp(argv[index], "--monitor") == 0;
		if (std::strncmp(argv[index], "--subsystem=", 12) == 0) {
			subsystem = QString::fromUtf8(argv[index] + 12);
			settingsMode = true;
		}
		selfTest |= std::strcmp(argv[index], "--self-test") == 0;
		serviceMode |= std::strcmp(argv[index], "--menu-service") == 0;
	}

	if (!notificationClient && !settingsMode && !monitorMode && !selfTest && !serviceMode && notifyMenu('T')) {
		return 0;
	}

	QApplication application(argc, argv);
	const auto qtReady = startupMicros(timing);
	application.setStyle(QStringLiteral("Fusion"));
	applyDesktopPalette(desktopTheme());
	if (notificationClient) {
		auto request = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"),
		    QStringLiteral("/org/freedesktop/Notifications"), QStringLiteral("org.freedesktop.Notifications"),
		    QStringLiteral("Notify"));
		request.setArguments({QStringLiteral("synapse-desktop"), 0U, QString(), QString::fromUtf8(argv[2]),
		    QString::fromUtf8(argv[3]), QStringList(), QVariantMap(), -1});
		const QDBusMessage reply = QDBusConnection::sessionBus().call(request, QDBus::Block, 5000);
		if (reply.type() != QDBusMessage::ReplyMessage) {
			std::fprintf(stderr, "%s: %s\n", qPrintable(reply.errorName()), qPrintable(reply.errorMessage()));
			return 1;
		}
		return 0;
	}
	auto *notifications = new DesktopNotifications(&application);
	if (serviceMode && !notifications->start()) {
		std::fprintf(stderr, "D-Bus notification service is unavailable\n");
	}
	QByteArray menuPath;
	bool delivered = false;
	int menuFd = (!settingsMode && !monitorMode && !selfTest) ?
	    listenMenu(menuPath, serviceMode ? 'P' : 'T', delivered) : -1;
	if (delivered) {
		return 0;
	}
	bool persistent = menuFd >= 0;
	if (serviceMode && !persistent) {
		return 1;
	}
	application.setQuitOnLastWindowClosed(!persistent);
	const auto paletteReady = startupMicros(timing);
	QMainWindow *window = monitorMode ? static_cast<QMainWindow *>(new MonitorWindow()) :
	    settingsMode ? static_cast<QMainWindow *>(new SettingsWindow(subsystem)) :
	                   static_cast<QMainWindow *>(new MenuWindow());
	const auto built = startupMicros(timing);
	if (!serviceMode) {
		window->show();
	}
	const auto shown = startupMicros(timing);
	if (persistent) {
		watchMenu(menuFd, window);
	}

	if (selfTest) {
		QTimer::singleShot(1500, &application, [&application, window, settingsMode, monitorMode]() {
			bool ready = monitorMode ? static_cast<MonitorWindow *>(window)->ready() :
			    settingsMode ? static_cast<SettingsWindow *>(window)->ready() :
			                   static_cast<MenuWindow *>(window)->ready();

			if (!ready) {
				std::puts("SYNAPSE_DESKTOP_FAIL");
				application.exit(1);
				return;
			}

			std::puts("SYNAPSE_DESKTOP_PASS");
			application.quit();
		});
	}

	const int result = application.exec();
	if (timing && settingsMode) {
		std::printf("SETTINGS_PHASE pid=%ld main_us=%lld qt_us=%lld palette_us=%lld built_us=%lld show_us=%lld\n",
		            static_cast<long>(getpid()), entered, qtReady, paletteReady, built, shown);
	}
	if (persistent) {
		unlink(menuPath.constData());
		close(menuFd);
	}
	return result;
}
