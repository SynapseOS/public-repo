/*
    player.cpp: Медиаплеер VLC с выводом видео и звука для SynapseOS
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64 и AArch64, ABI: Qt 6, libVLC и musl

    Описание: Воспроизведение медиа через libVLC с плейлистом и полным экраном
    Подсистема: мультимедиа
    Первое изменение: 21.11.2026
    Последнее изменение: 22.11.2026
    Доступ: общедоступный
*/

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QPushButton>
#include <QRandomGenerator>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QVector>
#include <vlc/vlc.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

struct VideoFrame {
    QMutex mutex;
    QVector<unsigned char> storage;
    unsigned char *data = nullptr;
    unsigned width = 0;
    unsigned height = 0;
    bool updated = false;
};

// Владеть одним экземпляром libVLC и системным звуковым устройством
class PlaybackEngine {
public:
    PlaybackEngine() {
        instance = libvlc_new(0, nullptr);
    }

    ~PlaybackEngine() {
        stop();
        if (instance) {
            libvlc_release(instance);
        }
        closeAudio();
    }

    bool play(const QString &source, bool location = false) {
        if (!instance || source.isEmpty()) {
            return false;
        }
        stop();
        const QByteArray encoded = location ? source.toUtf8() : QFile::encodeName(source);
        libvlc_media_t *media = location ? libvlc_media_new_location(instance, encoded.constData()) :
            libvlc_media_new_path(instance, encoded.constData());
        if (!media) {
            return false;
        }
        player = libvlc_media_player_new_from_media(media);
        libvlc_media_release(media);
        if (!player) {
            return false;
        }
        openAudio();
        libvlc_audio_set_callbacks(player, playAudio, nullptr, nullptr, flushAudio, drainAudio, this);
        libvlc_audio_set_format(player, "S16N", 48000, 2);
        libvlc_video_set_callbacks(player, lockVideo, unlockVideo, displayVideo, &frame);
        libvlc_video_set_format_callbacks(player, setupVideo, cleanupVideo);
        return libvlc_media_player_play(player) == 0;
    }

    void stop() {
        if (player) {
            libvlc_media_player_stop(player);
            libvlc_media_player_release(player);
            player = nullptr;
        }
        QMutexLocker locker(&frame.mutex);
        frame.storage.clear();
        frame.data = nullptr;
        frame.width = 0;
        frame.height = 0;
        frame.updated = false;
    }

    void setOutput(const QString &path) {
        closeAudio();
        output = path;
        openAudio();
    }

    void setVolume(int value) {
        volume.store(qBound(0, value, 100));
    }

    libvlc_instance_t *instance = nullptr;
    libvlc_media_player_t *player = nullptr;
    VideoFrame frame;
    std::atomic<int> volume{75};

private:
    QString output = QStringLiteral("/dev/dsp");
    QMutex audioMutex;
    int audioDevice = -1;

    void openAudio() {
        QMutexLocker locker(&audioMutex);
        if (audioDevice >= 0) {
            return;
        }
        audioDevice = open(QFile::encodeName(output).constData(), O_WRONLY | O_CLOEXEC);
        if (audioDevice < 0) {
            return;
        }
        int format = AFMT_S16_LE;
        int channels = 2;
        int rate = 48000;
        if (ioctl(audioDevice, SNDCTL_DSP_SETFMT, &format) < 0 ||
            ioctl(audioDevice, SNDCTL_DSP_CHANNELS, &channels) < 0 ||
            ioctl(audioDevice, SNDCTL_DSP_SPEED, &rate) < 0 || format != AFMT_S16_LE ||
            channels != 2 || rate != 48000) {
            close(audioDevice);
            audioDevice = -1;
        }
    }

    void closeAudio() {
        QMutexLocker locker(&audioMutex);
        if (audioDevice >= 0) {
            close(audioDevice);
            audioDevice = -1;
        }
    }

    static unsigned setupVideo(void **opaque, char *chroma, unsigned *width, unsigned *height,
                               unsigned *pitches, unsigned *lines) {
        auto *frame = static_cast<VideoFrame *>(*opaque);
        if (!frame || !width || !height || !pitches || !lines || *width == 0 || *height == 0 ||
            *width > 16384 || *height > 16384) {
            return 0;
        }
        std::memcpy(chroma, "RV32", 4);
        QMutexLocker locker(&frame->mutex);
        frame->width = *width;
        frame->height = *height;
        frame->storage.resize(static_cast<qsizetype>(*width) * *height * 4 + 32);
        const uintptr_t address = reinterpret_cast<uintptr_t>(frame->storage.data());
        frame->data = reinterpret_cast<unsigned char *>((address + 31) & ~uintptr_t(31));
        pitches[0] = *width * 4;
        lines[0] = *height;
        return 1;
    }

