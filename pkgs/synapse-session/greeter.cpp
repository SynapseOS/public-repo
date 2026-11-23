// greeter.cpp: Графический вход, смена пользователя и управление питанием SynapseOS
// Автор: Елчинян Арен Артурович, 2026. AMD64, ABI Qt6 и POSIX
// Подсистема: графический сеанс
// Первое изменение: 30.10.2026
// Последнее изменение: 23.11.2026
// Доступ: общедоступный

#include <QApplication>
#include <QByteArray>
#include <QCloseEvent>
#include <QDateTime>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct LoginRequest {
    char user[64];
    char password[256];
};

class LoginScreen final : public QWidget {
protected:
    void closeEvent(QCloseEvent *event) override {
        event->ignore();
    }
};

// Очистить временную копию пароля после отправки
static void clearSecret(void *memory, size_t length) {
    volatile unsigned char *bytes = static_cast<volatile unsigned char *>(memory);
    while (length != 0) {
        *bytes = 0;
        bytes++;
        length--;
    }
}

// Передать запрос полностью, даже если канал принял только часть данных
static bool writeRequest(int fd, const LoginRequest &request) {
    const char *bytes = reinterpret_cast<const char *>(&request);
    size_t sent = 0;
    while (sent < sizeof(request)) {
        ssize_t count = write(fd, bytes + sent, sizeof(request) - sent);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        sent += static_cast<size_t>(count);
    }
    return true;
}

