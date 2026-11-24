// greeter.cpp: Графический вход, смена пользователя и управление питанием SynapseOS
// Автор: Елчинян Арен Артурович, 2026. AMD64, ABI Qt6 и POSIX
// Подсистема: графический сеанс
// Первое изменение: 30.10.2026
// Последнее изменение: 24.11.2026
// Доступ: общедоступный

#include <QApplication>
#include <QByteArray>
#include <QCloseEvent>
#include <QDateTime>
#include <QFile>
#include <QFont>
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
    application.setFont(QFont(QStringLiteral("DejaVu Sans Mono"), 10));
    LoginScreen window;
    window.setObjectName(QStringLiteral("loginScreen"));
    window.setWindowTitle(QStringLiteral("Вход в SynapseOS"));
    window.setWindowFlags(Qt::FramelessWindowHint);
    window.setStyleSheet(QStringLiteral(
        "#loginScreen { border-image: url(/usr/share/backgrounds/synapse/Desktop-dark.png) 0 0 0 0 stretch stretch; }"
        "QWidget { color: #ffffff; font-size: 15px; }"
        "#loginCard { background-color: #104974; border: 1px solid #8fa6b2; }"
        "QLineEdit { background: #ffffff; color: #30363a; border: 1px solid #aab7be;"
        " padding: 10px 12px; selection-background-color: #006699; border-radius: 0; }"
        "QLineEdit:focus { border: 2px solid #b25e00; }"
        "QPushButton { background: #006699; color: #ffffff; border: 1px solid #8fc3e8;"
        " padding: 10px; font-weight: bold; border-radius: 0; }"
        "QPushButton:hover { background: #185c91; }"
        "QPushButton:focus { border: 2px solid #b25e00; }"
        "#powerButton { background: #104974; border: 1px solid #8fa6b2; padding: 9px; }"
        "#powerButton:hover { background: #185c91; }"));

    auto *root = new QVBoxLayout(&window);
    root->setContentsMargins(40, 28, 40, 28);
    root->setSpacing(0);
    root->addStretch();

    auto *center = new QHBoxLayout();
    center->addStretch();

    auto *card = new QWidget(&window);
    card->setObjectName(QStringLiteral("loginCard"));
    card->setFixedSize(460, 370);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(34, 28, 34, 28);
    layout->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("Вход в систему"), card);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("font-size: 25px; font-weight: bold;"));
    layout->addWidget(title);

    auto *form = new QFormLayout();
    form->setSpacing(12);
    auto *user = new QLineEdit(card);
    auto *password = new QLineEdit(card);
    user->setPlaceholderText(QStringLiteral("Имя пользователя"));
    password->setPlaceholderText(QStringLiteral("Пароль"));
    user->setMinimumHeight(44);
    password->setMinimumHeight(44);
    password->setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("Пользователь"), user);
    form->addRow(QStringLiteral("Пароль"), password);
    layout->addLayout(form);

    auto *error = new QLabel(card);
    error->setWordWrap(true);
    error->setMinimumHeight(28);
    error->setStyleSheet(QStringLiteral("color: #ffcece;"));
    layout->addWidget(error);

    auto *submit = new QPushButton(QStringLiteral("Войти"), card);
    submit->setMinimumHeight(46);
    layout->addWidget(submit);
    center->addWidget(card);
    center->addStretch();
    root->addLayout(center);
    root->addStretch();

    auto *footer = new QHBoxLayout();
    auto *clock = new QLabel(&window);
    clock->setStyleSheet(QStringLiteral("font-size: 15px; color: #ffffff;"));
    auto updateClock = [clock]() {
        clock->setText(QDateTime::currentDateTime().toString(QStringLiteral("dd MMMM yyyy, HH:mm")));
    };
    updateClock();
    auto *timer = new QTimer(&window);
    QObject::connect(timer, &QTimer::timeout, &window, updateClock);
    timer->start(30000);
    footer->addWidget(clock);
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
