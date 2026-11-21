/*
    main.cpp: Графический файловый менеджер SynapseOS
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64 и AArch64, ABI: Qt 6 и musl

    Описание: Просмотр каталогов и основные операции с файлами через Qt Widgets
    Подсистема: графические программы
    Первое изменение: 25.10.2026
    Последнее изменение: 21.11.2026
    Доступ: общедоступный
*/

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPixmap>
#include <QPalette>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSplitter>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdio>

// Согласовать оформление файлового менеджера с темой рабочего стола
static void applyDesktopTheme() {
	QString path = QDir::homePath() + QStringLiteral("/.config/labwc/rc.xml");
	if (!QFileInfo::exists(path)) {
		path = QStringLiteral("/etc/xdg/labwc/rc.xml");
	}

	QFile file(path);
	const bool loaded = file.open(QIODevice::ReadOnly | QIODevice::Text);
	const QString config = loaded ? QString::fromUtf8(file.readAll()) : QString();
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
	                    penumbra ? QStringLiteral("#769fb4") :
	                    QStringLiteral("#4388c5"));

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

// Скопировать файл или каталог, не переходя по символическим ссылкам
static bool copyPath(const QString &source, const QString &destination) {
	QFileInfo info(source);

	if (info.isSymLink()) {
		return false;
	}

	if (!info.isDir()) {
		return QFile::copy(source, destination);
	}

	if (!QDir().mkpath(destination)) {
		return false;
	}

	QDir directory(source);
	const QFileInfoList entries = directory.entryInfoList(
	    QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);

	for (const QFileInfo &entry : entries) {
		if (!copyPath(entry.absoluteFilePath(), QDir(destination).filePath(entry.fileName()))) {
			return false;
		}
	}

	return true;
}

// Выбрать свободное имя, не заменяя существующие файлы
static QString availablePath(const QString &path) {
	if (!QFileInfo::exists(path)) {
		return path;
	}

	for (int number = 1; number < 10000; number++) {
		QString candidate = path + QStringLiteral(" (copy %1)").arg(number);

		if (!QFileInfo::exists(candidate)) {
			return candidate;
		}
	}

	return QString();
}

// Проверить вложенное копирование и выбор имени без перезаписи
static bool testFileOperations() {
	QTemporaryDir temporary;

	if (!temporary.isValid()) {
		return false;
	}

	QString source = temporary.filePath(QStringLiteral("source"));
	QString nested = QDir(source).filePath(QStringLiteral("nested"));

	if (!QDir().mkpath(nested)) {
		return false;
	}

	QFile input(QDir(nested).filePath(QStringLiteral("note.txt")));

	if (!input.open(QIODevice::WriteOnly) || input.write("SynapseOS files\n") != 16) {
		return false;
	}

	input.close();
	QString destination = temporary.filePath(QStringLiteral("copy"));

	if (!copyPath(source, destination)) {
		return false;
	}

	QFile output(QDir(destination).filePath(QStringLiteral("nested/note.txt")));

	if (!output.open(QIODevice::ReadOnly) || output.readAll() != QByteArray("SynapseOS files\n")) {
		return false;
	}

	return availablePath(destination) == destination + QStringLiteral(" (copy 1)");
}

class FileListModel final : public QFileSystemModel {
public:
	using QFileSystemModel::QFileSystemModel;

	// Заголовок имени файла содержит только текст, значки остаются у самих файлов
	QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
		if (orientation == Qt::Horizontal && role == Qt::DecorationRole) {
			return {};
		}
		return QFileSystemModel::headerData(section, orientation, role);
	}
};