// Подключить окно входа к менеджеру через выделенный локальный сокет
static int connectManager() {
    const char *path = std::getenv("SYNAPSE_GREETER_SOCKET");
    if (path == nullptr || std::strlen(path) >= sizeof(sockaddr_un::sun_path)) {
        std::fputs("synapse-greeter: invalid login socket path\n", stderr);
        return -1;
    }

    int channel = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (channel < 0) {
        std::perror("synapse-greeter: socket");
        return -1;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path, std::strlen(path) + 1);
    if (connect(channel, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        std::perror("synapse-greeter: connect");
        close(channel);
        return -1;
    }
    return channel;
}

int main(int argc, char **argv) {
    int channel = connectManager();
    if (channel < 0) {
        return 1;
    }

    QApplication application(argc, argv);
    application.setStyle(QStringLiteral("Fusion"));
    LoginScreen window;
    window.setObjectName(QStringLiteral("loginScreen"));
    window.setWindowTitle(QStringLiteral("Вход в SynapseOS"));
    window.setWindowFlags(Qt::FramelessWindowHint);
    window.setStyleSheet(QStringLiteral(
        "#loginScreen { border-image: url(/usr/share/backgrounds/synapse/Desktop-dark.png) 0 0 0 0 stretch stretch; }"
        "QWidget { color: #edf4fa; font-size: 15px; }"
        "#brand { background: transparent; }"
        "#loginCard { background-color: rgba(20, 30, 42, 240); border: 1px solid #40566a; }"
        "QLineEdit { background: #142230; color: #f5f9fc; border: 1px solid #526b80;"
        " padding: 11px 12px; selection-background-color: #3daee9; }"
        "QLineEdit:focus { border: 1px solid #3daee9; }"
        "QPushButton { background: #3daee9; color: #07131c; border: 0; padding: 12px; font-weight: bold; }"
        "QPushButton:hover { background: #6bc5f2; }"
        "QPushButton:focus { border: 1px solid #b8e4fa; }"
        "#powerButton { background: transparent; color: #c1d2df; border: 1px solid #40566a; padding: 9px; }"
        "#powerButton:hover { background: #24384a; color: #ffffff; }"));

    auto *root = new QVBoxLayout(&window);
    root->setContentsMargins(48, 32, 48, 32);
    root->setSpacing(24);

    auto *header = new QHBoxLayout();
    auto *brand = new QLabel(QStringLiteral("SynapseOS"), &window);
    brand->setObjectName(QStringLiteral("brand"));
    brand->setStyleSheet(QStringLiteral("font-size: 27px; font-weight: bold; color: #ffffff;"));
    header->addWidget(brand);
    header->addStretch();
    auto *clock = new QLabel(&window);
    clock->setObjectName(QStringLiteral("brand"));
    clock->setStyleSheet(QStringLiteral("font-size: 16px; color: #d0dfeb;"));
    auto updateClock = [clock]() {
        clock->setText(QDateTime::currentDateTime().toString(QStringLiteral("dd MMMM yyyy, HH:mm")));
    };
    updateClock();
    auto *timer = new QTimer(&window);
    QObject::connect(timer, &QTimer::timeout, &window, updateClock);
    timer->start(30000);
    header->addWidget(clock);
    root->addLayout(header);

    auto *center = new QHBoxLayout();
    center->addStretch();
    auto *card = new QWidget(&window);
    card->setObjectName(QStringLiteral("loginCard"));
    card->setFixedSize(520, 430);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(42, 30, 42, 30);
    layout->setSpacing(14);

    auto *title = new QLabel(QStringLiteral("Вход в систему"), card);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("font-size: 28px; font-weight: bold; padding-bottom: 6px;"));
    layout->addWidget(title);

    auto *description = new QLabel(QStringLiteral("Используйте любую локальную учётную запись"), card);
    description->setAlignment(Qt::AlignCenter);
    description->setStyleSheet(QStringLiteral("color: #b6c8d6; padding-bottom: 8px;"));
    layout->addWidget(description);

    auto *form = new QFormLayout();
    form->setSpacing(12);
    auto *user = new QLineEdit(card);
    auto *password = new QLineEdit(card);
    user->setPlaceholderText(QStringLiteral("Имя пользователя"));
    password->setPlaceholderText(QStringLiteral("Пароль"));
    user->setMinimumHeight(46);
    password->setMinimumHeight(46);
    password->setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("Пользователь"), user);
    form->addRow(QStringLiteral("Пароль"), password);
    layout->addLayout(form);

    auto *error = new QLabel(card);
    error->setAlignment(Qt::AlignCenter);
    error->setWordWrap(true);
    error->setMinimumHeight(24);
    error->setStyleSheet(QStringLiteral("color: #ffadad;"));
    layout->addWidget(error);

    auto *submit = new QPushButton(QStringLiteral("Войти"), card);
    submit->setMinimumHeight(48);
    layout->addWidget(submit);
    center->addWidget(card);
    center->addStretch();
    root->addLayout(center, 1);

    auto *footer = new QHBoxLayout();
    footer->addStretch();
    auto *shutdown = new QPushButton(QStringLiteral("Выключить"), &window);
    auto *reboot = new QPushButton(QStringLiteral("Перезагрузить"), &window);
    shutdown->setObjectName(QStringLiteral("powerButton"));
    reboot->setObjectName(QStringLiteral("powerButton"));
    shutdown->setMinimumWidth(150);
    reboot->setMinimumWidth(170);
    footer->addWidget(shutdown);
    footer->addWidget(reboot);
    root->addLayout(footer);

    auto power = [&window, error](bool restart) {
        const QString question = restart ? QStringLiteral("Перезагрузить компьютер?") :
            QStringLiteral("Выключить компьютер?");
        if (QMessageBox::question(&window, QStringLiteral("Питание"), question) != QMessageBox::Yes) {
            return;
        }
        sync();
        const bool started = restart ? QProcess::startDetached(QStringLiteral("/bin/reboot"), {}) :
            QProcess::startDetached(QStringLiteral("/bin/halt"), {QStringLiteral("-p")});
        if (!started) {
            error->setText(QStringLiteral("Не удалось выполнить команду питания"));
        }
    };

    QObject::connect(shutdown, &QPushButton::clicked, &window, [=, &window, &error]() { power(false); });
    QObject::connect(reboot, &QPushButton::clicked, &window, [=, &window, &error]() { power(true); });
    QObject::connect(submit, &QPushButton::clicked, &window, [&]() {
        QByteArray name = user->text().toUtf8();
        QByteArray secret = password->text().toUtf8();
        if (name.isEmpty() || name.size() >= 64 || secret.isEmpty() || secret.size() >= 256) {
            error->setText(QStringLiteral("Введите имя пользователя и пароль"));
            secret.fill(0);
            return;
        }

        LoginRequest request{};
        std::memcpy(request.user, name.constData(), static_cast<size_t>(name.size()));
        std::memcpy(request.password, secret.constData(), static_cast<size_t>(secret.size()));
        const bool written = writeRequest(channel, request);
        std::fprintf(stderr, "synapse-greeter: login request write %s (%zu bytes)\n",
                     written ? "ok" : "failed", sizeof(request));
        clearSecret(&request, sizeof(request));
        secret.fill(0);
        password->clear();
        if (written) {
            error->setText(QStringLiteral("Запуск рабочего стола..."));
            application.quit();
        } else {
            error->setText(QStringLiteral("Не удалось передать данные входа"));
        }
    });

    QObject::connect(password, &QLineEdit::returnPressed, submit, &QPushButton::click);
    window.showFullScreen();
    user->setFocus();
    const int result = application.exec();
    close(channel);
    return result;
}