    static void cleanupVideo(void *opaque) {
        auto *frame = static_cast<VideoFrame *>(opaque);
        if (frame) {
            QMutexLocker locker(&frame->mutex);
            frame->storage.clear();
            frame->data = nullptr;
            frame->updated = false;
        }
    }

    static void *lockVideo(void *opaque, void **planes) {
        auto *frame = static_cast<VideoFrame *>(opaque);
        if (!frame || !planes) {
            return nullptr;
        }
        frame->mutex.lock();
        planes[0] = frame->data;
        return frame->data;
    }

    static void unlockVideo(void *opaque, void *picture, void *const *planes) {
        Q_UNUSED(planes);
        auto *frame = static_cast<VideoFrame *>(opaque);
        if (frame && picture) {
            frame->mutex.unlock();
        }
    }

    static void displayVideo(void *opaque, void *picture) {
        auto *frame = static_cast<VideoFrame *>(opaque);
        if (frame && picture) {
            frame->updated = true;
        }
    }

    static void playAudio(void *opaque, const void *samples, unsigned count, int64_t) {
        auto *engine = static_cast<PlaybackEngine *>(opaque);
        if (!engine || !samples) {
            return;
        }
        QMutexLocker locker(&engine->audioMutex);
        if (engine->audioDevice < 0) {
            return;
        }
        const auto *source = static_cast<const int16_t *>(samples);
        const int gain = engine->volume.load();
        int16_t chunk[2048];
        while (count) {
            const unsigned items = qMin(count, 1024U);
            for (unsigned index = 0; index < items * 2; ++index) {
                chunk[index] = static_cast<int16_t>(source[index] * gain / 100);
            }
            size_t written = 0;
            const size_t bytes = static_cast<size_t>(items) * 4;
            while (written < bytes) {
                const ssize_t result = write(engine->audioDevice, reinterpret_cast<char *>(chunk) + written,
                                             bytes - written);
                if (result < 0 && errno == EINTR) {
                    continue;
                }
                if (result <= 0) {
                    return;
                }
                written += static_cast<size_t>(result);
            }
            source += items * 2;
            count -= items;
        }
    }

    static void flushAudio(void *opaque, int64_t) {
        auto *engine = static_cast<PlaybackEngine *>(opaque);
        if (engine) {
            QMutexLocker locker(&engine->audioMutex);
            if (engine->audioDevice >= 0) {
                ioctl(engine->audioDevice, SNDCTL_DSP_RESET, 0);
            }
        }
    }

    static void drainAudio(void *opaque) {
        auto *engine = static_cast<PlaybackEngine *>(opaque);
        if (engine) {
            QMutexLocker locker(&engine->audioMutex);
            if (engine->audioDevice >= 0) {
                ioctl(engine->audioDevice, SNDCTL_DSP_SYNC, 0);
            }
        }
    }
};

