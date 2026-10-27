/*
    main.cpp: Меню программ и графические настройки SynapseOS
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI: synapse-x86_64-v1

    Описание: Запуск установленных программ и изменение общих настроек системы
    Подсистема: рабочий стол
    Первое изменение: 25.10.2026
    Последнее изменение: 27.10.2026
    Доступ: общедоступный
*/

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdio>
#include <iterator>

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
	{"Монитор системы", "Система", "/bin/weston-terminal", "--shell=/bin/htop", nullptr},
	{"Mindustry", "Игры", "/bin/mindustry", nullptr, "/usr/share/weston/mindustry-icon.png"},
};

// Прочитать видимое приложение из установленного Flatpak
static QString flatpakValue(const QString &path, const QString &key) {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return QString();
	}

	bool desktopEntry = false;
	while (!file.atEnd()) {
		QString line = QString::fromUtf8(file.readLine()).trimmed();
		if (line.startsWith(QLatin1Char('['))) {
			desktopEntry = line == QStringLiteral("[Desktop Entry]");
			continue;
		}

		if (desktopEntry && line.startsWith(key + QLatin1Char('='))) {
			return line.mid(key.size() + 1);
		}
	}

	return QString();
}

// Найти значок среди размеров, экспортированных Flatpak
static QIcon flatpakIcon(const QString &name) {
	if (name.startsWith(QLatin1Char('/'))) {
		return QIcon(name);
	}

	const QString root = QStringLiteral("/var/lib/flatpak/exports/share/icons/hicolor/");
	const QStringList sizes = {QStringLiteral("128x128"), QStringLiteral("64x64"),
	                           QStringLiteral("48x48"), QStringLiteral("256x256"),
	                           QStringLiteral("32x32"), QStringLiteral("16x16")};
	for (const QString &size : sizes) {
		for (const QString &suffix : {QStringLiteral(".png"), QStringLiteral(".svg")}) {
			QString path = root + size + QStringLiteral("/apps/") + name + suffix;
			if (QFileInfo::exists(path)) {
				return QIcon(path);
			}
		}
	}

	return QIcon::fromTheme(name);
}

