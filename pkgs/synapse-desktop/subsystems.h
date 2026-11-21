// subsystems.h: Настройки установленных подсистем SynapseOS Desktop
// Автор: Елчинян Арен Артурович, 2026. AMD64, ABI Qt6 и POSIX
// Подсистема: графическая оболочка. Первое и последнее изменение: 21.11.2026. Общедоступно
#pragma once
#include "../labwc/posix-profile.h"
#include <QRegularExpression>
#include <QFileDialog>
#include <QInputDialog>
#include <QScrollArea>
#include <QSpinBox>
#include <QSignalBlocker>
#include <climits>
#include <sys/file.h>

// Записывать только существующий файл, настройки Android разделяются с работающими окнами
static bool writeSubsystemFile(const QString &path, const QByteArray &bytes, bool profile = false) {
	QFile file(path);
	if (!file.open(QIODevice::ReadWrite | QIODevice::ExistingOnly | QIODevice::Unbuffered)) {
		return false;
	}
	if (profile && flock(file.handle(), LOCK_EX) != 0) {
		return false;
	}
	if (file.write(bytes) != bytes.size() || !file.flush()) {
		return false;
	}
	return !profile || (file.resize(bytes.size()) && fsync(file.handle()) == 0);
}

// Сохранить профиль POSIX для следующего графического входа пользователя
static void posixControls(QWidget *page, QFormLayout *form) {
	const QString home = QDir::homePath();
	const posix_profile profile = read_posix_profile(QFile::encodeName(home).constData());
	auto *mask = new QLineEdit(page);
	mask->setPlaceholderText(QStringLiteral("Наследовать"));
	mask->setMaxLength(3);
	if (profile.mask >= 0) {
		mask->setText(QString::number(profile.mask, 8).rightJustified(3, '0'));
	}
	form->addRow(QStringLiteral("Маска создания файлов (umask)"), mask);
	struct rlimit limit;
	auto *files = new QSpinBox(page);
	const bool limits = getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_max >= 128;
	files->setRange(0, limits ? int(qMin<rlim_t>(INT_MAX, limit.rlim_max)) : 0);
	files->setSpecialValueText(QStringLiteral("Наследовать"));
	files->setValue(profile.files);
	files->setEnabled(limits);
	form->addRow(QStringLiteral("Открытых файлов на процесс"), files);
	auto *apply = new QPushButton(QStringLiteral("Сохранить для следующего входа"), page);
	form->addRow(apply);
	QObject::connect(apply, &QPushButton::clicked, page, [=]() {
		const QString text = mask->text().trimmed();
		bool valid = false;
		const int mode = text.isEmpty() ? -1 : text.toInt(&valid, 8);
		if ((!text.isEmpty() && (!valid || text.contains(QRegularExpression("[^0-7]")))) ||
		    (files->value() && files->value() < 128)) {
			QMessageBox::warning(page, "POSIX", QStringLiteral("Укажите маску от 000 до 777 и лимит от 128 или выберите наследование."));
			return;
		}
		char path[128];
		posix_profile_path(path);
		QSaveFile file(QString::fromLatin1(path));
		const QByteArray data = "mask=" + QByteArray::number(mode) + "\nfiles=" +
		                        QByteArray::number(files->value()) + "\n";
		if (!file.open(QIODevice::WriteOnly) ||
		    !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
		    file.write(data) != data.size() || !file.commit()) {
			QMessageBox::warning(page, "POSIX", QStringLiteral("Не удалось сохранить профиль сеанса."));
		}
	});

	const char *keys[] = {"rmem_default", "rmem_max", "wmem_default", "wmem_max"};
	const char *labels[] = {"Буфер приёма по умолчанию", "Предельный буфер приёма",
	                        "Буфер передачи по умолчанию", "Предельный буфер передачи"};
	for (int i = 0; i < 4; i++) {
		const QString path = QStringLiteral("/proc/sys/net/core/") + keys[i];
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly)) {
			continue;
		}
		bool valid;
		const int current = file.read(32).trimmed().toInt(&valid);
		if (!valid) {
			continue;
		}
		auto *value = new QSpinBox(page);
		value->setRange(1, INT_MAX);
		value->setValue(current);
		value->setSuffix(QStringLiteral(" Б"));
		value->setEnabled(geteuid() == 0 && QFileInfo(path).isWritable());
		form->addRow(QString::fromUtf8(labels[i]), value);
		QObject::connect(value, &QSpinBox::editingFinished, page, [=]() {
			const QByteArray requested = QByteArray::number(value->value()) + "\n";
			QFile actual(path);
			const bool ok = writeSubsystemFile(path, requested);
			if (actual.open(QIODevice::ReadOnly)) {
				value->setValue(actual.read(32).trimmed().toInt());
			}
			if (!ok) {
				QMessageBox::warning(page, "POSIX", QStringLiteral("Ядро отклонило размер буфера."));
			}
		});
	}
}