class PlayerWindow final : public QMainWindow {
public:
    explicit PlayerWindow(const QStringList &sources) {
        setWindowTitle(QStringLiteral("VLC media player"));
        setAcceptDrops(true);
        resize(1100, 720);
        setMinimumSize(640, 420);
        auto *central = new QWidget(this);
        auto *layout = new QVBoxLayout(central);
        layout->setContentsMargins(0, 0, 0, 0);
        video = new QLabel(QStringLiteral("Откройте медиафайл или добавьте адрес потока"), central);
        video->setAlignment(Qt::AlignCenter);
        video->setMinimumSize(480, 300);
        video->setStyleSheet(QStringLiteral("QLabel { background: #05070a; color: #b8c5d3; }"));
        layout->addWidget(video, 1);
        auto *seekRow = new QHBoxLayout();
        elapsed = new QLabel(QStringLiteral("00:00"), central);
        position = new QSlider(Qt::Horizontal, central);
        position->setRange(0, 1000);
        duration = new QLabel(QStringLiteral("00:00"), central);
        seekRow->addWidget(elapsed);
        seekRow->addWidget(position, 1);
        seekRow->addWidget(duration);
        layout->addLayout(seekRow);
        auto *controls = new QHBoxLayout();
        previous = new QPushButton(QStringLiteral("Назад"), central);
        play = new QPushButton(QStringLiteral("Пуск"), central);
        stopButton = new QPushButton(QStringLiteral("Стоп"), central);
        next = new QPushButton(QStringLiteral("Далее"), central);
        fullscreen = new QPushButton(QStringLiteral("Полный экран"), central);
        volume = new QSlider(Qt::Horizontal, central);
        volume->setRange(0, 100);
        volume->setValue(75);
        volume->setMaximumWidth(150);
        for (auto *button : {previous, play, stopButton, next, fullscreen}) {
            controls->addWidget(button);
        }
        controls->addStretch();
        controls->addWidget(new QLabel(QStringLiteral("Громкость"), central));
        controls->addWidget(volume);
        layout->addLayout(controls);
        auto *options = new QHBoxLayout();
        output = new QComboBox(central);
        output->addItem(QStringLiteral("Системный выход"), QStringLiteral("/dev/dsp"));
        for (const QString &path : {QStringLiteral("/dev/audio-hdmi"), QStringLiteral("/dev/audio-jack")}) {
            if (QFileInfo::exists(path)) {
                output->addItem(path == QStringLiteral("/dev/audio-hdmi") ? QStringLiteral("HDMI") :
                    QStringLiteral("Наушники 3,5 мм"), path);
            }
        }
        rate = new QComboBox(central);
        for (const QString &value : {QStringLiteral("0.5"), QStringLiteral("0.75"), QStringLiteral("1.0"),
                                    QStringLiteral("1.25"), QStringLiteral("1.5"), QStringLiteral("2.0")}) {
            rate->addItem(value + QStringLiteral("×"), value);
        }
        rate->setCurrentIndex(2);
        audioTrack = new QComboBox(central);
        subtitles = new QComboBox(central);
        repeat = new QCheckBox(QStringLiteral("Повтор"), central);
        shuffle = new QCheckBox(QStringLiteral("Случайно"), central);
        options->addWidget(output);
        options->addWidget(rate);
        options->addWidget(audioTrack, 1);
        options->addWidget(subtitles, 1);
        options->addWidget(repeat);
        options->addWidget(shuffle);
        layout->addLayout(options);
        setCentralWidget(central);
        buildPlaylist();
        buildMenus();
        buildShortcuts();
        connect(play, &QPushButton::clicked, this, &PlayerWindow::togglePlayback);
        connect(stopButton, &QPushButton::clicked, this, [this]() {
            engine.stop();
            video->setPixmap(QPixmap());
            video->setText(QStringLiteral("Воспроизведение остановлено"));
        });
        connect(previous, &QPushButton::clicked, this, [this]() { playAt(currentIndex - 1); });
        connect(next, &QPushButton::clicked, this, [this]() { advance(); });
        connect(fullscreen, &QPushButton::clicked, this, &PlayerWindow::toggleFullscreen);
        connect(volume, &QSlider::valueChanged, this, [this](int value) { engine.setVolume(value); });
        connect(output, &QComboBox::currentIndexChanged, this, [this]() {
            engine.setOutput(output->currentData().toString());
        });
        connect(rate, &QComboBox::currentIndexChanged, this, [this]() {
            if (engine.player) {
                libvlc_media_player_set_rate(engine.player, rate->currentData().toFloat());
            }
        });
        connect(position, &QSlider::sliderMoved, this, [this](int value) {
            if (engine.player) {
                libvlc_media_player_set_time(engine.player, libvlc_media_player_get_length(engine.player) * value / 1000);
            }
        });
        connect(audioTrack, &QComboBox::currentIndexChanged, this, [this]() {
            if (engine.player && audioTrack->currentIndex() >= 0) {
                libvlc_audio_set_track(engine.player, audioTrack->currentData().toInt());
            }
        });
        connect(subtitles, &QComboBox::currentIndexChanged, this, [this]() {
            if (engine.player && subtitles->currentIndex() >= 0) {
                libvlc_video_set_spu(engine.player, subtitles->currentData().toInt());
            }
        });
        timer = new QTimer(this);
        timer->setInterval(33);
        connect(timer, &QTimer::timeout, this, &PlayerWindow::refresh);
        timer->start();
        for (const QString &source : sources) {
            playlist->addItem(source);
        }
        if (playlist->count()) {
            playAt(0);
        }
    }