class FilesWindow final : public QMainWindow {
public:
	// Собрать окно и открыть начальный каталог
	explicit FilesWindow(const QString &initial) {
		setWindowTitle(QStringLiteral("Files - SynapseOS"));
		resize(900, 600);

		model = new FileListModel(this);
		model->setReadOnly(false);

		view = new QTreeView(this);
		view->setModel(model);
		view->setRootIsDecorated(false);
		view->setItemsExpandable(false);
		view->setSelectionBehavior(QAbstractItemView::SelectRows);
		view->setSortingEnabled(true);
		view->setUniformRowHeights(true);
		view->setAlternatingRowColors(true);
		view->sortByColumn(0, Qt::AscendingOrder);

		places = new QListWidget(this);
		places->setMaximumWidth(170);
		places->addItem(QStringLiteral("Home"));
		places->addItem(QStringLiteral("Root"));
		places->addItem(QStringLiteral("Downloads"));
		places->addItem(QStringLiteral("Pictures"));

		QSplitter *splitter = new QSplitter(this);
		splitter->addWidget(places);
		splitter->addWidget(view);
		splitter->setStretchFactor(1, 1);
		setCentralWidget(splitter);

		QToolBar *toolbar = addToolBar(QStringLiteral("Navigation"));
		toolbar->setMovable(false);
		addButton(toolbar, QStringLiteral("←"), [this]() { back(); });
		addButton(toolbar, QStringLiteral("→"), [this]() { forward(); });
		addButton(toolbar, QStringLiteral("↑"), [this]() { up(); });
		addButton(toolbar, QStringLiteral("Home"), [this]() { navigate(QDir::homePath()); });

		location = new QLineEdit(this);
		location->setMinimumWidth(320);
		toolbar->addWidget(location);
		addButton(toolbar, QStringLiteral("Go"), [this]() { navigate(location->text()); });
		toolbar->addSeparator();
		addButton(toolbar, QStringLiteral("New folder"), [this]() { newFolder(); });
		addButton(toolbar, QStringLiteral("Rename"), [this]() { renameSelected(); });
		addButton(toolbar, QStringLiteral("Delete"), [this]() { deleteSelected(); });
		addButton(toolbar, QStringLiteral("Copy"), [this]() { rememberSelected(false); });
		addButton(toolbar, QStringLiteral("Cut"), [this]() { rememberSelected(true); });
		addButton(toolbar, QStringLiteral("Paste"), [this]() { paste(); });

		connect(location, &QLineEdit::returnPressed, this, [this]() { navigate(location->text()); });
		connect(view, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
			open(index);
		});
		connect(places, &QListWidget::currentRowChanged, this, [this](int row) {
			const QString home = QDir::homePath();
			const QStringList paths = {home, QStringLiteral("/"), home + QStringLiteral("/Downloads"),
			                           home + QStringLiteral("/Pictures")};

			if (row >= 0 && row < paths.size()) {
				navigate(paths.at(row));
			}
		});

		addShortcut(QKeySequence(QStringLiteral("Alt+Left")), [this]() { back(); });
		addShortcut(QKeySequence(QStringLiteral("Alt+Right")), [this]() { forward(); });
		addShortcut(QKeySequence(QStringLiteral("Alt+Up")), [this]() { up(); });
		addShortcut(QKeySequence(QStringLiteral("Ctrl+L")), [this]() { location->setFocus(); });
		addShortcut(QKeySequence(QStringLiteral("Ctrl+C")), [this]() { rememberSelected(false); });
		addShortcut(QKeySequence(QStringLiteral("Ctrl+X")), [this]() { rememberSelected(true); });
		addShortcut(QKeySequence(QStringLiteral("Ctrl+V")), [this]() { paste(); });
		addShortcut(QKeySequence(QStringLiteral("F2")), [this]() { renameSelected(); });
		addShortcut(QKeySequence(QStringLiteral("Delete")), [this]() { deleteSelected(); });

		QFileInfo initialEntry(initial);
		if (initialEntry.isFile()) {
			navigate(initialEntry.absolutePath());
			QModelIndex selected = model->index(initialEntry.absoluteFilePath());
			view->setCurrentIndex(selected);
			QTimer::singleShot(0, this, [this, selected]() { open(selected); });
		} else {
			navigate(initial);
		}
	}

	// Проверить, что модель открыла каталог и окно обработало события
	bool ready() const {
		return view->rootIndex().isValid() && QFileInfo(current).isDir();
	}

