// Synapse Recorder, панель записи Desktop, 24.11.2026
#include "ink.h"
#include <QPointer>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QWindow>
#include <QScrollArea>
#include <QIcon>
#include <QShortcut>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <functional>
#include <signal.h>

class Preview : public QWidget {
public:
    QPixmap picture;
    QRect crop;
    explicit Preview(QWidget *parent=nullptr) : QWidget(parent) { setMinimumHeight(165); }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(),QColor("#111b24"));
        if (!picture.isNull()) {
            QSize sz=picture.size(); sz.scale(size(),Qt::KeepAspectRatio);
            QRect target(QPoint((width()-sz.width())/2,(height()-sz.height())/2),sz);
            p.drawPixmap(target,picture);
            if (crop.isValid()) {
                p.setPen(QPen(QColor("#72dfcf"),2));
                p.drawRect(QRectF(target.x()+crop.x()*sz.width()/double(picture.width()),
                    target.y()+crop.y()*sz.height()/double(picture.height()),
                    crop.width()*sz.width()/double(picture.width()),crop.height()*sz.height()/double(picture.height())));
            }
            return;
        }
        p.setPen(Qt::NoPen); p.setBrush(QColor("#33434c"));
        for (int x=8;x<width();x+=14) for (int y=8;y<height();y+=14) p.drawEllipse(QPoint(x,y),1,1);
        p.translate(width()/2,height()/2);
        p.setPen(QPen(QColor("#73dfc7"),2));
        p.setBrush(QColor("#172c33"));
        p.drawRect(QRect(-55,-35,98,64));
        p.drawLine(-7,30,-7,37); p.drawLine(-27,38,13,38);
        p.setBrush(QColor("#22444a")); p.drawRect(QRect(17,3,43,31));
        p.setPen(Qt::NoPen); p.setBrush(QColor("#ff8069")); p.drawEllipse(QPoint(31,17),4,4);
    }
};

class Picker : public QWidget {
    QPixmap pix;
    QPoint origin, end;
    bool selecting=false;
    std::function<void(QRect)> accept;
    QRect bounds() const { return QRect(origin,end).normalized().intersected(rect()); }
public:
    Picker(QPixmap img,std::function<void(QRect)> fn) : pix(std::move(img)),accept(std::move(fn)) {
        setAttribute(Qt::WA_DeleteOnClose); setWindowFlags(Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint);
        setCursor(Qt::CrossCursor);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this); p.drawPixmap(rect(),pix);
        p.fillRect(rect(),QColor(5,12,18,115));
        if (selecting) {
            p.save(); p.setClipRect(bounds()); p.drawPixmap(rect(),pix); p.restore();
            p.setPen(QPen(QColor("#61ead2"),2)); p.drawRect(bounds().adjusted(0,0,-1,-1));
        }
        p.setPen(Qt::white); p.drawText(20,32,"Выделите область мышью. Esc для отмены");
    }
    void mousePressEvent(QMouseEvent *e) override { origin=end=e->position().toPoint(); selecting=true; update(); }
    void mouseMoveEvent(QMouseEvent *e) override { end=e->position().toPoint(); update(); }
    void mouseReleaseEvent(QMouseEvent *e) override {
        end=e->position().toPoint();
        QRect r=bounds();
        if (r.width()>2 && r.height()>2) {
            double sx=double(pix.width())/width(), sy=double(pix.height())/height();
            accept(QRect(int(r.x()*sx),int(r.y()*sy),int(r.width()*sx),int(r.height()*sy)));
        }
        close();
    }
    void keyPressEvent(QKeyEvent *e) override { if (e->key()==Qt::Key_Escape) close(); }
};

