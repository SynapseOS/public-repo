// greeter.cpp: Окно выбора пользователя SynapseOS
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: musl и Wayland
// Описание: Передать введённые данные менеджеру по закрытому каналу
// Подсистема: графический сеанс
// Первое изменение: 30.10.2026
// Последнее изменение: 30.10.2026
// Доступ: общедоступный

#include <QApplication>
#include <QByteArray>
#include <QCloseEvent>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

struct LoginRequest {
    char user[64];
    char password[256];
};

class LoginScreen : public QWidget {
protected:
    // Оставаться на экране входа при Alt+F4 и запросе закрытия окна
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

// Показать экран входа без доступа к панели и терминалу root
int main(int argc, char **argv) {
    const char *number = std::getenv("SYNAPSE_GREETER_FD");
    if (number == nullptr) {
        return 1;
    }

    char *end = nullptr;
    long value = std::strtol(number, &end, 10);
    if (end == number || *end != 0 || value < 3 || value > 1024) {
        return 1;
    }

    int channel = static_cast<int>(value);
    QApplication application(argc, argv);
    LoginScreen window;
    window.setObjectName("loginScreen");
    window.setWindowTitle(QString::fromUtf8("Вход в SynapseOS"));
    window.setWindowFlags(Qt::FramelessWindowHint);
    const char *wallpaper = "/usr/share/backgrounds/synapse/Desktop-dark.png";
    if (QFile::exists(QString::fromUtf8(wallpaper))) {
        window.setStyleSheet("#loginScreen { border-image: url(/usr/share/backgrounds/synapse/"
                             "Desktop-dark.png) 0 0 0 0 stretch stretch; } "
                             "QWidget { color: #eff5fa; font-size: 15px; }");
    } else {
        window.setStyleSheet("#loginScreen { background: #1b2530; } "
                             "QWidget { color: #eff5fa; font-size: 15px; }");
    }

    auto *screen = new QVBoxLayout(&window);
    screen->setContentsMargins(24, 24, 24, 24);
    screen->addStretch();

    auto *row = new QHBoxLayout();
    row->addStretch();
    auto *card = new QWidget(&window);
    card->setObjectName("loginCard");
    card->setFixedWidth(440);
    card->setStyleSheet("#loginCard { background: #283746; border: 1px solid #4b6478; "
                        "border-radius: 14px; } "
                        "QLineEdit { background: #f8fbfd; color: #172633; border: 1px solid #7296ae; "
                        "border-radius: 6px; padding: 8px; } "
                        "QPushButton { background: #56a9e8; color: #102434; border: 0; "
                        "border-radius: 6px; padding: 10px; font-weight: bold; } "
                        "QPushButton:hover { background: #80c3f2; }");
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(32, 28, 32, 28);
    layout->setSpacing(16);
    auto *title = new QLabel(QString::fromUtf8("SynapseOS"));
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("font-size: 32px; font-weight: bold; padding: 8px");
    layout->addWidget(title);

    auto *subtitle = new QLabel(QString::fromUtf8("Вход в систему"));
    subtitle->setAlignment(Qt::AlignCenter);
    subtitle->setStyleSheet("color: #abc1d1");
    layout->addWidget(subtitle);

    auto *error = new QLabel(QString::fromUtf8(std::getenv("SYNAPSE_LOGIN_ERROR") != nullptr ?
        std::getenv("SYNAPSE_LOGIN_ERROR") : ""));
    error->setAlignment(Qt::AlignCenter);
    error->setStyleSheet("color: #ffadad");
    layout->addWidget(error);

    auto *form = new QFormLayout();
    auto *user = new QLineEdit();
    auto *password = new QLineEdit();
    user->setPlaceholderText(QString::fromUtf8("Имя пользователя"));
    password->setPlaceholderText(QString::fromUtf8("Пароль"));
    password->setEchoMode(QLineEdit::Password);
    form->addRow(QString::fromUtf8("Пользователь"), user);
    form->addRow(QString::fromUtf8("Пароль"), password);
    layout->addLayout(form);

    auto *submit = new QPushButton(QString::fromUtf8("Войти"));
    layout->addWidget(submit);

    row->addWidget(card);
    row->addStretch();
    screen->addLayout(row);
    screen->addStretch();

    QObject::connect(submit, &QPushButton::clicked, &window, [&]() {
        QByteArray name = user->text().toUtf8();
        QByteArray secret = password->text().toUtf8();
        if (name.isEmpty() || name.size() >= 64 || secret.isEmpty() || secret.size() >= 256) {
            error->setText(QString::fromUtf8("Проверьте имя пользователя и пароль"));
            secret.fill(0);
            return;
        }

        LoginRequest request{};
        std::memcpy(request.user, name.constData(), static_cast<size_t>(name.size()));
        std::memcpy(request.password, secret.constData(), static_cast<size_t>(secret.size()));
        bool written = writeRequest(channel, request);
        std::fprintf(stderr, "synapse-greeter: login request write %s (%zu bytes)\n",
                     written ? "ok" : "failed", sizeof(request));
        clearSecret(&request, sizeof(request));
        secret.fill(0);
        password->clear();
        if (written) {
            application.quit();
        } else {
            error->setText(QString::fromUtf8("Не удалось передать данные входа"));
        }
    });

    QObject::connect(password, &QLineEdit::returnPressed, submit, &QPushButton::click);
    window.showFullScreen();
    user->setFocus();
    int result = application.exec();
    close(channel);
    return result;
}