private:
	QFileSystemModel *model = nullptr;
	QTreeView *view = nullptr;
	QListWidget *places = nullptr;
	QLineEdit *location = nullptr;
	QString current;
	QString clipboardPath;
	QStringList history;
	int historyIndex = -1;
	bool clipboardCut = false;

	// Добавить кнопку на панель без отдельного файла интерфейса
	template <typename Handler>
	void addButton(QToolBar *toolbar, const QString &title, Handler handler) {
		QAction *action = toolbar->addAction(title);
		connect(action, &QAction::triggered, this, handler);
	}

	// Привязать горячую клавишу к действию окна
	template <typename Handler>
	void addShortcut(const QKeySequence &sequence, Handler handler) {
		QShortcut *shortcut = new QShortcut(sequence, this);
		connect(shortcut, &QShortcut::activated, this, handler);
	}

	// Показать каталог и сохранить переход в истории
	void navigate(const QString &path, bool record = true) {
		QString target = QDir::cleanPath(QFileInfo(path).absoluteFilePath());

		if (!QFileInfo(target).isDir()) {
			QMessageBox::warning(this, QStringLiteral("Files"),
			                     QStringLiteral("Directory is unavailable: ") + target);
			return;
		}

		QModelIndex index = model->setRootPath(target);

		if (!index.isValid()) {
			QMessageBox::warning(this, QStringLiteral("Files"),
			                     QStringLiteral("Cannot open directory: ") + target);
			return;
		}

		current = target;
		view->setRootIndex(index);
		location->setText(target);
		statusBar()->showMessage(target);

		if (record && (historyIndex < 0 || history.at(historyIndex) != target)) {
			while (history.size() > historyIndex + 1) {
				history.removeLast();
			}

			history.append(target);
			historyIndex = history.size() - 1;
		}
	}

	// Вернуться к предыдущему каталогу
	void back() {
		if (historyIndex > 0) {
			historyIndex--;
			navigate(history.at(historyIndex), false);
		}
	}

	// Перейти к следующему каталогу в истории
	void forward() {
		if (historyIndex + 1 < history.size()) {
			historyIndex++;
			navigate(history.at(historyIndex), false);
		}
	}

	// Открыть родительский каталог
	void up() {
		navigate(QFileInfo(current).dir().absolutePath());
	}

	// Получить путь выбранной строки
	QString selectedPath() const {
		return model->filePath(view->currentIndex());
	}

	// Показать каталог, текст или изображение выбранного файла
	void open(const QModelIndex &index) {
		QString path = model->filePath(index);
		QFileInfo info(path);

		if (info.isDir()) {
			navigate(path);
			return;
		}

		if (!info.isFile()) {
			return;
		}

		QImageReader reader(path);

		if (reader.canRead()) {
			QPixmap image = QPixmap::fromImageReader(&reader);
			QLabel *label = new QLabel();
			label->setPixmap(image);
			QScrollArea *area = new QScrollArea();
			area->setAttribute(Qt::WA_DeleteOnClose);
			area->setWindowTitle(info.fileName());
			area->setWidget(label);
			area->resize(800, 600);
			area->show();
			return;
		}

		if (info.size() <= 1024 * 1024) {
			QFile file(path);

			if (file.open(QIODevice::ReadOnly)) {
				QByteArray contents = file.readAll();

				if (!contents.contains('\0')) {
					QTextEdit *preview = new QTextEdit();
					preview->setAttribute(Qt::WA_DeleteOnClose);
					preview->setWindowTitle(info.fileName());
					preview->setReadOnly(true);
					preview->setPlainText(QString::fromUtf8(contents));
					preview->resize(800, 600);
					preview->show();
					return;
				}
			}
		}

		QMessageBox::information(this, info.fileName(),
		                         QStringLiteral("Path: %1\nSize: %2 bytes").arg(path).arg(info.size()));
	}

	// Создать каталог с именем, введённым пользователем
	void newFolder() {
		bool accepted = false;
		QString name = QInputDialog::getText(this, QStringLiteral("New folder"),
		                                     QStringLiteral("Name:"), QLineEdit::Normal, QString(), &accepted);

		if (!accepted) {
			return;
		}

		name = name.trimmed();

		if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..") ||
		    name.contains('/')) {
			QMessageBox::warning(this, QStringLiteral("Files"), QStringLiteral("Invalid folder name"));
			return;
		}

		if (!QDir(current).mkdir(name)) {
			QMessageBox::warning(this, QStringLiteral("Files"), QStringLiteral("Cannot create folder"));
		}
	}

	// Переименовать выбранный файл или каталог
	void renameSelected() {
		QString path = selectedPath();

		if (path.isEmpty() || path == QStringLiteral("/")) {
			return;
		}

		QFileInfo info(path);
		bool accepted = false;
		QString name = QInputDialog::getText(this, QStringLiteral("Rename"),
		                                     QStringLiteral("New name:"), QLineEdit::Normal,
		                                     info.fileName(), &accepted);

		if (!accepted || name == info.fileName()) {
			return;
		}

		name = name.trimmed();

		if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..") ||
		    name.contains('/') || QFileInfo::exists(info.dir().filePath(name)) ||
		    !info.dir().rename(info.fileName(), name)) {
			QMessageBox::warning(this, QStringLiteral("Files"), QStringLiteral("Cannot rename item"));
		}
	}

	// Удалить выбранный объект после подтверждения
	void deleteSelected() {
		QString path = selectedPath();

		if (path.isEmpty() || path == QStringLiteral("/")) {
			return;
		}

		if (QMessageBox::question(this, QStringLiteral("Delete"),
		                          QStringLiteral("Delete %1 permanently?").arg(path)) != QMessageBox::Yes) {
			return;
		}

		QFileInfo info(path);
		bool removed = info.isDir() && !info.isSymLink() ? QDir(path).removeRecursively() : QFile::remove(path);

		if (!removed) {
			QMessageBox::warning(this, QStringLiteral("Files"), QStringLiteral("Cannot delete item"));
		}
	}

	// Запомнить выбранный путь для копирования или переноса
	void rememberSelected(bool cut) {
		QString path = selectedPath();

		if (path.isEmpty() || path == QStringLiteral("/")) {
			return;
		}

		clipboardPath = path;
		clipboardCut = cut;
		statusBar()->showMessage((cut ? QStringLiteral("Cut: ") : QStringLiteral("Copy: ")) + path);
	}

	// Поместить сохранённый объект в текущий каталог
	void paste() {
		if (clipboardPath.isEmpty() || !QFileInfo::exists(clipboardPath)) {
			return;
		}

		QFileInfo source(clipboardPath);
		QString destination = availablePath(QDir(current).filePath(source.fileName()));
		QString sourcePath = QDir::cleanPath(source.absoluteFilePath());
		QString targetPath = QDir::cleanPath(destination);

		if (destination.isEmpty() || (source.isDir() && targetPath.startsWith(sourcePath + '/'))) {
			QMessageBox::warning(this, QStringLiteral("Files"),
			                     QStringLiteral("Cannot paste a folder inside itself"));
			return;
		}

		bool success = false;

		if (clipboardCut) {
			success = QDir().rename(sourcePath, destination);
		} else {
			success = copyPath(sourcePath, destination);
		}

		if (!success) {
			QMessageBox::warning(this, QStringLiteral("Files"),
			                     QStringLiteral("Copy or move failed. Symbolic links cannot be copied."));
			return;
		}

		if (clipboardCut) {
			clipboardPath.clear();
		}
	}
};

// Запустить файловый менеджер или короткую проверку окна в гостевой системе
int main(int argc, char **argv) {
	QApplication application(argc, argv);
	applyDesktopTheme();
	QString initial = QDir::homePath();
	QStringList arguments = application.arguments();

	for (int index = 1; index < arguments.size(); index++) {
		if (!arguments.at(index).startsWith('-')) {
			initial = arguments.at(index);
		}
	}

	FilesWindow window(initial);
	window.show();

	if (arguments.contains(QStringLiteral("--self-test"))) {
		QTimer::singleShot(1500, &application, [&application, &window]() {
			if (!window.ready() || !testFileOperations()) {
				std::puts("SYNAPSE_FILES_FAIL");
				application.exit(1);
				return;
			}

			std::puts("SYNAPSE_FILES_PASS");
			application.quit();
		});
	}

	return application.exec();
}
