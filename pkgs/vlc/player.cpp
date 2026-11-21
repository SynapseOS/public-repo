// Музыкальный проигрыватель SynapseOS на libVLC и Qt 6, 21.11.2026.
#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
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
    QObject::connect(open, &QPushButton::clicked, [&]() {
        QString initial = file.isEmpty() && QDir("/data/Music").exists() ? QStringLiteral("/data/Music") : file;
        auto selected = QFileDialog::getOpenFileName(&window, QStringLiteral("Открыть музыку"), initial,
            QStringLiteral("Музыка (*.mp3 *.m4a *.aac *.wav);;Все файлы (*)"));
        if (!selected.isEmpty()) { file = selected; start(); }
    });
    auto controls = [&]() {
        bool active = playback.state() != QProcess::NotRunning;
        open->setEnabled(!active);
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
    bool stopped = false;
    QObject::connect(&playback, &QProcess::finished, [&](int code, QProcess::ExitStatus how) {
        QString reason = QString::fromLocal8Bit(playback.readAllStandardError()).trimmed();
        if (code || how != QProcess::NormalExit) status->setText(reason.isEmpty() ? QStringLiteral("Ошибка воспроизведения") : reason);
        else status->setText(stopped ? QStringLiteral("Остановлено") : QStringLiteral("Воспроизведение завершено"));
        stopped = paused = false;
        controls();
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
        if (playback.state() != QProcess::NotRunning) {
            playback.closeWriteChannel();
            if (!playback.waitForFinished(500)) { playback.kill(); playback.waitForFinished(500); }
        }
    });
    controls();
    if (argc == 2) { file = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath(); start(); }
    window.resize(400, 180);
    window.show();
    return app.exec();
}