// Прочитать настройку через штатную программу без самостоятельной записи INI
static QString readSetting(const QString &key) {
	QProcess command;
	command.start(QStringLiteral("/bin/settings"), {QStringLiteral("--get"), key});

	if (!command.waitForFinished(5000) || command.exitCode() != 0) {
		return QString();
	}

	return QString::fromUtf8(command.readAllStandardOutput()).trimmed();
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

class MenuWindow final : public QMainWindow {
public:
	// Показать только установленные приложения и поле поиска
	MenuWindow() {
		setWindowTitle(QStringLiteral("Программы SynapseOS"));
		setWindowIcon(QIcon(QStringLiteral("/usr/share/weston/synapse-logo.png")));
		resize(520, 500);

		QWidget *content = new QWidget(this);
		QVBoxLayout *layout = new QVBoxLayout(content);
		QLabel *heading = new QLabel(QStringLiteral("SynapseOS"), content);
		QFont font = heading->font();
		font.setPointSize(20);
		font.setBold(true);
		heading->setFont(font);
		layout->addWidget(heading);

		search = new QLineEdit(content);
		search->setPlaceholderText(QStringLiteral("Поиск программ"));
		layout->addWidget(search);

		list = new QListWidget(content);
		layout->addWidget(list);
		setCentralWidget(content);

		connect(search, &QLineEdit::textChanged, this, [this]() { refresh(); });
		connect(list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
			launch(item);
		});
		refresh();
		search->setFocus();
	}

	// Проверить наличие хотя бы одной доступной программы
	bool ready() const {
		return list->count() > 0;
	}

private:
	QLineEdit *search = nullptr;
	QListWidget *list = nullptr;

	// Перестроить список после изменения строки поиска
	void refresh() {
		list->clear();
		QString query = search->text();

		for (int index = 0; index < static_cast<int>(std::size(applications)); index++) {
			const ApplicationEntry &entry = applications[index];
			QString title = QString::fromUtf8(entry.name);

			if (!QFileInfo(QString::fromUtf8(entry.path)).isExecutable() ||
			    !title.contains(query, Qt::CaseInsensitive)) {
				continue;
			}

			QIcon icon;
			if (entry.icon != nullptr) {
				icon = QIcon(QString::fromUtf8(entry.icon));
			}

			QListWidgetItem *item = new QListWidgetItem(
			    icon, QStringLiteral("%1  ·  %2").arg(title, QString::fromUtf8(entry.category)), list);
			item->setData(Qt::UserRole, index);
		}

		if (!QFileInfo(QStringLiteral("/bin/flatpak")).isExecutable()) {
			return;
		}

		const QDir exports(QStringLiteral("/var/lib/flatpak/exports/share/applications"));
		for (const QFileInfo &file : exports.entryInfoList({QStringLiteral("*.desktop")}, QDir::Files)) {
			QString path = file.absoluteFilePath();
			QString id = file.fileName();
			id.chop(QStringLiteral(".desktop").size());
			QString title = flatpakValue(path, QStringLiteral("Name[ru]"));
			if (title.isEmpty()) {
				title = flatpakValue(path, QStringLiteral("Name"));
			}

			if (title.isEmpty() || !title.contains(query, Qt::CaseInsensitive) ||
			    flatpakValue(path, QStringLiteral("Type")) != QStringLiteral("Application") ||
			    flatpakValue(path, QStringLiteral("Terminal")) == QStringLiteral("true") ||
			    flatpakValue(path, QStringLiteral("Hidden")) == QStringLiteral("true") ||
			    flatpakValue(path, QStringLiteral("NoDisplay")) == QStringLiteral("true")) {
				continue;
			}

			QIcon icon = flatpakIcon(flatpakValue(path, QStringLiteral("Icon")));
			QListWidgetItem *item = new QListWidgetItem(
			    icon, QStringLiteral("%1  ·  Flatpak").arg(title), list);
			item->setData(Qt::UserRole, id);
		}
	}

	// Запустить выбранную программу отдельным процессом
	void launch(QListWidgetItem *item) {
		bool isBuiltIn = false;
		int index = item->data(Qt::UserRole).toInt(&isBuiltIn);
		if (!isBuiltIn) {
			QString id = item->data(Qt::UserRole).toString();
			QProcess process;
			QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
			if (!environment.contains(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"))) {
				environment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
				                   QStringLiteral("unix:path=/var/run/weston-runtime/bus"));
			}

			process.setProcessEnvironment(environment);
			if (!process.startDetached(QStringLiteral("/bin/flatpak"),
			                           {QStringLiteral("run"), QStringLiteral("--system"),
			                            QStringLiteral("--socket=session-bus"),
			                            QStringLiteral("--no-a11y-bus"), id})) {
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
		layout->addWidget(new QLabel(QStringLiteral("Рабочий стол SynapseOS\nWeston и Qt 6"), page));
		layout->addWidget(new QLabel(
		    QStringLiteral("Print Screen сохраняет PNG в домашнем каталоге.\n"
		                   "Alt+Tab переключает окна. Alt+Shift меняет раскладку клавиатуры."), page));
		layout->addStretch();
		tabs->addTab(page, QStringLiteral("О системе"));
	}
};

// Выбрать меню программ либо окно настроек и проверить его в госте
int main(int argc, char **argv) {
	QApplication application(argc, argv);
	QStringList arguments = application.arguments();
	bool settingsMode = arguments.contains(QStringLiteral("--settings"));
	QMainWindow *window = settingsMode ? static_cast<QMainWindow *>(new SettingsWindow())
	                                 : static_cast<QMainWindow *>(new MenuWindow());
	window->show();

	if (arguments.contains(QStringLiteral("--self-test"))) {
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