// Выбрать установленный профиль или передать открытый пользователем APK установщику
static void androidPackages(QWidget *parent) {
	auto *dialog = new QDialog(parent);
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setWindowTitle(QStringLiteral("Приложения Android"));
	dialog->resize(540, 330);
	auto *layout = new QVBoxLayout(dialog);
	auto *list = new QListWidget(dialog);
	layout->addWidget(list);
	auto *row = new QHBoxLayout();
	layout->addLayout(row);
	auto *open = new QPushButton(QStringLiteral("Открыть"), dialog);
	auto *install = new QPushButton(QStringLiteral("Установить или обновить…"), dialog);
	auto *clone = new QPushButton(QStringLiteral("Клонировать…"), dialog);
	row->addWidget(open);
	row->addWidget(install);
	row->addWidget(clone);
	auto *status = new QLabel(dialog);
	status->setWordWrap(true);
	status->setTextFormat(Qt::PlainText);
	layout->addWidget(status);
	auto refresh = [=]() {
		QFile registry("/synapse/subsys/android/packages/identities");
		if (registry.exists() && !registry.open(QIODevice::ReadOnly)) {
			status->setText(QStringLiteral("Не удалось прочитать список установок."));
			return;
		}
		list->clear();
		QHash<QByteArray, QListWidgetItem *> installed;
		while (registry.isOpen() && !registry.atEnd()) {
			const QList<QByteArray> fields = registry.readLine().simplified().split(' ');
			if (fields.size() != 5 && fields.size() != 7 && fields.size() != 8) {
				continue;
			}
			const QString profile = fields.size() >= 7 ? QString::fromUtf8(fields[5]) : QStringLiteral("main");
			const QString filename = QString::fromUtf8(fields[2]);
			if (filename != QFileInfo(filename).fileName() || !filename.endsWith(".apk")) {
				continue;
			}
			const QString title = QString::fromUtf8(fields[3]) + " · " +
			                      (profile == "main" ? QStringLiteral("Основная") : profile);
			auto *&item = installed[fields[3] + '\t' + profile.toUtf8()];
			if (!item) {
				item = new QListWidgetItem(title, list);
			}
			item->setData(Qt::UserRole, profile);
			item->setData(Qt::UserRole + 1, QStringLiteral("/synapse/subsys/android/packages/") + filename);
		}
		if (list->count()) {
			list->setCurrentRow(0);
		}
		open->setEnabled(list->currentItem() != nullptr);
	};
	QObject::connect(open, &QPushButton::clicked, dialog, [=]() {
		auto *item = list->currentItem();
		if (item && !QProcess::startDetached("/bin/android-run", {"--profile",
		        item->data(Qt::UserRole).toString(), item->data(Qt::UserRole + 1).toString()})) {
			status->setText(QStringLiteral("Не удалось открыть приложение."));
		}
	});
	auto choose = [=](bool separate) {
		const QString path = QFileDialog::getOpenFileName(dialog, QStringLiteral("Выбрать APK"), {}, "APK (*.apk)");
		if (path.isEmpty()) {
			return;
		}
		QStringList args;
		if (separate) {
			bool ok = false;
			const QString name = QInputDialog::getText(dialog, QStringLiteral("Клон Android"),
				QStringLiteral("Имя профиля (латиница, цифры, дефис, подчёркивание)"), QLineEdit::Normal, {}, &ok);
			if (!ok) {
				return;
			}
			if (name == "main" || !QRegularExpression("^[a-zA-Z0-9_-]{1,32}$").match(name).hasMatch()) {
				status->setText(QStringLiteral("Выберите имя клона длиной от 1 до 32 знаков."));
				return;
			}
			args << "--clone" << name;
		} else {
			args << "--install";
		}
		args << path;
		auto *job = new QProcess(dialog);
		job->setProcessChannelMode(QProcess::MergedChannels);
		install->setEnabled(false);
		clone->setEnabled(false);
		status->setText(QStringLiteral("Установка APK…"));
		QObject::connect(job, &QProcess::finished, dialog, [=](int exitCode, QProcess::ExitStatus result) {
			QString detail = QString::fromUtf8(job->readAllStandardOutput()).trimmed();
			status->setText(exitCode == 0 && result == QProcess::NormalExit ?
				QStringLiteral("Приложение установлено.") : detail.right(700));
			install->setEnabled(true);
			clone->setEnabled(true);
			refresh();
			job->deleteLater();
		});
		QObject::connect(job, &QProcess::errorOccurred, dialog, [=](QProcess::ProcessError error) {
			if (error == QProcess::FailedToStart) {
				status->setText(job->errorString());
				install->setEnabled(true);
				clone->setEnabled(true);
				job->deleteLater();
			}
		});
		job->start("/bin/android-run", args);
	};
	QObject::connect(install, &QPushButton::clicked, dialog, [=]() { choose(false); });
	QObject::connect(clone, &QPushButton::clicked, dialog, [=]() { choose(true); });
	refresh();
	dialog->show();
}