    ~PlayerWindow() override = default;

protected:
    void closeEvent(QCloseEvent *event) override {
        engine.stop();
        QMainWindow::closeEvent(event);
    }

    void dragEnterEvent(QDragEnterEvent *event) override {
        if (event->mimeData()->hasUrls()) {
            event->acceptProposedAction();
        }
    }

    void dropEvent(QDropEvent *event) override {
        for (const QUrl &url : event->mimeData()->urls()) {
            if (url.isLocalFile()) {
                playlist->addItem(url.toLocalFile());
            }
        }
        if (currentIndex < 0 && playlist->count()) {
            playAt(0);
        }
        event->acceptProposedAction();
    }

private:
    PlaybackEngine engine;
    QLabel *video = nullptr;
    QLabel *elapsed = nullptr;
    QLabel *duration = nullptr;
    QSlider *position = nullptr;
    QSlider *volume = nullptr;
    QPushButton *previous = nullptr;
    QPushButton *play = nullptr;
    QPushButton *stopButton = nullptr;
    QPushButton *next = nullptr;
    QPushButton *fullscreen = nullptr;
    QComboBox *output = nullptr;
    QComboBox *rate = nullptr;
    QComboBox *audioTrack = nullptr;
    QComboBox *subtitles = nullptr;
    QCheckBox *repeat = nullptr;
    QCheckBox *shuffle = nullptr;
    QListWidget *playlist = nullptr;
    QDockWidget *playlistDock = nullptr;
    QTimer *timer = nullptr;
    int currentIndex = -1;
    bool tracksLoaded = false;
    bool seekUpdating = false;

    static QString timeText(qint64 milliseconds) {
        const qint64 seconds = qMax<qint64>(0, milliseconds / 1000);
        return QStringLiteral("%1:%2:%3").arg(seconds / 3600, 2, 10, QLatin1Char('0'))
            .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'));
    }