static QLabel *label(const QString &s, const char *role=nullptr) {
    auto *w=new QLabel(s); if (role) w->setObjectName(role); return w;
}
static QPushButton *button(const QString &s, const char *role=nullptr) {
    auto *b=new QPushButton(s); b->setCursor(Qt::PointingHandCursor); if (role) b->setObjectName(role); return b;
}
static QSpinBox *spin(int lo,int hi,int v,const QString &suffix={}) {
    auto *s=new QSpinBox; s->setRange(lo,hi); s->setValue(v); s->setSuffix(suffix); return s;
}
class Hud : public QWidget {
public:
    std::function<void()> restore;
    Hud() : QWidget(nullptr,Qt::Window|Qt::WindowStaysOnTopHint) {}
    void closeEvent(QCloseEvent *e) override { if (restore) restore(); QWidget::closeEvent(e); }
};
class Studio : public QWidget {
    QProcess worker, probe;
    QTemporaryDir temp;
    QByteArray pending;
    QComboBox *mode=new QComboBox, *audio=new QComboBox, *format=new QComboBox;
    QLineEdit *device=new QLineEdit("/dev/video0"), *title=new QLineEdit, *dest=new QLineEdit, *logo=new QLineEdit;
    QComboBox *rate=new QComboBox, *monitor=new QComboBox, *windowList=new QComboBox;
    QCheckBox *withCamera=new QCheckBox("Камера поверх видео"), *cursor=new QCheckBox("Показывать курсор");
    QSpinBox *duration=spin(0,86400,0," с"), *delay=spin(0,86400,0," с");
    QSpinBox *bitrate=spin(1,100,6," Мбит/с"), *pipSize=spin(5,100,25," %");
    QCheckBox *clock=new QCheckBox("Таймер в кадре"), *compact=new QCheckBox("Плавающая панель");
    QPointer<Ink> ink;
    QPushButton *record=button("●   Начать запись","record"), *pause=button("Пауза"), *snap=button("Снимок"),
        *grab=button("Обновить кадр"), *select=button("Выделить область"), *hudPause=button("Пауза");
    QLabel *state=label("Готово","state"), *timer=label("00:00:00","time"), *hudTime=label("00:00:00","time");
    QLabel *message=label(""), *sizeLabel=label("Весь экран","muted");
    Preview *preview=new Preview;
    QWidget *options=new QWidget, *hud=nullptr;
    QString saved, previewFile;
    bool savingShot=false, picking=false, paused=false, confirmed=false, closing=false;
    void sources() {
        QString oldMonitor=monitor->currentData().toString(), oldWindow=windowList->currentData().toString();
        QProcess list; list.start("/usr/libexec/recorder-grab",{"--list"});
        if (!list.waitForFinished(1800) || list.exitCode()) {
            list.kill(); list.waitForFinished(200); message->setText("Не удалось получить список источников"); return;
        }
        monitor->clear(); windowList->clear();
        for (const auto &line:list.readAllStandardOutput().split('\n')) {
            auto f=line.split('\t'); if (f.size()!=3) continue;
            auto *box=f[0]=="window"?windowList:monitor;
            box->addItem(QString::fromUtf8(f[2]),QString::fromUtf8(f[1]));
        }
        if (monitor->findData(oldMonitor)>=0) monitor->setCurrentIndex(monitor->findData(oldMonitor));
        if (windowList->findData(oldWindow)>=0) windowList->setCurrentIndex(windowList->findData(oldWindow));
    }
    QStringList target() const {
        if (mode->currentIndex()==2) return {"--camera",device->text()};
        QStringList args;
        if (mode->currentIndex()==3) args << "--window" << windowList->currentData().toString();
        else if (monitor->count()) args << "--monitor" << monitor->currentData().toString();
        if (!cursor->isChecked()) args << "--hide-cursor";
        return args;
    }
    QStringList arguments(bool photo,const QString &file) {
        QStringList a{photo?"shot":"record",file,"--screen","wayland"};
        a << target();
        if (mode->currentIndex()!=2 && withCamera->isChecked()) a << "--pip" << device->text() << "--pip-width" << QString::number(pipSize->value());
        if (mode->currentIndex()==1 && preview->crop.isValid()) {
            QRect r=preview->crop;
            a << "--region" << QString("%1,%2,%3,%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
        }
        if (clock->isChecked()) a << "--clock";
        if (!title->text().isEmpty()) {
            QFont font("DejaVu Sans Mono",14,QFont::DemiBold); QFontMetrics fm(font);
            QString text=fm.elidedText(title->text(),Qt::ElideRight,1400);
            QImage img(fm.horizontalAdvance(text)+20,fm.height()+10,QImage::Format_ARGB32);
            img.fill(QColor(10,17,21,175)); QPainter paint(&img);
            paint.setFont(font); paint.setPen(Qt::white); paint.drawText(img.rect(),Qt::AlignCenter,text); paint.end();
            QString path=temp.filePath("title.png");
            if (img.save(path)) a << "--banner" << path;
        }
        if (!logo->text().isEmpty()) a << "--logo" << logo->text();
        if (!photo) {
            a << "--controls" << "--fps" << rate->currentData().toString() << "--mbps" << QString::number(bitrate->value());
            if (duration->value()) a << "--seconds" << QString::number(duration->value());
            if (delay->value()) a << "--delay" << QString::number(delay->value());
            if (audio->currentIndex()) a << "--audio" << audio->currentData().toString();
        }
        return a;
    }
    void signalWorker(int sig) {
        if (confirmed && worker.state()==QProcess::Running && worker.processId()>0) kill(worker.processId(),sig);
    }
    void finish() {
        if (worker.state()==QProcess::Running && worker.processId()>0) worker.terminate();
    }
    void controls(bool busy) {
        options->setEnabled(!busy); pause->setEnabled(busy && confirmed && !savingShot);
        snap->setEnabled(!busy || (confirmed && !savingShot)); grab->setEnabled(!busy); select->setEnabled(!busy && mode->currentIndex()<=1);
        record->setText(busy?"■   Завершить":"●   Начать запись");
    }
    void begin(bool photo) {
        if (worker.state()!=QProcess::NotRunning) { if (photo) signalWorker(SIGUSR2); else finish(); return; }
        if (mode->currentIndex()==3 && windowList->currentData().toString().isEmpty()) { message->setText("Выберите открытое окно"); return; }
        if (mode->currentIndex()==1 && !preview->crop.isValid()) { refresh(true); return; }
        QDir dir(dest->text());
        if (!dir.exists() && !dir.mkpath(".")) { message->setText("Не удалось создать каталог"); return; }
        saved=dir.absoluteFilePath(QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz")+
            (photo?".png":"."+format->currentText().toLower()));
        confirmed=paused=false; pending.clear(); savingShot=photo;
        pause->setText("Пауза"); hudPause->setText("Пауза");
        timer->setText("00:00:00"); hudTime->setText(timer->text());
        state->setText("Подготовка"); message->clear(); controls(true);
        if (photo || compact->isChecked()) hide();
        if (!photo && compact->isChecked()) { hud->show(); hud->raise(); }
        QTimer::singleShot(180,this,[this,photo](){ worker.start("/bin/recorder",arguments(photo,saved)); });
    }
    void idle(bool ok, const QString &error) {
        confirmed=false;
        if (ink) ink->close();
        if (hud) hud->hide();
        controls(false); state->setText(ok?"Сохранено":"Ошибка");
        message->setText(ok ? QDir::toNativeSeparators(saved) : error.trimmed().left(400));
        if (closing) close(); else { show(); raise(); }
    }
    void refresh(bool pick) {
        if (probe.state()!=QProcess::NotRunning || worker.state()!=QProcess::NotRunning) return;
        picking=pick; previewFile=temp.filePath("screen.bmp"); QFile::remove(previewFile);
        hide();
        QTimer::singleShot(200,this,[this](){ probe.start("/bin/recorder",QStringList{"shot",previewFile,"--screen","wayland"}+target()); });
    }
public:
    Studio() {
        setWindowTitle("Synapse Recorder"); resize(840,610);
        auto *outer=new QVBoxLayout(this); outer->setContentsMargins(24,18,24,16); outer->setSpacing(10);
        auto *heading=new QHBoxLayout; heading->addWidget(label("Synapse Recorder","title")); heading->addStretch();
        heading->addWidget(state); heading->addSpacing(12); heading->addWidget(timer); outer->addLayout(heading);
        auto *columns=new QHBoxLayout(options); columns->setContentsMargins(0,0,0,0); columns->setSpacing(20);
        auto *left=new QVBoxLayout; left->addWidget(label("Источник","section")); mode->addItems(
            {"Весь экран","Область экрана","Камера","Отдельное окно"}); left->addWidget(mode);
        auto *sourceRow=new QHBoxLayout; sourceRow->addWidget(monitor,1); sourceRow->addWidget(windowList,1);
        auto *rescan=button("Обновить список"); sourceRow->addWidget(rescan); left->addLayout(sourceRow);
        connect(rescan,&QPushButton::clicked,this,[this]{sources();});
        windowList->hide(); cursor->setChecked(true);
        left->addWidget(preview,1); left->addWidget(sizeLabel);
        auto *tools=new QHBoxLayout; tools->addWidget(grab); tools->addWidget(select); left->addLayout(tools);
        left->addSpacing(3); left->addWidget(label("Наложения","section")); left->addWidget(title);
        title->setPlaceholderText("Подпись в кадре");
        auto *logoRow=new QHBoxLayout; logo->setPlaceholderText("Логотип PNG, JPEG или BMP"); logoRow->addWidget(logo);
        auto *pickLogo=button("Обзор"); logoRow->addWidget(pickLogo); left->addLayout(logoRow);
        left->addWidget(withCamera); left->addWidget(cursor); left->addWidget(clock); columns->addLayout(left,5);
        auto *right=new QVBoxLayout; right->addWidget(label("Параметры записи","section"));
        auto *form=new QFormLayout; form->setVerticalSpacing(8); form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        for (int hz:{10,15,24,25,30,50,60}) rate->addItem(QString::number(hz)+" FPS",hz);
        rate->setCurrentIndex(rate->findData(30)); format->addItems({"MP4","MKV","AVI"}); audio->addItem("Выключен"); audio->addItem("Системный звук","system");
        audio->addItem("Микрофон","mic"); audio->addItem("Система и микрофон","both");
        form->addRow("Контейнер",format); form->addRow("Частота",rate); form->addRow("Битрейт",bitrate);
        form->addRow("Звук",audio); form->addRow("Камера",device); form->addRow("Размер вставки",pipSize);
        duration->setSpecialValueText("Вручную"); delay->setSpecialValueText("Сразу");
        form->addRow("Остановить через",duration); form->addRow("Отложить на",delay); right->addLayout(form);
        right->addStretch(); right->addWidget(label("Каталог записей","section"));
        dest->setText(QDir::home().filePath("Videos")); auto *dirRow=new QHBoxLayout;
        auto *browse=button("Обзор"); dirRow->addWidget(dest); dirRow->addWidget(browse); right->addLayout(dirRow);
        compact->setChecked(true); right->addWidget(compact); columns->addLayout(right,4); auto *scroll=new QScrollArea; scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame); scroll->setWidget(options); outer->addWidget(scroll,1);
        message->setWordWrap(true); message->setObjectName("muted"); outer->addWidget(message);
        auto *bar=new QHBoxLayout; bar->addWidget(record); bar->addWidget(pause); bar->addStretch(); bar->addWidget(snap);
        outer->addLayout(bar); controls(false);
        auto *floating=new Hud; hud=floating;
        floating->restore=[this](){show();raise();};
        hud->setWindowTitle("Synapse Recorder HUD");
        auto *mini=new QHBoxLayout(hud); mini->addWidget(label("● REC","live")); mini->addWidget(hudTime);
        mini->addWidget(hudPause); auto *cam=button("Снимок"); mini->addWidget(cam);
        auto *pen=button("Перо"); mini->addWidget(pen);
        connect(pen,&QPushButton::clicked,this,[this](){
            if (ink) ink->close(); else { ink=new Ink; ink->showFullScreen(); }
        });
        auto *end=button("Стоп","record"); mini->addWidget(end);
        connect(end,&QPushButton::clicked,this,[this](){finish();});
        connect(hudPause,&QPushButton::clicked,this,[this](){signalWorker(SIGUSR1);});
        connect(cam,&QPushButton::clicked,this,[this](){signalWorker(SIGUSR2);});
        connect(pickLogo,&QPushButton::clicked,this,[this](){
            QString p=QFileDialog::getOpenFileName(this,"Логотип",{},"Image (*.png *.jpg *.jpeg *.bmp)");
            if (!p.isEmpty()) logo->setText(p);
        });
        connect(browse,&QPushButton::clicked,this,[this](){
            QString p=QFileDialog::getExistingDirectory(this,"Каталог записей",dest->text());
            if (!p.isEmpty()) dest->setText(p);
        });
        connect(grab,&QPushButton::clicked,this,[this](){refresh(false);});
        connect(select,&QPushButton::clicked,this,[this](){refresh(true);});
        connect(record,&QPushButton::clicked,this,[this](){begin(false);});
        connect(snap,&QPushButton::clicked,this,[this](){begin(true);});
        connect(pause,&QPushButton::clicked,this,[this](){signalWorker(SIGUSR1);});
        connect(mode,&QComboBox::currentIndexChanged,this,[this](int i){
            withCamera->setEnabled(i!=2); device->setEnabled(i==2 || withCamera->isChecked());
            pipSize->setEnabled(i!=2 && withCamera->isChecked()); select->setEnabled(i<=1);
            monitor->setVisible(i<=1); windowList->setVisible(i==3); cursor->setEnabled(i!=2);
            sizeLabel->setText(i==1 && preview->crop.isValid()?QString("%1 × %2").arg(preview->crop.width())
                .arg(preview->crop.height()):mode->currentText());
        });
        connect(withCamera,&QCheckBox::toggled,this,[this](bool checked){
            device->setEnabled(checked || mode->currentIndex()==2); pipSize->setEnabled(checked && mode->currentIndex()!=2);
        });
        connect(monitor,&QComboBox::currentIndexChanged,this,[this]{preview->crop={};preview->picture={};preview->update();});
        QTimer::singleShot(0,this,[this]{sources();});
        device->setEnabled(false); pipSize->setEnabled(false);
        connect(&probe,&QProcess::errorOccurred,this,[this](QProcess::ProcessError e){
            if (e==QProcess::FailedToStart) { show(); message->setText(probe.errorString()); }
        });
        connect(&probe,&QProcess::finished,this,[this](int rc,QProcess::ExitStatus){
            QPixmap p(previewFile);
            if (rc || p.isNull()) { show(); message->setText(probe.readAllStandardError()); return; }
            preview->picture=p; preview->update();
            if (!picking) { show(); return; }
            auto *picker=new Picker(p,[this](QRect r){
                preview->crop=r; mode->setCurrentIndex(1);
                sizeLabel->setText(QString("%1 × %2").arg(r.width()).arg(r.height())); preview->update();
            });
            connect(picker,&QObject::destroyed,this,[this](){show();}); picker->winId();
            for (auto *s:QGuiApplication::screens()) if (s->name()==monitor->currentData().toString()) picker->windowHandle()->setScreen(s);
            picker->showFullScreen();
        });
        connect(&worker,&QProcess::readyReadStandardOutput,this,[this](){
            pending+=worker.readAllStandardOutput();
            while (pending.contains('\n')) {
                int i=pending.indexOf('\n'); auto line=pending.left(i); pending.remove(0,i+1);
                if (line=="RECORDING") {
                    confirmed=true; controls(true); state->setText("Идёт запись");
                    if (compact->isChecked()) { hud->show(); hud->raise(); }
                } else if (line=="PAUSED" || line=="RESUMED") {
                    paused=line=="PAUSED"; state->setText(paused?"Пауза":"Идёт запись");
                    pause->setText(paused?"Продолжить":"Пауза"); hudPause->setText(pause->text());
                } else if (line.startsWith("TIME ")) {
                    int t=line.split(' ').value(1).toInt();
                    timer->setText(QString("%1:%2:%3").arg(t/3600,2,10,QChar('0'))
                        .arg(t/60%60,2,10,QChar('0')).arg(t%60,2,10,QChar('0'))); hudTime->setText(timer->text());
                }
            }
        });
        connect(&worker,&QProcess::finished,this,[this](int rc,QProcess::ExitStatus status){
            idle(!rc && status==QProcess::NormalExit,worker.readAllStandardError());
        });
        connect(&worker,&QProcess::errorOccurred,this,[this](QProcess::ProcessError e){
            if (e==QProcess::FailedToStart) idle(false,worker.errorString());
        });
        for (auto *target : {static_cast<QWidget *>(this),hud}) {
            auto *toggle=new QShortcut(QKeySequence("F9"),target);
            connect(toggle,&QShortcut::activated,this,[this](){begin(false);});
            auto *save=new QShortcut(QKeySequence("F10"),target);
            connect(save,&QShortcut::activated,this,[this](){begin(true);});
        }
    }
    ~Studio() override { if (ink) ink->close(); delete hud; }
    void closeEvent(QCloseEvent *e) override {
        if (worker.state()!=QProcess::NotRunning) { closing=true; finish(); e->ignore(); }
        else QWidget::closeEvent(e);
    }
};

