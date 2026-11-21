// Музыкальный проигрыватель SynapseOS на libVLC и Qt 6, 21.11.2026.
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QListWidget>
#include <QLineEdit>
#include <QTimer>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <QProcess>
#include <cstdio>
#include <cstring>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc == 3 && !std::strcmp(argv[1], "--check-audio")) {
        execv("/usr/libexec/vlc-audio", argv);
        perror("vlc-audio");
        return 1;
    }
    QApplication app(argc, argv);
    QProcess playback;
    bool paused = false;
    QWidget window;
    window.setWindowTitle(QStringLiteral("Музыка VLC"));
    auto *layout = new QVBoxLayout(&window);
    auto *title = new QLabel(QStringLiteral("Выберите аудиофайл"), &window);
    title->setTextFormat(Qt::PlainText);
    layout->addWidget(title);
    auto *route = new QComboBox(&window);
    route->setAccessibleName(QStringLiteral("Звуковой выход"));
    route->addItem(QStringLiteral("Системный выход"), "/dev/dsp");
    for (const auto &device : {"/dev/audio-hdmi", "/dev/audio-jack"}) {
        if (QFileInfo::exists(device)) route->addItem(QString::fromLatin1(device).endsWith("hdmi") ?
            QStringLiteral("HDMI") : QStringLiteral("Наушники 3,5 мм"), device);
    }
    layout->addWidget(route);
    auto *row = new QHBoxLayout;
    layout->addLayout(row);
    auto *open = new QPushButton(QStringLiteral("Открыть…"));
    auto *play = new QPushButton(QStringLiteral("Слушать"));
    auto *stop = new QPushButton(QStringLiteral("Стоп"));
    for (auto *button : {open, play, stop}) row->addWidget(button);
    auto *volume = new QSlider(Qt::Horizontal);
    volume->setRange(0, 100);
    volume->setValue(75);
    volume->setAccessibleName(QStringLiteral("Громкость"));
    layout->addWidget(volume);
    auto *status = new QLabel;
    layout->addWidget(status);
    QString file;
    auto start = [&]() {
        if (file.isEmpty()) return;
        title->setText(QFileInfo(file).fileName());
        if (playback.state() != QProcess::NotRunning) return;
        paused = false;
        status->setText(QStringLiteral("Открытие…"));
        playback.start("/usr/libexec/vlc-audio", {"--play", route->currentData().toString(), file});
    };
    // Список внутри самого окна работает и без портала выбора файлов Wayland
    auto *picker = new QWidget(&window);
    auto *browse = new QVBoxLayout(picker);
    auto *path = new QLineEdit(picker);
    path->setAccessibleName(QStringLiteral("Каталог или путь к файлу"));
    auto *list = new QListWidget(picker);
    list->setAccessibleName(QStringLiteral("Аудиофайлы"));
    auto *choose = new QPushButton(QStringLiteral("Выбрать"), picker);
    browse->addWidget(path);
    browse->addWidget(list);
    browse->addWidget(choose);
    layout->addWidget(picker);
    picker->hide();
    QString directory = QDir("/data/Music").exists() ? QStringLiteral("/data/Music") : QDir::homePath();
    QString nextFile;
    bool stopped = false;
    auto showDirectory = [&](const QString &name) {
        directory = QDir(name).absolutePath();
        path->setText(directory);
        list->clear();
        QDir dir(directory);
        if (!dir.isRoot()) {
            auto *up = new QListWidgetItem(QStringLiteral("↑  На уровень выше"), list);
            up->setData(Qt::UserRole, QFileInfo(directory).dir().absolutePath());
        }
        const auto entries = dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot,
                                               QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
        const QStringList formats = {"mp3", "aac", "m4a", "wav"};
        for (const auto &entry : entries) {
            if (!entry.isDir() && !formats.contains(entry.suffix().toLower())) continue;
            auto *item = new QListWidgetItem(entry.fileName() + (entry.isDir() ? "/" : ""), list);
            item->setData(Qt::UserRole, entry.absoluteFilePath());
        }
        list->setCurrentRow(dir.isRoot() || list->count() < 2 ? 0 : 1);
        list->setFocus();
    };
    auto selectPath = [&](const QString &name) {
        QFileInfo entry(name);
        if (entry.isDir()) { showDirectory(entry.absoluteFilePath()); return; }
        if (!entry.isFile() || !entry.isReadable()) {
            status->setText(QStringLiteral("Не удалось прочитать файл"));
            return;
        }
        picker->hide();
        file = entry.absoluteFilePath();
        directory = entry.absolutePath();
        if (playback.state() == QProcess::NotRunning) { start(); return; }
        nextFile = file;
        stopped = true;
        playback.putChar(static_cast<char>(202));
        QTimer::singleShot(1200, &window, [&]() {
            if (!nextFile.isEmpty() && playback.state() != QProcess::NotRunning) playback.kill();
        });
    };
    QObject::connect(open, &QPushButton::clicked, [&]() {
        picker->setVisible(!picker->isVisible());
        if (picker->isVisible()) showDirectory(directory);
    });
    auto selectCurrent = [&]() {
        if (auto *item = list->currentItem()) selectPath(item->data(Qt::UserRole).toString());
    };
    QObject::connect(choose, &QPushButton::clicked, selectCurrent);
    QObject::connect(list, &QListWidget::itemActivated, [&](QListWidgetItem *item) {
        selectPath(item->data(Qt::UserRole).toString());
    });
    QObject::connect(path, &QLineEdit::returnPressed, [&]() {
        selectPath(QDir(directory).absoluteFilePath(path->text()));
    });
    auto controls = [&]() {
        bool active = playback.state() != QProcess::NotRunning;
        open->setEnabled(true);
        stop->setEnabled(active);
        play->setEnabled(!file.isEmpty());
        route->setEnabled(!active);
        play->setText(active ? (paused ? QStringLiteral("Продолжить") : QStringLiteral("Пауза")) : QStringLiteral("Слушать"));
    };
    QObject::connect(&playback, &QProcess::started, [&]() {
        playback.putChar(static_cast<char>(volume->value()));
        controls();
    });
    QObject::connect(&playback, &QProcess::readyReadStandardOutput, [&]() {
        while (playback.canReadLine()) {
            bool ok;
            auto sec = playback.readLine().trimmed().toLongLong(&ok);
            if (ok && sec >= 0) status->setText(QStringLiteral("%1:%2").arg(sec / 60).arg(sec % 60, 2, 10, QLatin1Char('0')));
        }
    });
    QObject::connect(&playback, &QProcess::errorOccurred, [&](QProcess::ProcessError) {
        status->setText(playback.errorString());
        controls();
    });
    QObject::connect(&playback, &QProcess::finished, [&](int code, QProcess::ExitStatus how) {
        QString reason = QString::fromLocal8Bit(playback.readAllStandardError()).trimmed();
        if (code || how != QProcess::NormalExit) status->setText(reason.isEmpty() ? QStringLiteral("Ошибка воспроизведения") : reason);
        else status->setText(stopped ? QStringLiteral("Остановлено") : QStringLiteral("Воспроизведение завершено"));
        stopped = paused = false;
        controls();
        if (!nextFile.isEmpty()) {
            file = nextFile;
            nextFile.clear();
            start();
        }
    });
    QObject::connect(volume, &QSlider::valueChanged, [&](int value) {
        if (playback.state() == QProcess::Running) playback.putChar(static_cast<char>(value));
    });
    QObject::connect(play, &QPushButton::clicked, [&]() {
        if (playback.state() == QProcess::NotRunning) start();
        else {
            paused = !paused;
            playback.putChar(static_cast<char>(paused ? 201 : 200));
            controls();
        }
    });
    QObject::connect(stop, &QPushButton::clicked, [&]() { stopped = true; playback.putChar(static_cast<char>(202)); });
    QObject::connect(&app, &QApplication::aboutToQuit, [&]() {
        nextFile.clear();
        if (playback.state() != QProcess::NotRunning) {
            playback.closeWriteChannel();
            if (!playback.waitForFinished(500)) { playback.kill(); playback.waitForFinished(500); }
        }
    });
    controls();
    if (argc == 2) { file = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath(); start(); }
    window.resize(400, 180);
    window.show();
    if (argc == 3 && !std::strcmp(argv[1], "--check-picker")) {
        QString expected = QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath();
        QTimer::singleShot(0, &window, [&, expected]() {
            open->click();
            for (int i = 0; i < list->count(); ++i) {
                if (list->item(i)->data(Qt::UserRole).toString() == expected) {
                    list->setCurrentRow(i);
                    choose->click();
                    break;
                }
            }
            app.exit(file == expected && !picker->isVisible() && playback.state() != QProcess::NotRunning ? 0 : 1);
        });
    }
    return app.exec();
}