    void buildPlaylist() {
        playlistDock = new QDockWidget(QStringLiteral("Плейлист"), this);
        auto *content = new QWidget(playlistDock);
        auto *layout = new QVBoxLayout(content);
        auto *search = new QLineEdit(content);
        search->setPlaceholderText(QStringLiteral("Фильтр плейлиста"));
        layout->addWidget(search);
        playlist = new QListWidget(content);
        playlist->setAlternatingRowColors(true);
        layout->addWidget(playlist, 1);
        auto *row = new QHBoxLayout();
        auto *add = new QPushButton(QStringLiteral("Добавить"), content);
        auto *remove = new QPushButton(QStringLiteral("Убрать"), content);
        auto *clear = new QPushButton(QStringLiteral("Очистить"), content);
        row->addWidget(add);
        row->addWidget(remove);
        row->addWidget(clear);
        layout->addLayout(row);
        playlistDock->setWidget(content);
        addDockWidget(Qt::RightDockWidgetArea, playlistDock);
        connect(add, &QPushButton::clicked, this, &PlayerWindow::addFiles);
        connect(remove, &QPushButton::clicked, this, [this]() {
            const int row = playlist->currentRow();
            if (row >= 0) {
                delete playlist->takeItem(row);
                if (row == currentIndex) {
                    currentIndex = -1;
                    engine.stop();
                } else if (row < currentIndex) {
                    --currentIndex;
                }
            }
        });
        connect(clear, &QPushButton::clicked, this, [this]() {
            engine.stop();
            playlist->clear();
            currentIndex = -1;
        });
        connect(playlist, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
            playAt(playlist->row(item));
        });
        connect(search, &QLineEdit::textChanged, this, [this](const QString &text) {
            for (int index = 0; index < playlist->count(); ++index) {
                playlist->item(index)->setHidden(!text.isEmpty() &&
                    !playlist->item(index)->text().contains(text, Qt::CaseInsensitive));
            }
        });
    }

    void buildMenus() {
        auto *file = menuBar()->addMenu(QStringLiteral("Файл"));
        file->addAction(QStringLiteral("Открыть файлы…"), this, &PlayerWindow::addFiles);
        file->addAction(QStringLiteral("Открыть адрес…"), this, &PlayerWindow::addLocation);
        file->addSeparator();
        file->addAction(QStringLiteral("Выход"), this, &QWidget::close);
        auto *playback = menuBar()->addMenu(QStringLiteral("Воспроизведение"));
        playback->addAction(QStringLiteral("Пуск / пауза"), this, &PlayerWindow::togglePlayback);
        playback->addAction(QStringLiteral("Следующий"), this, &PlayerWindow::advance);
        playback->addAction(QStringLiteral("Полный экран"), this, &PlayerWindow::toggleFullscreen);
        auto *view = menuBar()->addMenu(QStringLiteral("Вид"));
        view->addAction(playlistDock->toggleViewAction());
        auto *help = menuBar()->addMenu(QStringLiteral("Справка"));
        help->addAction(QStringLiteral("О VLC"), this, [this]() {
            QMessageBox::about(this, QStringLiteral("VLC media player"),
                QStringLiteral("VLC 3.0.24\nВоспроизведение: VideoLAN libVLC и FFmpeg\nИнтерфейс: Qt 6"));
        });
    }

    void buildShortcuts() {
        auto bind = [this](const QKeySequence &key, auto handler) {
            auto *shortcut = new QShortcut(key, this);
            connect(shortcut, &QShortcut::activated, this, handler);
        };
        bind(QKeySequence(Qt::Key_Space), [this]() { togglePlayback(); });
        bind(QKeySequence(Qt::Key_F11), [this]() { toggleFullscreen(); });
        bind(QKeySequence(Qt::Key_Escape), [this]() {
            if (isFullScreen()) {
                showNormal();
            }
        });
        bind(QKeySequence(Qt::Key_Left), [this]() { seekRelative(-5000); });
        bind(QKeySequence(Qt::Key_Right), [this]() { seekRelative(5000); });
        bind(QKeySequence(Qt::Key_Up), [this]() { volume->setValue(volume->value() + 5); });
        bind(QKeySequence(Qt::Key_Down), [this]() { volume->setValue(volume->value() - 5); });
    }

    void addFiles() {
        const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("Открыть медиа"),
            QDir::homePath(), QStringLiteral(
            "Медиафайлы (*.mp4 *.mkv *.avi *.mov *.webm *.mpeg *.mpg *.ts *.m4v *.mp3 *.m4a *.aac *.wav *.flac *.ogg *.opus);;Все файлы (*)"));
        if (files.isEmpty()) {
            return;
        }
        playlist->addItems(files);
        if (currentIndex < 0) {
            playAt(playlist->count() - files.size());
        }
    }

    void addLocation() {
        bool accepted = false;
        const QString url = QInputDialog::getText(this, QStringLiteral("Открыть адрес"),
            QStringLiteral("HTTP, HTTPS, RTSP или другой поддерживаемый адрес"), QLineEdit::Normal,
            QStringLiteral("https://"), &accepted);
        if (!accepted || url.trimmed().isEmpty()) {
            return;
        }
        const int row = playlist->count();
        auto *item = new QListWidgetItem(url.trimmed(), playlist);
        item->setData(Qt::UserRole, true);
        playAt(row);
    }

    void playAt(int index) {
        if (index < 0 || index >= playlist->count()) {
            return;
        }
        currentIndex = index;
        playlist->setCurrentRow(index);
        const auto *item = playlist->item(index);
        const bool location = item->data(Qt::UserRole).toBool();
        tracksLoaded = false;
        audioTrack->clear();
        subtitles->clear();
        if (!engine.play(item->text(), location)) {
            statusBar()->showMessage(QStringLiteral("Не удалось открыть %1").arg(item->text()), 8000);
            return;
        }
        setWindowTitle(QStringLiteral("%1: VLC").arg(location ? item->text() : QFileInfo(item->text()).fileName()));
        if (rate->currentIndex() >= 0) {
            libvlc_media_player_set_rate(engine.player, rate->currentData().toFloat());
        }
        statusBar()->showMessage(QStringLiteral("Воспроизведение: %1").arg(item->text()));
    }

    void togglePlayback() {
        if (!engine.player) {
            if (currentIndex < 0 && playlist->count()) {
                playAt(qMax(0, playlist->currentRow()));
            }
            return;
        }
        const auto state = libvlc_media_player_get_state(engine.player);
        libvlc_media_player_set_pause(engine.player, state == libvlc_Playing ? 1 : 0);
    }

    void advance() {
        if (!playlist->count()) {
            return;
        }
        if (shuffle->isChecked() && playlist->count() > 1) {
            int nextIndex = currentIndex;
            while (nextIndex == currentIndex) {
                nextIndex = QRandomGenerator::global()->bounded(playlist->count());
            }
            playAt(nextIndex);
            return;
        }
        if (currentIndex + 1 < playlist->count()) {
            playAt(currentIndex + 1);
        } else if (repeat->isChecked()) {
            playAt(0);
        } else {
            engine.stop();
        }
    }

    void seekRelative(qint64 milliseconds) {
        if (engine.player) {
            libvlc_media_player_set_time(engine.player,
                qMax<qint64>(0, libvlc_media_player_get_time(engine.player) + milliseconds));
        }
    }

    void toggleFullscreen() {
        if (isFullScreen()) {
            showNormal();
        } else {
            showFullScreen();
        }
    }

    void loadTracks() {
        if (!engine.player || tracksLoaded) {
            return;
        }
        tracksLoaded = true;
        {
            const QSignalBlocker blocker(audioTrack);
            auto *tracks = libvlc_audio_get_track_description(engine.player);
            for (auto *track = tracks; track; track = track->p_next) {
                audioTrack->addItem(QStringLiteral("Аудио: %1").arg(QString::fromUtf8(track->psz_name)), track->i_id);
            }
            if (tracks) {
                libvlc_track_description_list_release(tracks);
            }
            audioTrack->setCurrentIndex(audioTrack->findData(libvlc_audio_get_track(engine.player)));
        }
        {
            const QSignalBlocker blocker(subtitles);
            subtitles->addItem(QStringLiteral("Субтитры: выключены"), -1);
            auto *tracks = libvlc_video_get_spu_description(engine.player);
            for (auto *track = tracks; track; track = track->p_next) {
                subtitles->addItem(QString::fromUtf8(track->psz_name), track->i_id);
            }
            if (tracks) {
                libvlc_track_description_list_release(tracks);
            }
            subtitles->setCurrentIndex(qMax(0, subtitles->findData(libvlc_video_get_spu(engine.player))));
        }
    }

    void refresh() {
        {
            QMutexLocker locker(&engine.frame.mutex);
            if (engine.frame.updated && engine.frame.data) {
                const QImage image(engine.frame.data, engine.frame.width, engine.frame.height,
                                   engine.frame.width * 4, QImage::Format_ARGB32);
                video->setPixmap(QPixmap::fromImage(image.copy()).scaled(video->size(), Qt::KeepAspectRatio,
                                                                          Qt::SmoothTransformation));
                engine.frame.updated = false;
            }
        }
        if (!engine.player) {
            play->setText(QStringLiteral("Пуск"));
            return;
        }
        const auto state = libvlc_media_player_get_state(engine.player);
        play->setText(state == libvlc_Playing ? QStringLiteral("Пауза") : QStringLiteral("Пуск"));
        const qint64 current = libvlc_media_player_get_time(engine.player);
        const qint64 total = libvlc_media_player_get_length(engine.player);
        elapsed->setText(timeText(current));
        duration->setText(timeText(total));
        if (!position->isSliderDown()) {
            position->setValue(total > 0 ? static_cast<int>(current * 1000 / total) : 0);
        }
        if (state == libvlc_Playing || state == libvlc_Paused) {
            loadTracks();
        }
        if (state == libvlc_Ended) {
            QTimer::singleShot(0, this, &PlayerWindow::advance);
        } else if (state == libvlc_Error) {
            statusBar()->showMessage(QStringLiteral("Ошибка декодирования или открытия медиа"), 8000);
            engine.stop();
        }
    }
};

int main(int argc, char **argv) {
    if (getenv("VLC_PLUGIN_PATH") == nullptr) {
        setenv("VLC_PLUGIN_PATH", "/usr/lib/vlc/plugins", 1);
    }
    if (argc == 3 && !std::strcmp(argv[1], "--check-audio")) {
        execv("/usr/libexec/vlc-audio", argv);
        perror("vlc-audio");
        return 1;
    }
    QApplication application(argc, argv);
    QStringList sources;
    for (int index = 1; index < argc; ++index) {
        if (argv[index][0] != '-') {
            sources.append(QFileInfo(QString::fromLocal8Bit(argv[index])).absoluteFilePath());
        }
    }
    PlayerWindow window(sources);
    window.show();
    if (argc == 3 && !std::strcmp(argv[1], "--check-picker")) {
        QTimer::singleShot(500, &application, [&application]() { application.quit(); });
    }
    return application.exec();
}