int main(int argc,char **argv) {
    QApplication app(argc,argv);
    app.setApplicationName("synapse-recorder");
    app.setDesktopFileName("synapse-recorder");
    app.setWindowIcon(QIcon("/usr/share/icons/hicolor/128x128/apps/synapse-recorder.png"));
    app.setStyle("Fusion");
    app.setStyleSheet("QWidget{background:#12171c;color:#e3e9ec;font:14px 'DejaVu Sans Mono'}"
        "QLabel#title{font-size:27px;font-weight:700} QLabel#section{font-size:16px;font-weight:600;color:#b3e3d6}"
        "QLabel#state,QLabel#muted{color:#a5b5bf} QLabel#live{color:#ff8777;font-weight:700}"
        "QLabel#time{font:18px monospace;color:#8ee4c9}"
        "QLineEdit,QComboBox,QSpinBox{background:#202930;border:1px solid #39464d;border-radius:0;padding:5px}"
        "QPushButton{background:#28343a;border:1px solid #435258;border-radius:0;padding:7px 12px}"
        "QPushButton:hover{background:#374950} QPushButton#record{background:#fd896f;color:#191714;font-weight:700}"
        "QPushButton:disabled{color:#77858b;background:#1e272e} QCheckBox{spacing:8px}");
    Studio studio; studio.show();
    return app.exec();
}
