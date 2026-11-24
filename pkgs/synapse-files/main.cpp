/*
    main.cpp: Графический файловый менеджер SynapseOS
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64 и AArch64, ABI: Qt 6 и musl

    Описание: Навигация, поиск, просмотр и операции с файлами через Qt Widgets
    Подсистема: графические программы
    Первое изменение: 25.10.2026
    Последнее изменение: 24.11.2026
    Доступ: общедоступный
*/

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QStyle>
#include <QHeaderView>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPalette>
#include <QSettings>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

// Применить общую переносимую тему штатных программ
static void applyDesktopTheme() {
	QString path = QDir::homePath() + QStringLiteral("/.config/labwc/rc.xml");
	if (!QFileInfo::exists(path)) {
		path = QStringLiteral("/etc/xdg/labwc/rc.xml");
	}
	QFile file(path);
	const QString config = file.open(QIODevice::ReadOnly | QIODevice::Text) ?
	    QString::fromUtf8(file.readAll()) : QString();
	const bool classic = config.contains(QStringLiteral("<name>synapse-classic</name>"));
	const bool light = classic || config.contains(QStringLiteral("<name>Arc-Clone</name>")) ||
	    config.contains(QStringLiteral("<name>Synapse-Light</name>"));
	const bool ocean = config.contains(QStringLiteral("<name>Synapse-Ocean</name>"));
	const bool graphite = config.contains(QStringLiteral("<name>Synapse-Graphite</name>"));
	const bool dracula = config.contains(QStringLiteral("<name>Synapse-Dracula</name>"));
	const bool penumbra = config.contains(QStringLiteral("<name>Penumbra</name>"));
	const bool numix = config.contains(QStringLiteral("<name>Numix-Clone</name>"));
	QColor background(classic ? QStringLiteral("#f4f3ec") : light ? QStringLiteral("#f3f5f8") :
	    ocean ? QStringLiteral("#102a34") : graphite ? QStringLiteral("#202124") : dracula ? QStringLiteral("#282a36") :
	    penumbra ? QStringLiteral("#243842") : numix ? QStringLiteral("#303238") : QStringLiteral("#10161e"));
	QColor base(classic || light ? QStringLiteral("#ffffff") : ocean ? QStringLiteral("#153642") :
	    graphite ? QStringLiteral("#292a2d") : dracula ? QStringLiteral("#1e1f29") :
	    penumbra ? QStringLiteral("#1b2e37") : numix ? QStringLiteral("#26282d") : QStringLiteral("#151e28"));
	QColor surface(classic ? QStringLiteral("#e3e8e9") : light ? QStringLiteral("#e1e7ef") :
	    ocean ? QStringLiteral("#1e4b5a") : graphite ? QStringLiteral("#3c4043") : dracula ? QStringLiteral("#44475a") :
	    penumbra ? QStringLiteral("#304b57") : numix ? QStringLiteral("#41434a") : QStringLiteral("#223142"));
	QColor foreground(classic ? QStringLiteral("#17333c") : light ? QStringLiteral("#172033") :
	    dracula ? QStringLiteral("#f8f8f2") : QStringLiteral("#edf4fb"));
	QColor accent(classic ? QStringLiteral("#006f8f") : ocean ? QStringLiteral("#38bdf8") :
	    graphite ? QStringLiteral("#a8c7fa") : dracula ? QStringLiteral("#bd93f9") : numix ? QStringLiteral("#d65d3a") :
	    penumbra ? QStringLiteral("#769fb4") : light ? QStringLiteral("#2563eb") : QStringLiteral("#4c9aff"));
	QSettings preferences(QStringLiteral("SynapseOS"), QStringLiteral("Desktop"));
	const QStringList keys = {QStringLiteral("background"), QStringLiteral("base"), QStringLiteral("surface"),
	                          QStringLiteral("foreground"), QStringLiteral("accent")};
	QList<QColor *> colors = {&background, &base, &surface, &foreground, &accent};
	for (int index = 0; index < colors.size(); ++index) {
		const QColor configured(preferences.value(QStringLiteral("appearance/") + keys[index]).toString());
		if (configured.isValid()) {
			*colors[index] = configured;
		}
	}
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
	palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(QStringLiteral("#71808b")));
	palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#71808b")));
	palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#71808b")));
	QApplication::setStyle(QStringLiteral("Fusion"));
	QApplication::setPalette(palette);
	QFont font = QApplication::font();
	font.setFamily(preferences.value(QStringLiteral("appearance/font-family"),
	    QStringLiteral("DejaVu Sans Mono")).toString());
	font.setPointSize(preferences.value(QStringLiteral("appearance/font-size"), 10).toInt());
	QApplication::setFont(font);
	const int radius = 0;
	const int padding = preferences.value(QStringLiteral("appearance/padding"), 6).toInt();
	qApp->setStyleSheet(QStringLiteral(
	    "QMainWindow, QDialog { background-color: %1; color: %4; }"
	    "QLineEdit, QListWidget, QTreeView, QListView, QTextEdit, QComboBox { background-color: %2;"
	    " color: %4; border: 1px solid %3; border-radius: %6px; padding: %7px;"
	    " selection-background-color: %5; selection-color: white; }"
	    "QLineEdit:focus, QComboBox:focus { border-color: %5; }"
	    "QPushButton, QToolButton { background-color: transparent; color: %4; border: 1px solid transparent;"
	    " border-radius: %6px; padding: %7px; }"
	    "QPushButton:hover, QToolButton:hover { background-color: %3; border-color: %5; }"
	    "QPushButton:pressed, QToolButton:pressed, QToolButton:checked { background-color: %5; color: white; }"
	    "QToolBar { background-color: %1; border: 0; border-bottom: 1px solid %3; padding: 5px; spacing: 3px; }"
	    "QHeaderView::section { background-color: %3; color: %4; border: 0; padding: 8px; }"
	    "QTreeView::item, QListView::item { padding: 5px; border-radius: %6px; }"
	    "QTreeView::item:hover, QListView::item:hover { background-color: %3; }"
	    "QListWidget#filePlaces { border: 0; border-right: 1px solid %3; border-radius: 0; padding: 8px; }"
	    "QListWidget#filePlaces::item { padding: 7px; border-radius: %6px; }"
	    "QListWidget#filePlaces::item:selected { background-color: %5; color: white; }"
	    "QMenuBar, QMenu { background-color: %2; color: %4; }"
	    "QMenu::item { padding: 7px 24px; }"
	    "QMenu::item:selected { background-color: %5; color: white; }"
	    "QStatusBar { background-color: %2; color: %4; border-top: 1px solid %3; }"
	    "QScrollBar:vertical { background: %1; width: 12px; margin: 0; }"
	    "QScrollBar::handle:vertical { background: %3; min-height: 28px; border-radius: %6px; }"
	    "QScrollBar::handle:vertical:hover { background: %5; }"
	    "QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }")
	    .arg(background.name(), base.name(), surface.name(), foreground.name(), accent.name())
	    .arg(radius).arg(padding));
}