// Ориентация и размеры действующих окон и параметры новых процессов
static void androidControls(QWidget *page, QFormLayout *form, int version) {
	const bool dimensions = version >= 2;
	const QString path = QStringLiteral("/synapse/subsys/android/data/orientation-%1").arg(getuid());
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly) || flock(file.handle(), LOCK_SH) != 0) {
		form->addRow(new QLabel(QStringLiteral("Профиль Android для этого сеанса ещё не создан."), page));
		return;
	}
	const QList<QByteArray> parts = file.read(80).simplified().split(' ');
	file.close();
	const bool phones = version >= 4;
	const bool writable = QFileInfo(path).isWritable();
	struct Phone { const char *name; int height, width, dpi; };
	static const Phone models[] = {
		{"Google Pixel 5", 2340, 1080, 440},
		{"Google Pixel 6", 2400, 1080, 420},
		{"Google Pixel 6 Pro", 3120, 1440, 560}
	};
	const int storedLong = parts.value(1).toInt(), storedShort = parts.value(2).toInt();
	const int storedDensity = parts.value(4).toInt();
	auto *device = new QComboBox(page);
	int selection = 0;
	if (phones) {
		device->addItem(QStringLiteral("Компактное окно Desktop"));
		for (const auto &model : models) {
			device->addItem(QStringLiteral("%1 · %2×%3 · %4 dpi")
				.arg(QString::fromUtf8(model.name)).arg(model.width).arg(model.height).arg(model.dpi));
			if (parts.value(5) == "1" && model.height == storedLong && model.width == storedShort &&
			    model.dpi == storedDensity) {
				selection = device->count() - 1;
			}
		}
		device->addItem(QStringLiteral("Свой размер и плотность"));
		if (!selection && storedLong && storedShort) {
			selection = device->count() - 1;
		}
		device->setCurrentIndex(selection);
		device->setEnabled(writable);
		form->addRow(QStringLiteral("Экран устройства"), device);
	} else {
		device->hide();
	}
	auto *orientation = new QComboBox(page);
	orientation->addItems({QStringLiteral("По приложению и экрану"), QStringLiteral("Книжная"),
	                       QStringLiteral("Альбомная")});
	if (phones) {
		orientation->addItem(QStringLiteral("По приложению, с поворотом на 90°"));
	}
	orientation->setCurrentIndex(qBound(0, parts.value(0).toInt(), orientation->count() - 1));
	orientation->setEnabled(writable);
	form->addRow(QStringLiteral("Ориентация"), orientation);
	auto *rotate = new QPushButton(QStringLiteral("Повернуть на 90°"), page);
	if (phones) {
		rotate->setEnabled(writable);
		form->addRow(rotate);
	} else {
		rotate->hide();
	}
	auto *longer = new QSpinBox(page);
	auto *shorter = new QSpinBox(page);
	auto *density = new QSpinBox(page);
	auto *sizing = new QCheckBox(QStringLiteral("Задать размер окна"), page);
	sizing->setChecked(storedLong >= 420 && storedShort >= 240);
	if (dimensions) {
		longer->setRange(420, phones ? 4096 : 1920);
		shorter->setRange(240, phones ? 4096 : 1200);
		longer->setValue(sizing->isChecked() ? storedLong : 520);
		shorter->setValue(sizing->isChecked() ? storedShort : 300);
		if (!phones) {
			form->addRow(sizing);
			longer->setEnabled(sizing->isChecked());
			shorter->setEnabled(sizing->isChecked());
			QObject::connect(sizing, &QCheckBox::toggled, longer, &QWidget::setEnabled);
			QObject::connect(sizing, &QCheckBox::toggled, shorter, &QWidget::setEnabled);
		} else {
			sizing->hide();
		}
		form->addRow(QStringLiteral("Длинная сторона, пикс."), longer);
		form->addRow(QStringLiteral("Короткая сторона, пикс."), shorter);
	} else {
		longer->hide();
		shorter->hide();
		sizing->hide();
	}
	if (phones) {
		density->setRange(120, 640);
		density->setValue(storedDensity >= 120 && storedDensity <= 640 ? storedDensity : 160);
		form->addRow(QStringLiteral("Плотность Android, dpi"), density);
	} else {
		density->hide();
	}
	auto *objects = new QSpinBox(page);
	if (version >= 3) {
		objects->setRange(512, 65536);
		objects->setSingleStep(512);
		const int stored = parts.value(3).toInt();
		objects->setValue(stored >= 512 && stored <= 65536 ? stored : 4096);
		form->addRow(QStringLiteral("Лимит Java-объектов при запуске"), objects);
	} else {
		objects->hide();
	}
	auto *apply = new QPushButton(QStringLiteral("Применить"), page);
	apply->setEnabled(writable);
	form->addRow(apply);
	auto save = [=]() {
		const int index = device->currentIndex();
		const bool preset = phones && index > 0 && index < device->count() - 1;
		const bool custom = phones ? index == device->count() - 1 : sizing->isChecked();
		const int h = preset ? models[index - 1].height : custom ? longer->value() : 0;
		const int w = preset ? models[index - 1].width : custom ? shorter->value() : 0;
		const int dpi = preset ? models[index - 1].dpi : custom ? density->value() : 160;
		if (custom && w > h) {
			QMessageBox::warning(page, "Android", QStringLiteral("Короткая сторона превышает длинную."));
			return;
		}
		QByteArray data = QByteArray::number(orientation->currentIndex()) + "\n";
		if ((dimensions && (preset || custom)) || version >= 3) {
			data += QByteArray::number(h) + " " + QByteArray::number(w) + "\n";
		}
		if (version >= 3) {
			data += QByteArray::number(objects->value()) + "\n";
		}
		if (phones) {
			data += QByteArray::number(dpi) + (preset ? " 1\n" : " 0\n");
		}
		if (!writeSubsystemFile(path, data, true)) {
			QMessageBox::warning(page, "Android", QStringLiteral("Не удалось сохранить профиль окна."));
		}
	};
	QObject::connect(apply, &QPushButton::clicked, page, save);
	if (phones) {
		auto customFields = [=]() {
			const bool custom = device->currentIndex() == device->count() - 1;
			form->setRowVisible(longer, custom);
			form->setRowVisible(shorter, custom);
			form->setRowVisible(density, custom);
		};
		customFields();
		QObject::connect(device, &QComboBox::currentIndexChanged, page, [=]() {
			customFields();
			if (device->currentIndex() != device->count() - 1) {
				save();
			}
		});
		QObject::connect(orientation, &QComboBox::currentIndexChanged, page, save);
		QObject::connect(rotate, &QPushButton::clicked, page, [=]() {
			const QByteArray rotated("3210");
			QFile current(path);
			char previous;
			if (!current.open(QIODevice::ReadWrite | QIODevice::ExistingOnly | QIODevice::Unbuffered) ||
			    flock(current.handle(), LOCK_EX) != 0 || current.read(&previous, 1) != 1 ||
			    previous < '0' || previous > '3' || !current.seek(0) ||
			    current.write(rotated.constData() + (previous - '0'), 1) != 1 ||
			    !current.flush() || fsync(current.handle()) != 0) {
				QMessageBox::warning(page, "Android", QStringLiteral("Не удалось повернуть экран."));
				return;
			}
			const QSignalBlocker blocked(orientation);
			orientation->setCurrentIndex(rotated.at(previous - '0') - '0');
		});
	}
}

