// subsystems.h: Настройки установленных подсистем SynapseOS Desktop
// Автор: Елчинян Арен Артурович, 2026. AMD64, ABI Qt6 и POSIX
// Подсистема: графическая оболочка. Первое изменение: 21.11.2026. Последнее изменение: 23.11.2026. Общедоступно
#pragma once
#include "../labwc/posix-profile.h"
#include <QRegularExpression>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QInputDialog>
#include <QScrollArea>
#include <QSpinBox>
#include <QSignalBlocker>
#include <climits>
#include <sys/file.h>

// Записывать только существующий файл, настройки Android разделяются с работающими окнами
static bool writeSubsystemFile(const QString &path, const QByteArray &bytes, bool profile = false) {
	if (geteuid() != 0 && path.startsWith(QStringLiteral("/proc/"))) {
		QString output;
		return runPrivileged(nullptr, QStringLiteral("/bin/tee"), {path}, bytes, output);
	}
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

	auto *memory = new QComboBox(page);
	memory->addItem(QStringLiteral("Без отдельного ограничения"), 0);
	memory->addItem(QStringLiteral("Экономный режим, 512 МиБ"), 512);
	memory->addItem(QStringLiteral("Стандартный режим, 2 ГиБ"), 2048);
	memory->addItem(QStringLiteral("Большие задачи, 4 ГиБ"), 4096);
	const int memoryIndex = memory->findData(static_cast<int>(profile.memory_mib));
	memory->setCurrentIndex(memoryIndex >= 0 ? memoryIndex : 0);
	memory->setToolTip(QStringLiteral("Мягкий RLIMIT_AS для каждого процесса нового графического сеанса"));
	form->addRow(QStringLiteral("Память на процесс"), memory);

	auto *nice = new QSpinBox(page);
	nice->setRange(-20, 19);
	nice->setValue(profile.nice);
	form->addRow(QStringLiteral("Приоритет процессов (nice)"), nice);

	auto *stack = new QSpinBox(page);
	stack->setRange(0, 4096);
	stack->setSpecialValueText(QStringLiteral("Без отдельного ограничения"));
	stack->setSuffix(QStringLiteral(" МиБ"));
	stack->setValue(static_cast<int>(profile.stack_mib));
	form->addRow(QStringLiteral("Размер стека на процесс"), stack);

	auto *apply = new QPushButton(QStringLiteral("Сохранить для следующего входа"), page);
	form->addRow(apply);
	auto *status = new QLabel(page);
	status->setWordWrap(true);
	form->addRow(status);
	QObject::connect(apply, &QPushButton::clicked, page, [=]() {
		const QString text = mask->text().trimmed();
		bool valid = false;
		const int mode = text.isEmpty() ? -1 : text.toInt(&valid, 8);
		if ((!text.isEmpty() && (!valid || text.contains(QRegularExpression("[^0-7]")))) ||
		    (files->value() && files->value() < 128)) {
			QMessageBox::warning(page, "POSIX", QStringLiteral("Укажите маску от 000 до 777 и лимит от 128 или выберите наследование"));
			return;
		}
		char path[128];
		posix_profile_path(path);
		QSaveFile file(QString::fromLatin1(path));
		const QByteArray data = "mask=" + QByteArray::number(mode) + "\nfiles=" +
		                        QByteArray::number(files->value()) + "\nmemory=" +
		                        QByteArray::number(memory->currentData().toInt()) + "\npopulate=" +
		                        QByteArray::number(profile.populate) + "\nnice=" +
		                        QByteArray::number(nice->value()) + "\nstack=" +
		                        QByteArray::number(stack->value()) + "\n";
		if (!file.open(QIODevice::WriteOnly) ||
		    !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
		    file.write(data) != data.size() || !file.commit()) {
			status->setText(QStringLiteral("Не удалось сохранить профиль сеанса"));
			return;
		}
		status->setText(QStringLiteral("Параметры будут применены при следующем входе"));
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
		value->setEnabled(QFileInfo::exists(path));
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

// Записать личный файл Android с атомарной заменой и владельцем 0600
static bool writeAndroidPrivateFile(const QString &path, const QByteArray &data) {
	const QString parent = QFileInfo(path).absolutePath();
	if (!QDir().mkpath(parent)) {
		return false;
	}
	chmod(QFile::encodeName(parent).constData(), 0700);
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly) ||
	    !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
		return false;
	}
	return file.write(data) == data.size() && file.commit();
}

// Выбрать уровень совместимости, который читает установщик Android
static void androidApiControls(QWidget *page, QFormLayout *form) {
	const QString path = QStringLiteral("/synapse/subsys/android/api-profile");
	auto *profile = new QComboBox(page);
	profile->addItem(QStringLiteral("Android 8.0 · API 26"), 26);
	profile->addItem(QStringLiteral("Совместимость · API 24"), 24);
	QFile current(path);
	const QByteArray value = current.open(QIODevice::ReadOnly) ? current.read(16).trimmed() : QByteArray("26");
	profile->setCurrentIndex(value == "24" ? 1 : 0);
	profile->setEnabled(QFileInfo(path).exists() && QFileInfo(path).isWritable());
	form->addRow(QStringLiteral("Платформа приложений"), profile);
	QObject::connect(profile, &QComboBox::currentIndexChanged, page, [=]() {
		if (!writeSubsystemFile(path, profile->currentData().toString().toUtf8() + "\n", true)) {
			QMessageBox::warning(page, "Android", QStringLiteral("Не удалось сохранить профиль API."));
		}
	});
}

// Настроить общий журнал и число завершённых сеансов Android
static void androidLogControls(QWidget *page, QFormLayout *form) {
	const QString directory = QStringLiteral("/synapse/subsys/android/users/%1/log-settings").arg(getuid());
	auto readNumber = [=](const QString &name, int fallback) {
		QFile file(directory + "/" + name);
		return file.open(QIODevice::ReadOnly) ? file.read(16).trimmed().toInt() : fallback;
	};
	auto *mode = new QComboBox(page);
	mode->addItem(QStringLiteral("Выключен"), 0);
	mode->addItem(QStringLiteral("Полный"), 1);
	mode->setCurrentIndex(readNumber(QStringLiteral("global"), 0) == 1 ? 1 : 0);
	form->addRow(QStringLiteral("Общий журнал"), mode);
	auto *keep = new QSpinBox(page);
	keep->setRange(1, 20);
	keep->setValue(readNumber(QStringLiteral("keep"), 5));
	form->addRow(QStringLiteral("Завершённых сеансов на профиль"), keep);
	auto *apply = new QPushButton(QStringLiteral("Сохранить параметры журнала"), page);
	form->addRow(apply);
	auto *status = new QLabel(page);
	status->setWordWrap(true);
	form->addRow(status);
	QObject::connect(apply, &QPushButton::clicked, page, [=]() {
		QFile lock(directory + "/.lock");
		const bool locked = lock.open(QIODevice::ReadWrite) && flock(lock.handle(), LOCK_EX) == 0;
		const bool saved = locked &&
			writeAndroidPrivateFile(directory + "/global", mode->currentData().toString().toUtf8() + "\n") &&
			writeAndroidPrivateFile(directory + "/keep", QByteArray::number(keep->value()) + "\n");
		if (locked) {
			flock(lock.handle(), LOCK_UN);
		}
		status->setText(saved ? QStringLiteral("Настройки журнала сохранены") :
		               QStringLiteral("Не удалось сохранить настройки журнала"));
	});
	auto *open = new QPushButton(QStringLiteral("Открыть журналы приложений и клонов"), page);
	form->addRow(open);
	QObject::connect(open, &QPushButton::clicked, page, [=]() {
		QProcess::startDetached(QStringLiteral("/bin/synapse-files"),
		    {QStringLiteral("/synapse/subsys/android/users/%1/logs").arg(getuid())});
	});
	auto *folder = new QPushButton(QStringLiteral("Открыть каталог журналов"), page);
	form->addRow(folder);
	QObject::connect(folder, &QPushButton::clicked, page, [=]() {
		QProcess::startDetached(QStringLiteral("/bin/synapse-files"),
		                        {QStringLiteral("/synapse/subsys/android/users/%1/logs").arg(getuid())});
	});
}

// Настроить журнал отдельного приложения или клона без отдельного окна Android
static void androidPackageLogControls(QWidget *page, QFormLayout *form) {
	auto *installed = new QComboBox(page);
	form->addRow(QStringLiteral("Приложение или клон"), installed);
	auto *packageMode = new QComboBox(page);
	auto *cloneMode = new QComboBox(page);
	for (auto *mode : {packageMode, cloneMode}) {
		mode->addItem(QStringLiteral("Наследовать"), -1);
		mode->addItem(QStringLiteral("Выключен"), 0);
		mode->addItem(QStringLiteral("Полный"), 1);
	}
	form->addRow(QStringLiteral("Журнал всех профилей"), packageMode);
	form->addRow(QStringLiteral("Журнал выбранного клона"), cloneMode);
	QProcess listing;
	listing.start(QStringLiteral("/bin/android-run"), {QStringLiteral("--list")});
	if (listing.waitForFinished(5000) && listing.exitCode() == 0) {
		const QList<QByteArray> rows = listing.readAllStandardOutput().split('\n');
		for (const QByteArray &row : rows) {
			const QList<QByteArray> fields = row.split('\t');
			if (fields.size() < 4) {
				continue;
			}
			const QString package = QString::fromUtf8(fields[0]);
			const QString profile = QString::fromUtf8(fields[1]);
			installed->addItem(package + QStringLiteral(" · ") + profile);
			const int index = installed->count() - 1;
			installed->setItemData(index, package, Qt::UserRole);
			installed->setItemData(index, profile, Qt::UserRole + 1);
		}
	}
	installed->setEnabled(installed->count() > 0);
	packageMode->setEnabled(installed->count() > 0);
	cloneMode->setEnabled(installed->count() > 0);
	const QString root = QStringLiteral("/synapse/subsys/android/users/%1/log-settings").arg(getuid());
	auto readMode = [=](const QString &path) {
		QFile file(path);
		return file.open(QIODevice::ReadOnly) ? file.read(4).trimmed().toInt() : -1;
	};
	auto refresh = [=]() {
		const QString package = installed->currentData(Qt::UserRole).toString();
		const QString profile = installed->currentData(Qt::UserRole + 1).toString();
		const QString directory = root + QStringLiteral("/packages/") + package;
		packageMode->setCurrentIndex(qMax(0, packageMode->findData(readMode(directory + QStringLiteral("/mode")))));
		cloneMode->setCurrentIndex(qMax(0, cloneMode->findData(readMode(directory + QStringLiteral("/clone.") + profile))));
		cloneMode->setEnabled(installed->count() > 0 && profile != QStringLiteral("main"));
	};
	QObject::connect(installed, &QComboBox::currentIndexChanged, page, refresh);
	refresh();
	auto *apply = new QPushButton(QStringLiteral("Сохранить журнал приложения"), page);
	form->addRow(apply);
	auto *status = new QLabel(page);
	status->setWordWrap(true);
	form->addRow(status);
	QObject::connect(apply, &QPushButton::clicked, page, [=]() {
		const QString package = installed->currentData(Qt::UserRole).toString();
		const QString profile = installed->currentData(Qt::UserRole + 1).toString();
		if (package.isEmpty() || profile.isEmpty()) {
			return;
		}
		const QString packages = root + QStringLiteral("/packages");
		const QString directory = packages + QStringLiteral("/") + package;
		QFile lock(root + QStringLiteral("/.lock"));
		const bool locked = lock.open(QIODevice::ReadWrite) && flock(lock.handle(), LOCK_EX) == 0;
		bool saved = locked && QDir().mkpath(directory);
		if (saved) {
			chmod(QFile::encodeName(packages).constData(), 0700);
			chmod(QFile::encodeName(directory).constData(), 0700);
		}
		auto store = [=](const QString &name, int value) {
			const QString path = directory + QStringLiteral("/") + name;
			return value < 0 ? (QFile::remove(path) || !QFileInfo::exists(path)) :
			    writeAndroidPrivateFile(path, QByteArray::number(value) + "\n");
		};
		saved = saved && store(QStringLiteral("mode"), packageMode->currentData().toInt()) &&
		    (profile == QStringLiteral("main") || store(QStringLiteral("clone.") + profile, cloneMode->currentData().toInt()));
		if (locked) {
			flock(lock.handle(), LOCK_UN);
		}
		status->setText(saved ? QStringLiteral("Настройки журнала приложения сохранены") :
		    QStringLiteral("Не удалось сохранить настройки журнала приложения"));
	});
}

// Настроить ручной источник координат Android для текущего пользователя
static void androidGpsControls(QWidget *page, QFormLayout *form) {
	const QString path = QStringLiteral("/synapse/subsys/android/users/%1/location/manual").arg(getuid());
	bool enabled = false;
	double savedLatitude = 55.755833, savedLongitude = 37.617278;
	QFile current(path);
	if (current.open(QIODevice::ReadOnly)) {
		const QList<QByteArray> parts = current.readAll().simplified().split(' ');
		if (parts.size() >= 6 && parts[0] == "GPS1") {
			enabled = parts[1] == "1";
			savedLatitude = parts[4].toDouble();
			savedLongitude = parts[5].toDouble();
		}
	}
	auto *manual = new QCheckBox(QStringLiteral("Использовать ручной GPS"), page);
	manual->setChecked(enabled);
	form->addRow(manual);
	auto *latitude = new QDoubleSpinBox(page);
	latitude->setRange(-90.0, 90.0);
	latitude->setDecimals(6);
	latitude->setValue(savedLatitude);
	form->addRow(QStringLiteral("Широта"), latitude);
	auto *longitude = new QDoubleSpinBox(page);
	longitude->setRange(-180.0, 180.0);
	longitude->setDecimals(6);
	longitude->setValue(savedLongitude);
	form->addRow(QStringLiteral("Долгота"), longitude);
	auto *apply = new QPushButton(QStringLiteral("Применить местоположение"), page);
	form->addRow(apply);
	auto *status = new QLabel(page);
	status->setWordWrap(true);
	form->addRow(status);
	QObject::connect(apply, &QPushButton::clicked, page, [=]() {
		timespec wall = {}, monotonic = {};
		if (clock_gettime(CLOCK_REALTIME, &wall) != 0 || clock_gettime(CLOCK_MONOTONIC, &monotonic) != 0) {
			status->setText(QStringLiteral("Не удалось прочитать системные часы"));
			return;
		}
		QFile lock(QFileInfo(path).absolutePath() + "/.lock");
		const bool locked = lock.open(QIODevice::ReadWrite) && flock(lock.handle(), LOCK_EX) == 0;
		const long long milliseconds = static_cast<long long>(wall.tv_sec) * 1000 + wall.tv_nsec / 1000000;
		const long long nanoseconds = static_cast<long long>(monotonic.tv_sec) * 1000000000 + monotonic.tv_nsec;
		const QByteArray data = "GPS1 " + QByteArray::number(manual->isChecked() ? 1 : 0) + " " +
			QByteArray::number(milliseconds) + " " + QByteArray::number(nanoseconds) + " " +
			QByteArray::number(latitude->value(), 'g', 17) + " " + QByteArray::number(longitude->value(), 'g', 17) + "\n";
		const bool saved = locked && writeAndroidPrivateFile(path, data);
		if (locked) {
			flock(lock.handle(), LOCK_UN);
		}
		status->setText(saved ? (manual->isChecked() ? QStringLiteral("Ручные координаты применены") :
			QStringLiteral("Ручной GPS выключен")) : QStringLiteral("Не удалось сохранить местоположение"));
	});
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
		objects->setRange(version >= 5 ? 0 : 512, 65536);
		if (version >= 5) {
			objects->setSpecialValueText(QStringLiteral("Автоматически (32 768)"));
			QObject::connect(objects, &QSpinBox::editingFinished, objects, [objects]() {
				if (objects->value() > 0 && objects->value() < 512) {
					objects->setValue(512);
				}
			});
		}
		objects->setSingleStep(512);
		const int stored = parts.value(3).toInt();
		objects->setValue(stored >= 512 && stored <= 65536 ? stored : (version >= 5 ? 0 : 4096));
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

// Добавить самостоятельные страницы подсистем в общий список настроек
static void discoverSubsystems(QTabWidget *pages, const QString &selected = {}) {
	auto add = [pages](const QString &id, const QString &title) {
		auto *page = new QWidget(pages);
		auto *form = new QFormLayout(page);
		form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		auto *scroll = new QScrollArea(pages);
		scroll->setObjectName(id);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setWidget(page);
		scroll->setProperty("settingsBuilt", true);
		pages->addTab(scroll, title);
		return form;
	};

	auto *posix = add(QStringLiteral("posix"), QStringLiteral("POSIX"));
	auto *posixNote = new QLabel(QStringLiteral(
	    "Обязательная подсистема Linux ABI. Эти параметры наследуют процессы нового графического сеанса"), posix->parentWidget());
	posixNote->setWordWrap(true);
	posix->addRow(posixNote);
	posixControls(posix->parentWidget(), posix);

	const QDir registry(QStringLiteral("/usr/share/synapse/subsystems"));
	QSettings spec(registry.filePath(QStringLiteral("android.ini")), QSettings::IniFormat);
	spec.beginGroup(QStringLiteral("Subsystem"));
	auto *android = add(QStringLiteral("android"), QStringLiteral("Android"));
	auto *androidPage = android->parentWidget();
	const bool androidInstalled = QFileInfo(QStringLiteral("/bin/android-run")).isExecutable() &&
	    QFileInfo(QStringLiteral("/usr/share/android-rt/activity.template")).isFile();
	auto *androidState = new QLabel(androidInstalled ?
	    QStringLiteral("Android Runtime установлен. Изменения окон применяются к новым процессам") :
	    QStringLiteral("Android Runtime не установлен. Установите пакет android-rt через магазин программ"), androidPage);
	androidState->setWordWrap(true);
	android->addRow(androidState);
	subsystemToggle(androidPage, android, QStringLiteral("android"));
	androidApiControls(androidPage, android);
	androidControls(androidPage, android, spec.value(QStringLiteral("DisplayProfile"), 5).toInt());
	androidGpsControls(androidPage, android);
	androidLogControls(androidPage, android);
	androidPackageLogControls(androidPage, android);
	auto *packages = new QPushButton(QStringLiteral("Приложения, установка APK и клоны…"), androidPage);
	packages->setEnabled(androidInstalled);
	android->addRow(packages);
	QObject::connect(packages, &QPushButton::clicked, androidPage, [=]() { androidPackages(androidPage); });

	const QString access = QStringLiteral(
	    "Доступ к подсистеме закрыт, обратитесь к aren@synapseos.ru и/или t.me/nera00 "
	    "для доступа к альфа-версии подсистемы");
	for (const auto &locked : {QPair<QString, QString>{QStringLiteral("winapi"), QStringLiteral("WinAPI")},
	                          QPair<QString, QString>{QStringLiteral("dos"), QStringLiteral("DOS")}}) {
		auto *form = add(locked.first, locked.second);
		auto *label = new QLabel(access, form->parentWidget());
		label->setWordWrap(true);
		label->setTextInteractionFlags(Qt::TextSelectableByMouse);
		form->addRow(label);
		auto *disabled = new QCheckBox(QStringLiteral("Включить подсистему"), form->parentWidget());
		disabled->setEnabled(false);
		form->addRow(disabled);
		auto choice = [=](const QString &name, const QStringList &values) {
			auto *value = new QComboBox(form->parentWidget());
			value->addItems(values);
			value->setEnabled(false);
			form->addRow(name, value);
		};
		auto text = [=](const QString &name, const QString &value) {
			auto *field = new QLineEdit(value, form->parentWidget());
			field->setEnabled(false);
			form->addRow(name, field);
		};
		auto toggle = [=](const QString &name) {
			auto *value = new QCheckBox(name, form->parentWidget());
			value->setEnabled(false);
			form->addRow(value);
		};
		if (locked.first == QStringLiteral("winapi")) {
			choice(QStringLiteral("Совместимость"), {QStringLiteral("Windows 10 22H2")});
			choice(QStringLiteral("Архитектура приложений"), {QStringLiteral("x86_64 и x86"), QStringLiteral("Только x86_64"), QStringLiteral("Только x86")});
			text(QStringLiteral("Каталог системы"), QStringLiteral("/synapse/subsys/winapi/system"));
			text(QStringLiteral("Диск C:"), QStringLiteral("/synapse/subsys/winapi/c"));
			choice(QStringLiteral("Поиск DLL"), {QStringLiteral("Сначала системные"), QStringLiteral("Сначала из каталога программы")});
			choice(QStringLiteral("Direct3D 11"), {QStringLiteral("Аппаратный"), QStringLiteral("Программный")});
			choice(QStringLiteral("Аудио"), {QStringLiteral("Системный выход SynapseOS"), QStringLiteral("Выключен")});
			choice(QStringLiteral("Исключения SEH"), {QStringLiteral("Полная обработка"), QStringLiteral("Только совместимость")});
			choice(QStringLiteral("Планировщик потоков"), {QStringLiteral("Windows 10"), QStringLiteral("Совместимый")});
			toggle(QStringLiteral("Поддержка Unicode UTF-16"));
			toggle(QStringLiteral("Изолировать программы в отдельных каталогах"));
			toggle(QStringLiteral("Журнал вызовов WinAPI"));
			toggle(QStringLiteral("Показывать диагностику запуска"));
		} else {
			choice(QStringLiteral("Машина"), {QStringLiteral("PC/AT"), QStringLiteral("80486"), QStringLiteral("Pentium"), QStringLiteral("Pentium II")});
			choice(QStringLiteral("Процессор"), {QStringLiteral("Автоматически"), QStringLiteral("80486DX"), QStringLiteral("Pentium MMX")});
			choice(QStringLiteral("Частота эмуляции"), {QStringLiteral("Автоматически"), QStringLiteral("Максимальная"), QStringLiteral("Фиксированная")});
			choice(QStringLiteral("Оперативная память"), {QStringLiteral("16 МиБ"), QStringLiteral("32 МиБ"), QStringLiteral("64 МиБ"), QStringLiteral("128 МиБ"), QStringLiteral("256 МиБ")});
			choice(QStringLiteral("Видеокарта"), {QStringLiteral("VGA"), QStringLiteral("SVGA"), QStringLiteral("VESA 2.0")});
			choice(QStringLiteral("Режим видео"), {QStringLiteral("Текстовый"), QStringLiteral("13h"), QStringLiteral("VESA"), QStringLiteral("По программе")});
			choice(QStringLiteral("Звуковая карта"), {QStringLiteral("Sound Blaster 16"), QStringLiteral("AdLib"), QStringLiteral("Gravis Ultrasound"), QStringLiteral("PC Speaker")});
			choice(QStringLiteral("Мышь"), {QStringLiteral("PS/2"), QStringLiteral("COM1"), QStringLiteral("Нет")});
			choice(QStringLiteral("Раскладка клавиатуры"), {QStringLiteral("Русская"), QStringLiteral("Английская США")});
			text(QStringLiteral("Диск C:"), QStringLiteral("/synapse/subsys/dos/c"));
			text(QStringLiteral("Каталог общих файлов"), QDir::homePath() + QStringLiteral("/Documents/DOS"));
			toggle(QStringLiteral("Эмуляция сопроцессора FPU"));
			toggle(QStringLiteral("Эмуляция EMS"));
			toggle(QStringLiteral("Эмуляция XMS"));
			choice(QStringLiteral("Сетевой адаптер"), {QStringLiteral("Нет"), QStringLiteral("NE2000"), QStringLiteral("PCnet")});
			choice(QStringLiteral("Пакетный драйвер"), {QStringLiteral("Выключен"), QStringLiteral("NDIS2"), QStringLiteral("Packet interface")});
			toggle(QStringLiteral("Полноэкранный режим"));
			toggle(QStringLiteral("Сохранять состояние при выходе"));
		}
	}

	for (const QFileInfo &info : registry.entryInfoList({QStringLiteral("*.ini")}, QDir::Files | QDir::NoSymLinks, QDir::Name)) {
		const QString id = info.completeBaseName();
		if (id == QStringLiteral("android") || id == QStringLiteral("posix") ||
		    id == QStringLiteral("winapi") || id == QStringLiteral("dos") ||
		    !QRegularExpression(QStringLiteral("^[a-z0-9_-]+$")).match(id).hasMatch()) {
			continue;
		}
		QSettings extra(info.absoluteFilePath(), QSettings::IniFormat);
		extra.beginGroup(QStringLiteral("Subsystem"));
		const QString editor = extra.value(QStringLiteral("SettingsExecutable")).toString();
		if (!QDir::isAbsolutePath(editor) || !QFileInfo(editor).isExecutable()) {
			continue;
		}
		auto *form = add(id, extra.value(QStringLiteral("Name"), id).toString());
		auto *open = new QPushButton(QStringLiteral("Открыть настройки подсистемы"), form->parentWidget());
		form->addRow(open);
		QObject::connect(open, &QPushButton::clicked, form->parentWidget(), [=]() {
			QProcess::startDetached(editor, {});
		});
	}

	if (!selected.isEmpty()) {
		for (int index = 0; index < pages->count(); ++index) {
			if (pages->widget(index)->objectName() == selected) {
				pages->setCurrentIndex(index);
				break;
			}
		}
	}
}