// Скопировать файл или каталог, сохранив символические ссылки
static bool copyPath(const QString &source, const QString &destination) {
	QFileInfo info(source);
	if (info.isSymLink()) {
		const QByteArray target = QFile::encodeName(info.symLinkTarget());
		const QByteArray path = QFile::encodeName(destination);
		return symlink(target.constData(), path.constData()) == 0;
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
	for (int number = 1; number < 10000; ++number) {
		const QString candidate = path + QStringLiteral(" (копия %1)").arg(number);
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
	const QString source = temporary.filePath(QStringLiteral("source"));
	const QString nested = QDir(source).filePath(QStringLiteral("nested"));
	if (!QDir().mkpath(nested)) {
		return false;
	}
	QFile input(QDir(nested).filePath(QStringLiteral("note.txt")));
	if (!input.open(QIODevice::WriteOnly) || input.write("SynapseOS files\n") != 16) {
		return false;
	}
	input.close();
	const QString destination = temporary.filePath(QStringLiteral("copy"));
	if (!copyPath(source, destination)) {
		return false;
	}
	QFile output(QDir(destination).filePath(QStringLiteral("nested/note.txt")));
	return output.open(QIODevice::ReadOnly) && output.readAll() == QByteArray("SynapseOS files\n") &&
	    availablePath(destination) == destination + QStringLiteral(" (копия 1)");
}

class FileListModel final : public QFileSystemModel {
public:
	using QFileSystemModel::QFileSystemModel;

	QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
		if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
			static const QStringList names = {QStringLiteral("Имя"), QStringLiteral("Размер"),
			    QStringLiteral("Тип"), QStringLiteral("Изменён")};
			if (section >= 0 && section < names.size()) {
				return names[section];
			}
		}
		if (orientation == Qt::Horizontal && role == Qt::DecorationRole) {
			return {};
		}
		return QFileSystemModel::headerData(section, orientation, role);
	}
};

class FilesWindow final : public QMainWindow {
public:
	explicit FilesWindow(const QString &initial) {
		setWindowTitle(QStringLiteral("Файлы"));
		resize(1100, 700);
		setMinimumSize(720, 460);
		model = new FileListModel(this);
		model->setReadOnly(false);
		model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System);