// Управлять состоянием по контракту подсистемы, независимо от текущего значения переключателя
static void subsystemToggle(QWidget *page, QFormLayout *form, const QString &name) {
	const QString path = name == "android" ? QStringLiteral("/synapse/subsys/android/enabled") :
	                     "/etc/synapse/subsystems/" + name + ".enabled";
	QFile file(path);
	QByteArray value;
	if (file.open(QIODevice::ReadOnly)) {
		value = file.read(8).trimmed();
	}
	auto *toggle = new QCheckBox(QStringLiteral("Включена"), page);
	toggle->setChecked(value == "1");
	toggle->setEnabled((value == "0" || value == "1") && QFileInfo(path).isWritable());
	form->addRow(toggle);
	if (value != "0" && value != "1") {
		form->addRow(new QLabel(QStringLiteral("Состояние подсистемы недоступно."), page));
	}
	QObject::connect(toggle, &QCheckBox::clicked, page, [=](bool enabled) {
		if (!writeSubsystemFile(path, enabled ? "1" : "0")) {
			toggle->setChecked(!enabled);
			QMessageBox::warning(page, name, QStringLiteral("Не удалось изменить состояние подсистемы."));
		}
	});
}

// Прочитать описания из пакетов и проверить присутствие каждой реализации перед показом
static void discoverSubsystems(QTabWidget *pages, QString selected = {}) {
	if (selected.isEmpty() && pages->currentWidget()) {
		selected = pages->currentWidget()->objectName();
	}
	while (pages->count()) {
		delete pages->widget(0);
	}
	auto add = [pages](const QString &id, const QString &title) {
		auto *page = new QWidget(pages);
		auto *form = new QFormLayout(page);
		auto *scroll = new QScrollArea(pages);
		scroll->setObjectName(id);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setWidget(page);
		pages->addTab(scroll, title);
		return form;
	};
	if (QFileInfo::exists(QStringLiteral("/proc/%1/status").arg(getpid()))) {
		auto *form = add("posix", "POSIX");
		form->addRow(new QLabel(QStringLiteral("Обязательная подсистема. Параметры сеанса наследуют запущенные в нём программы.")));
		posixControls(form->parentWidget(), form);
	}
	const QDir registry("/usr/share/synapse/subsystems");
	QStringList ids;
	for (const QFileInfo &info : registry.entryInfoList({"*.ini"}, QDir::Files | QDir::NoSymLinks, QDir::Name)) {
		ids.append(info.completeBaseName());
	}
	if (!ids.contains("android")) {
		ids.append("android");
	}
	for (const QString &id : ids) {
		if (id == "posix" || !QRegularExpression("^[a-z0-9_-]+$").match(id).hasMatch()) {
			continue;
		}
		QSettings spec(registry.filePath(id + ".ini"), QSettings::IniFormat);
		spec.beginGroup("Subsystem");
		const bool android = id == "android";
		const QString executable = spec.value("Executable", android ? "/bin/android-run" : "").toString();
		const QString required = spec.value("RequiredFile", android ? "/usr/share/android-rt/activity.template" : "").toString();
		if (!QDir::isAbsolutePath(executable) || !QFileInfo(executable).isFile() || !QFileInfo(executable).isExecutable() ||
		    (!required.isEmpty() && (!QDir::isAbsolutePath(required) || !QFileInfo(required).isFile()))) {
			continue;
		}
		auto *form = add(id, spec.value("Name", android ? QStringLiteral("Android") : id).toString());
		auto *page = form->parentWidget();
		subsystemToggle(page, form, id);
		if (android) {
			androidControls(page, form, spec.value("DisplayProfile").toInt());
			if (spec.value("InstallProfiles").toInt() >= 1) {
				auto *packages = new QPushButton(QStringLiteral("Приложения Android…"), page);
				form->addRow(packages);
				QObject::connect(packages, &QPushButton::clicked, page, [=]() { androidPackages(page); });
			}
		}
		const QString editor = spec.value("SettingsExecutable").toString();
		if (QDir::isAbsolutePath(editor) && QFileInfo(editor).isFile() && QFileInfo(editor).isExecutable()) {
			auto *open = new QPushButton(spec.value("SettingsLabel", QStringLiteral("Открыть настройки подсистемы")).toString(), page);
			form->addRow(open);
			QObject::connect(open, &QPushButton::clicked, page, [=]() {
				if (!QProcess::startDetached(editor, {})) {
					QMessageBox::warning(page, id, QStringLiteral("Не удалось открыть настройки."));
				}
			});
		}
	}
	for (int i = 0; i < pages->count(); i++) {
		if (pages->widget(i)->objectName() == selected) {
			pages->setCurrentIndex(i);
			break;
		}
	}
}