		details = new QTreeView(this);
		configureView(details);
		details->setRootIsDecorated(false);
		details->setItemsExpandable(false);
		details->setUniformRowHeights(true);
		details->setAlternatingRowColors(true);
		details->setSortingEnabled(true);
		details->sortByColumn(0, Qt::AscendingOrder);
		details->header()->setSectionResizeMode(0, QHeaderView::Stretch);
		details->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
		details->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
		details->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

		icons = new QListView(this);
		configureView(icons);
		icons->setViewMode(QListView::IconMode);
		icons->setIconSize(QSize(56, 56));
		icons->setGridSize(QSize(120, 100));
		icons->setResizeMode(QListView::Adjust);
		icons->setMovement(QListView::Static);
		icons->setWordWrap(true);
		views = new QStackedWidget(this);
		views->addWidget(details);
		views->addWidget(icons);

		places = new QListWidget(this);
		places->setObjectName(QStringLiteral("filePlaces"));
		places->setMinimumWidth(185);
		places->setMaximumWidth(240);
		places->setIconSize(QSize(20, 20));
		places->setSpacing(2);
		addPlace(QStringLiteral("Домой"), QDir::homePath());
		addPlace(QStringLiteral("Рабочий стол"), QDir::homePath() + QStringLiteral("/Desktop"));
		addPlace(QStringLiteral("Документы"), QDir::homePath() + QStringLiteral("/Documents"));
		addPlace(QStringLiteral("Загрузки"), QDir::homePath() + QStringLiteral("/Downloads"));
		addPlace(QStringLiteral("Изображения"), QDir::homePath() + QStringLiteral("/Pictures"));
		addPlace(QStringLiteral("Музыка"), QDir::homePath() + QStringLiteral("/Music"));
		addPlace(QStringLiteral("Видео"), QDir::homePath() + QStringLiteral("/Videos"));
		addPlace(QStringLiteral("Корзина"), trashPath());
		addPlace(QStringLiteral("Вся система"), QStringLiteral("/"));
		if (QDir(QStringLiteral("/data")).exists()) {
			addPlace(QStringLiteral("Данные"), QStringLiteral("/data"));
		}
		for (const QStorageInfo &volume : QStorageInfo::mountedVolumes()) {
			if (volume.isReady() && volume.rootPath() != QStringLiteral("/") &&
			    !volume.rootPath().startsWith(QStringLiteral("/proc")) &&
			    !volume.rootPath().startsWith(QStringLiteral("/synapse"))) {
				addPlace(QStringLiteral("Том: %1").arg(volume.rootPath()), volume.rootPath());
			}
		}
		auto *splitter = new QSplitter(this);
		splitter->addWidget(places);
		splitter->addWidget(views);
		splitter->setStretchFactor(1, 1);
		setCentralWidget(splitter);
		buildActions();
		buildMenus();
		connect(places, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
			navigate(item->data(Qt::UserRole).toString());
		});
		connect(details, &QTreeView::doubleClicked, this, &FilesWindow::open);
		connect(icons, &QListView::doubleClicked, this, &FilesWindow::open);
		for (QAbstractItemView *view : {static_cast<QAbstractItemView *>(details),
		                              static_cast<QAbstractItemView *>(icons)}) {
			view->setContextMenuPolicy(Qt::CustomContextMenu);
			connect(view, &QWidget::customContextMenuRequested, this, &FilesWindow::showContextMenu);
			connect(view->selectionModel(), &QItemSelectionModel::selectionChanged,
			    this, &FilesWindow::updateStatus);
		}
		connect(model, &QFileSystemModel::directoryLoaded, this, [this](const QString &) {
			applyFilter();
			updateStatus();
		});
		QFileInfo initialEntry(initial);
		if (initialEntry.isFile()) {
			navigate(initialEntry.absolutePath());
			QModelIndex selected = model->index(initialEntry.absoluteFilePath());
			details->setCurrentIndex(selected);
			icons->setCurrentIndex(selected);
			QTimer::singleShot(0, this, [this, selected]() { open(selected); });
		} else {
			navigate(initial);
		}
	}

	bool ready() const {
		return details->rootIndex().isValid() && QFileInfo(current).isDir();
	}

private:
	QFileSystemModel *model = nullptr;
	QTreeView *details = nullptr;
	QListView *icons = nullptr;
	QStackedWidget *views = nullptr;
	QListWidget *places = nullptr;
	QLineEdit *location = nullptr;
	QLineEdit *search = nullptr;
	QComboBox *viewMode = nullptr;
	QString current;
	QStringList clipboardPaths;
	QStringList history;
	int historyIndex = -1;
	bool clipboardCut = false;
	bool showHidden = false;
	QAction *pasteAction = nullptr;
	QAction *hiddenAction = nullptr;

	void configureView(QAbstractItemView *view) {
		view->setModel(model);
		view->setSelectionMode(QAbstractItemView::ExtendedSelection);
		view->setSelectionBehavior(QAbstractItemView::SelectRows);
		view->setEditTriggers(QAbstractItemView::NoEditTriggers);
		view->setDragDropMode(QAbstractItemView::NoDragDrop);
	}

	void addPlace(const QString &title, const QString &path) {
		QString iconName = QStringLiteral("folder");
		if (title == QStringLiteral("Домой")) iconName = QStringLiteral("user-home");
		if (title == QStringLiteral("Рабочий стол")) iconName = QStringLiteral("user-desktop");
		if (title == QStringLiteral("Документы")) iconName = QStringLiteral("folder-documents");
		if (title == QStringLiteral("Загрузки")) iconName = QStringLiteral("folder-download");
		if (title == QStringLiteral("Изображения")) iconName = QStringLiteral("folder-pictures");
		if (title == QStringLiteral("Музыка")) iconName = QStringLiteral("folder-music");
		if (title == QStringLiteral("Видео")) iconName = QStringLiteral("folder-videos");
		if (title == QStringLiteral("Корзина")) iconName = QStringLiteral("user-trash");
		if (path == QStringLiteral("/") || title.startsWith(QStringLiteral("Том:"))) {
			iconName = QStringLiteral("drive-harddisk");
		}
		QIcon icon = QIcon::fromTheme(iconName, style()->standardIcon(QStyle::SP_DirIcon));
		auto *item = new QListWidgetItem(icon, title, places);
		item->setData(Qt::UserRole, path);
		item->setToolTip(path);
		item->setSizeHint(QSize(0, 34));
	}

	void buildActions() {
		auto *navigation = addToolBar(QStringLiteral("Навигация"));
		navigation->setMovable(false);
		navigation->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		addButton(navigation, QStringLiteral("Назад"), &FilesWindow::back, QStringLiteral("Alt+Left"));
		addButton(navigation, QStringLiteral("Вперёд"), &FilesWindow::forward, QStringLiteral("Alt+Right"));
		addButton(navigation, QStringLiteral("Вверх"), &FilesWindow::up, QStringLiteral("Alt+Up"));
		addButton(navigation, QStringLiteral("Домой"), [this]() { navigate(QDir::homePath()); });
		location = new QLineEdit(this);
		location->setPlaceholderText(QStringLiteral("Путь к каталогу"));
		navigation->addWidget(location);
		addButton(navigation, QStringLiteral("Перейти"), [this]() { navigate(location->text()); });
		addButton(navigation, QStringLiteral("Обновить"), [this]() { refresh(); }, QStringLiteral("F5"));
		connect(location, &QLineEdit::returnPressed, this, [this]() { navigate(location->text()); });

		auto *operations = addToolBar(QStringLiteral("Файловые операции"));
		operations->setMovable(false);
		addButton(operations, QStringLiteral("Создать"), &FilesWindow::createEntry, QStringLiteral("Ctrl+Shift+N"));
		addButton(operations, QStringLiteral("Копировать"), [this]() { rememberSelected(false); }, QStringLiteral("Ctrl+C"));
		addButton(operations, QStringLiteral("Вырезать"), [this]() { rememberSelected(true); }, QStringLiteral("Ctrl+X"));
		pasteAction = addButton(operations, QStringLiteral("Вставить"), &FilesWindow::paste, QStringLiteral("Ctrl+V"));
		addButton(operations, QStringLiteral("Переименовать"), &FilesWindow::renameSelected, QStringLiteral("F2"));
		addButton(operations, QStringLiteral("В корзину"), [this]() { removeSelected(false); }, QStringLiteral("Delete"));
		addButton(operations, QStringLiteral("Свойства"), &FilesWindow::properties, QStringLiteral("Alt+Return"));
		addButton(operations, QStringLiteral("Открыть с помощью"), &FilesWindow::openWith);
		addButton(operations, QStringLiteral("В терминале"), [this]() {
			QProcess::startDetached(QStringLiteral("/bin/weston-terminal"), {}, current);
		});
		hiddenAction = addButton(operations, QStringLiteral("Скрытые файлы"), &FilesWindow::toggleHidden);
		hiddenAction->setCheckable(true);
		search = new QLineEdit(this);
		search->setPlaceholderText(QStringLiteral("Поиск в текущем каталоге"));
		search->setClearButtonEnabled(true);
		search->setMaximumWidth(270);
		operations->addWidget(search);
		viewMode = new QComboBox(this);
		viewMode->addItems({QStringLiteral("Подробно"), QStringLiteral("Значки")});
		operations->addWidget(viewMode);
		connect(search, &QLineEdit::textChanged, this, &FilesWindow::applyFilter);
		connect(viewMode, &QComboBox::currentIndexChanged, views, &QStackedWidget::setCurrentIndex);
		addShortcut(QKeySequence(QStringLiteral("Ctrl+L")), [this]() { location->setFocus(); });
		addShortcut(QKeySequence(QStringLiteral("Ctrl+F")), [this]() { search->setFocus(); });
		addShortcut(QKeySequence(QStringLiteral("Ctrl+A")), [this]() { activeView()->selectAll(); });
		addShortcut(QKeySequence(QStringLiteral("Ctrl+H")), [this]() { toggleHidden(); });
		addShortcut(QKeySequence(QStringLiteral("Shift+Delete")), [this]() { removeSelected(true); });
	}

	void buildMenus() {
		auto *file = menuBar()->addMenu(QStringLiteral("Файл"));
		file->addAction(QStringLiteral("Создать файл или каталог"), this, &FilesWindow::createEntry);
		file->addAction(QStringLiteral("Свойства"), this, &FilesWindow::properties);
		file->addSeparator();
		file->addAction(QStringLiteral("Закрыть"), this, &QWidget::close);
		auto *edit = menuBar()->addMenu(QStringLiteral("Правка"));
		edit->addAction(QStringLiteral("Копировать"), this, [this]() { rememberSelected(false); });
		edit->addAction(QStringLiteral("Вырезать"), this, [this]() { rememberSelected(true); });
		edit->addAction(pasteAction);
		edit->addAction(QStringLiteral("Переименовать"), this, &FilesWindow::renameSelected);
		edit->addAction(QStringLiteral("В корзину"), this, [this]() { removeSelected(false); });
		auto *view = menuBar()->addMenu(QStringLiteral("Вид"));
		view->addAction(hiddenAction);
		view->addAction(QStringLiteral("Обновить"), this, &FilesWindow::refresh);
	}

	template <typename Handler>
	QAction *addButton(QToolBar *toolbar, const QString &title, Handler handler, const QString &shortcut = {}) {
		QAction *action = toolbar->addAction(title);
		static const QHash<QString, QString> icons = {
		    {QStringLiteral("Назад"), QStringLiteral("go-previous")},
		    {QStringLiteral("Вперёд"), QStringLiteral("go-next")},
		    {QStringLiteral("Вверх"), QStringLiteral("go-up")},
		    {QStringLiteral("Домой"), QStringLiteral("go-home")},
		    {QStringLiteral("Перейти"), QStringLiteral("go-jump")},
		    {QStringLiteral("Обновить"), QStringLiteral("view-refresh")},
		    {QStringLiteral("Создать"), QStringLiteral("folder-new")},
		    {QStringLiteral("Копировать"), QStringLiteral("edit-copy")},
		    {QStringLiteral("Вырезать"), QStringLiteral("edit-cut")},
		    {QStringLiteral("Вставить"), QStringLiteral("edit-paste")},
		    {QStringLiteral("Переименовать"), QStringLiteral("edit-rename")},
		    {QStringLiteral("В корзину"), QStringLiteral("user-trash")},
		    {QStringLiteral("Свойства"), QStringLiteral("document-properties")},
		    {QStringLiteral("Скрытые файлы"), QStringLiteral("view-visible")},
		    {QStringLiteral("В терминале"), QStringLiteral("utilities-terminal")}
		};
		action->setIcon(QIcon::fromTheme(icons.value(title)));
		connect(action, &QAction::triggered, this, handler);
		if (!shortcut.isEmpty()) {
			addShortcut(QKeySequence(shortcut), [action]() { action->trigger(); });
		}
		return action;
	}

	template <typename Handler>
	void addShortcut(const QKeySequence &sequence, Handler handler) {
		auto *shortcut = new QShortcut(sequence, this);
		connect(shortcut, &QShortcut::activated, this, handler);
	}

	QAbstractItemView *activeView() const {
		return views->currentIndex() == 0 ? static_cast<QAbstractItemView *>(details) :
		    static_cast<QAbstractItemView *>(icons);
	}

	QStringList selectedPaths() const {
		QStringList paths;
		for (const QModelIndex &index : activeView()->selectionModel()->selectedRows(0)) {
			paths.append(model->filePath(index));
		}
		return paths;
	}

	QString selectedPath() const {
		return model->filePath(activeView()->currentIndex());
	}

	void navigate(const QString &path, bool record = true) {
		const QString target = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
		if (!QFileInfo(target).isDir()) {
			statusBar()->showMessage(QStringLiteral("Каталог недоступен: %1").arg(target), 5000);
			return;
		}
		const QModelIndex index = model->setRootPath(target);
		if (!index.isValid()) {
			statusBar()->showMessage(QStringLiteral("Не удалось открыть каталог: %1").arg(target), 5000);
			return;
		}
		current = target;
		details->setRootIndex(index);
		icons->setRootIndex(index);
		location->setText(target);
		setWindowTitle(QStringLiteral("%1: Файлы").arg(target == QStringLiteral("/") ? target : QFileInfo(target).fileName()));
		if (record && (historyIndex < 0 || history.at(historyIndex) != target)) {
			while (history.size() > historyIndex + 1) {
				history.removeLast();
			}
			history.append(target);
			historyIndex = history.size() - 1;
		}
		applyFilter();
		updateStatus();
	}

	void refresh() {
		model->setRootPath(QString());
		navigate(current, false);
	}

	void back() {
		if (historyIndex > 0) {
			--historyIndex;
			navigate(history.at(historyIndex), false);
		}
	}

	void forward() {
		if (historyIndex + 1 < history.size()) {
			++historyIndex;
			navigate(history.at(historyIndex), false);
		}
	}

	void up() {
		navigate(QFileInfo(current).dir().absolutePath());
	}

	void applyFilter() {
		const QString query = search ? search->text().trimmed() : QString();
		const QModelIndex root = model->index(current);
		for (int row = 0; row < model->rowCount(root); ++row) {
			const QModelIndex index = model->index(row, 0, root);
			const bool hide = !query.isEmpty() && !model->fileName(index).contains(query, Qt::CaseInsensitive);
			details->setRowHidden(row, root, hide);
			icons->setRowHidden(row, hide);
		}
		updateStatus();
	}

	void toggleHidden() {
		showHidden = !showHidden;
		model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System |
		    (showHidden ? QDir::Hidden : QDir::Filter()));
		hiddenAction->setChecked(showHidden);
		refresh();
	}

	void updateStatus() {
		const QStringList selected = selectedPaths();
		qulonglong bytes = 0;
		for (const QString &path : selected) {
			bytes += QFileInfo(path).size();
		}
		statusBar()->showMessage(selected.isEmpty() ?
		    QStringLiteral("%1  |  Объектов: %2").arg(current).arg(model->rowCount(model->index(current))) :
		    QStringLiteral("Выбрано: %1  |  Размер файлов: %2 байт").arg(selected.size()).arg(bytes));
		pasteAction->setEnabled(!clipboardPaths.isEmpty());
	}

	void open(const QModelIndex &index) {
		const QString path = model->filePath(index);
		QFileInfo info(path);
		if (info.isDir()) {
			navigate(path);
			return;
		}
		if (!info.isFile()) {
			return;
		}
		static const QStringList media = {"mp3", "m4a", "aac", "wav", "flac", "ogg", "opus",
		    "mp4", "mkv", "avi", "mov", "webm", "mpeg", "mpg", "ts", "m4v"};
		if (media.contains(info.suffix().toLower()) && QFileInfo(QStringLiteral("/bin/vlc")).isExecutable()) {
			if (!QProcess::startDetached(QStringLiteral("/bin/vlc"), {info.absoluteFilePath()})) {
				QMessageBox::warning(this, info.fileName(), QStringLiteral("Не удалось запустить VLC"));
			}
			return;
		}
		QImageReader reader(path);
		if (reader.canRead()) {
			QPixmap image = QPixmap::fromImageReader(&reader);
			auto *label = new QLabel();
			label->setPixmap(image);
			auto *area = new QScrollArea();
			area->setAttribute(Qt::WA_DeleteOnClose);
			area->setWindowTitle(info.fileName());
			area->setWidget(label);
			area->resize(900, 650);
			area->show();
			return;
		}
		if (info.size() <= 4 * 1024 * 1024) {
			QFile file(path);
			if (file.open(QIODevice::ReadOnly)) {
				const QByteArray contents = file.readAll();
				if (!contents.contains('\0')) {
					auto *preview = new QTextEdit();
					preview->setAttribute(Qt::WA_DeleteOnClose);
					preview->setWindowTitle(info.fileName());
					preview->setReadOnly(true);
					preview->setPlainText(QString::fromUtf8(contents));
					preview->resize(850, 650);
					preview->show();
					return;
				}
			}
		}
		properties();
	}

	// Запустить выбранный файл через явно указанную программу без shell
	void openWith() {
		const QString path = selectedPath();
		if (path.isEmpty() || !QFileInfo(path).isFile()) {
			return;
		}
		bool accepted = false;
		const QString program = QInputDialog::getItem(this, QStringLiteral("Открыть с помощью"),
		    QStringLiteral("Программа"), {QStringLiteral("/bin/vlc"), QStringLiteral("Встроенный просмотр")},
		    0, false, &accepted);
		if (!accepted) {
			return;
		}
		if (program == QStringLiteral("Встроенный просмотр")) {
			open(activeView()->currentIndex());
			return;
		}
		if (!QProcess::startDetached(program, {path})) {
			QMessageBox::warning(this, QStringLiteral("Файлы"), QStringLiteral("Не удалось запустить программу"));
		}
	}

	void createEntry() {
		QDialog dialog(this);
		dialog.setWindowTitle(QStringLiteral("Создать объект"));
		auto *layout = new QFormLayout(&dialog);
		auto *type = new QComboBox(&dialog);
		type->addItems({QStringLiteral("Каталог"), QStringLiteral("Пустой файл")});
		auto *name = new QLineEdit(&dialog);
		layout->addRow(QStringLiteral("Тип"), type);
		layout->addRow(QStringLiteral("Имя"), name);
		auto *accept = new QPushButton(QStringLiteral("Создать"), &dialog);
		layout->addRow(accept);
		connect(accept, &QPushButton::clicked, &dialog, [&]() {
			const QString value = name->text().trimmed();
			if (value.isEmpty() || value == QStringLiteral(".") || value == QStringLiteral("..") ||
			    value.contains(QLatin1Char('/'))) {
				QMessageBox::warning(&dialog, QStringLiteral("Файлы"), QStringLiteral("Недопустимое имя"));
				return;
			}
			const QString path = QDir(current).filePath(value);
			bool created = false;
			if (type->currentIndex() == 0) {
				created = QDir(current).mkdir(value);
			} else {
				QSaveFile file(path);
				created = !QFileInfo::exists(path) && file.open(QIODevice::WriteOnly) && file.commit();
			}
			if (!created) {
				QMessageBox::warning(&dialog, QStringLiteral("Файлы"), QStringLiteral("Не удалось создать объект"));
				return;
			}
			dialog.accept();
			refresh();
		});
		dialog.exec();
	}

	void renameSelected() {
		const QString path = selectedPath();
		if (path.isEmpty() || path == QStringLiteral("/")) {
			return;
		}
		QFileInfo info(path);
		bool accepted = false;
		QString name = QInputDialog::getText(this, QStringLiteral("Переименовать"),
		    QStringLiteral("Новое имя:"), QLineEdit::Normal, info.fileName(), &accepted);
		name = name.trimmed();
		if (!accepted || name == info.fileName()) {
			return;
		}
		if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..") ||
		    name.contains(QLatin1Char('/')) || QFileInfo::exists(info.dir().filePath(name)) ||
		    !info.dir().rename(info.fileName(), name)) {
			QMessageBox::warning(this, QStringLiteral("Файлы"), QStringLiteral("Не удалось переименовать объект"));
		}
	}

	static QString trashPath() {
		return QDir::homePath() + QStringLiteral("/.local/share/Trash/files");
	}

	void removeSelected(bool permanent) {
		const QStringList paths = selectedPaths();
		if (paths.isEmpty()) {
			return;
		}
		if (QMessageBox::question(this, permanent ? QStringLiteral("Удалить навсегда") : QStringLiteral("В корзину"),
		    permanent ? QStringLiteral("Удалить выбранные объекты без возможности восстановления?") :
		        QStringLiteral("Переместить выбранные объекты в корзину?")) != QMessageBox::Yes) {
			return;
		}
		if (!permanent && !QDir().mkpath(trashPath())) {
			QMessageBox::warning(this, QStringLiteral("Файлы"), QStringLiteral("Корзина недоступна"));
			return;
		}
		QStringList failed;
		for (const QString &path : paths) {
			if (path == QStringLiteral("/") || path == trashPath()) {
				failed.append(path);
				continue;
			}
			QFileInfo info(path);
			if (permanent) {
				const bool removed = info.isDir() && !info.isSymLink() ? QDir(path).removeRecursively() : QFile::remove(path);
				if (!removed) {
					failed.append(path);
				}
				continue;
			}
			const QString target = availablePath(QDir(trashPath()).filePath(info.fileName()));
			bool moved = QDir().rename(path, target);
			if (!moved && copyPath(path, target)) {
				moved = info.isDir() && !info.isSymLink() ? QDir(path).removeRecursively() : QFile::remove(path);
			}
			if (!moved) {
				failed.append(path);
			}
		}
		if (!failed.isEmpty()) {
			QMessageBox::warning(this, QStringLiteral("Файлы"),
			    QStringLiteral("Не удалось обработать:\n%1").arg(failed.join(QLatin1Char('\n'))));
		}
		refresh();
	}

	void rememberSelected(bool cut) {
		clipboardPaths = selectedPaths();
		clipboardCut = cut;
		statusBar()->showMessage(QStringLiteral("%1 объектов: %2")
		    .arg(cut ? QStringLiteral("Вырезано") : QStringLiteral("Скопировано")).arg(clipboardPaths.size()));
		updateStatus();
	}

	void paste() {
		QStringList failed;
		for (const QString &sourcePath : clipboardPaths) {
			QFileInfo source(sourcePath);
			if (!source.exists()) {
				failed.append(sourcePath);
				continue;
			}
			const QString destination = availablePath(QDir(current).filePath(source.fileName()));
			const QString cleanSource = QDir::cleanPath(source.absoluteFilePath());
			const QString cleanTarget = QDir::cleanPath(destination);
			if (destination.isEmpty() || (source.isDir() && cleanTarget.startsWith(cleanSource + QLatin1Char('/')))) {
				failed.append(sourcePath);
				continue;
			}
			const bool success = clipboardCut ? QDir().rename(cleanSource, destination) : copyPath(cleanSource, destination);
			if (!success) {
				failed.append(sourcePath);
			}
		}
		if (clipboardCut) {
			clipboardPaths.clear();
		}
		if (!failed.isEmpty()) {
			QMessageBox::warning(this, QStringLiteral("Файлы"),
			    QStringLiteral("Не удалось вставить:\n%1").arg(failed.join(QLatin1Char('\n'))));
		}
		refresh();
	}

	void properties() {
		const QString path = selectedPath();
		QFileInfo info(path);
		if (path.isEmpty() || !info.exists()) {
			return;
		}
		QDialog dialog(this);
		dialog.setWindowTitle(QStringLiteral("Свойства: %1").arg(info.fileName()));
		auto *layout = new QFormLayout(&dialog);
		layout->addRow(QStringLiteral("Имя"), new QLabel(info.fileName(), &dialog));
		layout->addRow(QStringLiteral("Путь"), new QLabel(info.absoluteFilePath(), &dialog));
		layout->addRow(QStringLiteral("Тип"), new QLabel(info.isDir() ? QStringLiteral("Каталог") :
		    info.isSymLink() ? QStringLiteral("Символическая ссылка") : QStringLiteral("Файл"), &dialog));
		layout->addRow(QStringLiteral("Размер"), new QLabel(QStringLiteral("%1 байт").arg(info.size()), &dialog));
		layout->addRow(QStringLiteral("Владелец"), new QLabel(QStringLiteral("%1:%2").arg(info.owner(), info.group()), &dialog));
		layout->addRow(QStringLiteral("Изменён"), new QLabel(info.lastModified().toString(Qt::ISODate), &dialog));
		layout->addRow(QStringLiteral("Права"), new QLabel(QStringLiteral("%1").arg(static_cast<int>(info.permissions()), 0, 8), &dialog));
		auto *close = new QPushButton(QStringLiteral("Закрыть"), &dialog);
		layout->addRow(close);
		connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
		dialog.exec();
	}

	void showContextMenu(const QPoint &position) {
		QMenu menu(this);
		menu.addAction(QStringLiteral("Открыть"), this, [this]() { open(activeView()->currentIndex()); });
		menu.addSeparator();
		menu.addAction(QStringLiteral("Копировать"), this, [this]() { rememberSelected(false); });
		menu.addAction(QStringLiteral("Вырезать"), this, [this]() { rememberSelected(true); });
		menu.addAction(pasteAction);
		menu.addAction(QStringLiteral("Переименовать"), this, &FilesWindow::renameSelected);
		menu.addAction(QStringLiteral("В корзину"), this, [this]() { removeSelected(false); });
		menu.addSeparator();
		menu.addAction(QStringLiteral("Свойства"), this, &FilesWindow::properties);
		menu.exec(activeView()->viewport()->mapToGlobal(position));
	}
};

// Запустить файловый менеджер или короткую проверку окна в гостевой системе
int main(int argc, char **argv) {
	QApplication application(argc, argv);
	applyDesktopTheme();
	QString initial = QDir::homePath();
	const QStringList arguments = application.arguments();
	for (int index = 1; index < arguments.size(); ++index) {
		if (!arguments.at(index).startsWith(QLatin1Char('-'))) {
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
