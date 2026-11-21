// SynapseOS, просмотр камеры и диктофон
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLabel>
#include <QIcon>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QtEndian>

class Media : public QWidget {
    bool camera, closing=false, wantRecord=false;
    QProcess capture, writer;
    QLabel *picture=new QLabel, *state=new QLabel;
    QPushButton *preview=new QPushButton("Включить камеру"), *save=new QPushButton("Снимок"),
        *record=new QPushButton("Начать запись");
    QComboBox *devices=new QComboBox;
    QImage latest;
    QString file, writerError;
    QByteArray incoming;
    QElapsedTimer elapsed;
    QTimer timer;

    QString output(QStandardPaths::StandardLocation where,const QString &extension) {
        QDir directory(QStandardPaths::writableLocation(where));
        if (!directory.mkpath(".")) return {};
        return directory.filePath(QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz")+extension);
    }
    void controls() {
        bool live=capture.state()!=QProcess::NotRunning, busy=writer.state()!=QProcess::NotRunning;
        preview->setText(live?"Выключить камеру":"Включить камеру");
        preview->setEnabled(!busy && !wantRecord && devices->count());
        save->setEnabled((live || busy) && !latest.isNull() && !wantRecord);
        devices->setEnabled(!live && !busy && !wantRecord);
        record->setEnabled(!wantRecord && (!camera || devices->count()>0));
        record->setText(busy?"Завершить запись":"Начать запись");
    }
    void startRecording() {
        wantRecord=false;
        file=output(camera?QStandardPaths::MoviesLocation:QStandardPaths::MusicLocation,camera?".mp4":".wav");
        if (file.isEmpty()) { state->setText("Не удалось создать каталог записи"); controls(); return; }
        state->setText("Подготовка записи"); writerError.clear(); incoming.clear();
        QStringList args=camera?QStringList{"record",file,"--camera",devices->currentText(),"--fps","30","--live-preview"}:
                                QStringList{"voice",file};
        writer.start("/bin/recorder",args);
        controls();
    }
    void readFrame(QProcess *source) {
        incoming+=source->readAllStandardOutput();
        if (incoming.size()<8) return;
        const auto *p=reinterpret_cast<const uchar *>(incoming.constData());
        uint w=qFromLittleEndian<quint32>(p),h=qFromLittleEndian<quint32>(p+4);
        if (!w || w>640 || !h || h>8192) { source->terminate(); return; }
        qsizetype size=8+qsizetype(w)*h*4;
        if (incoming.size()<size) return;
        if (source->state()==QProcess::Running && !wantRecord && !closing) source->write("F",1);
        latest=QImage(p+8,w,h,w*4,QImage::Format_RGB32).copy();
        picture->setPixmap(QPixmap::fromImage(latest).scaled(picture->size(),Qt::KeepAspectRatio,Qt::FastTransformation));
        incoming.remove(0,size); controls();
    }
public:
    explicit Media(bool isCamera):camera(isCamera) {
        const QString name=camera?"Камера":"Диктофон";
        setWindowTitle(name); resize(camera?600:390,camera?480:205);
        auto *layout=new QVBoxLayout(this); layout->setContentsMargins(20,18,20,18);
        if (camera) {
            auto *row=new QHBoxLayout;
            auto *refresh=new QPushButton("Обновить список");
            auto reload=[this] {
                if (capture.state()!=QProcess::NotRunning || writer.state()!=QProcess::NotRunning) return;
                QString old=devices->currentText(); devices->clear();
                for (const QString &s:QDir("/dev").entryList({"video[0-9]*"},QDir::System|QDir::Files,QDir::Name))
                    devices->addItem("/dev/"+s);
                if (devices->findText(old)>=0) devices->setCurrentText(old);
                state->setText(devices->count()?"Камера готова":"Камера не найдена"); controls();
            };
            row->addWidget(devices,1); row->addWidget(refresh); layout->addLayout(row);
            picture->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Ignored); picture->setMinimumSize(250,180); picture->setAlignment(Qt::AlignCenter);
            picture->setStyleSheet("background:#172027;color:white"); picture->setText("Просмотр выключен");
            layout->addWidget(picture,1);
            connect(refresh,&QPushButton::clicked,this,reload); reload();
            connect(preview,&QPushButton::clicked,this,[this] {
                if (capture.state()!=QProcess::NotRunning) { capture.closeWriteChannel(); return; }
                incoming.clear(); latest={}; capture.start("/bin/recorder",{"preview",devices->currentText()}); controls();
            });
            connect(&capture,&QProcess::started,this,[this]{capture.write("F",1);});
            connect(&capture,&QProcess::readyReadStandardOutput,this,[this]{readFrame(&capture);});
            connect(&capture,&QProcess::finished,this,[this](int rc,QProcess::ExitStatus){
                picture->clear(); picture->setText("Просмотр выключен"); latest={};
                if (rc) state->setText(QString::fromUtf8(capture.readAllStandardError()).trimmed());
                if (closing) close(); else if (wantRecord) startRecording();
                controls();
            });
            connect(save,&QPushButton::clicked,this,[this]{
                QString path=output(QStandardPaths::PicturesLocation,".png");
                state->setText(!latest.isNull() && !QFileInfo::exists(path) && latest.save(path)?path:"Не удалось сохранить снимок");
            });
        } else state->setText("Запись с микрофона в WAV · 48 кГц");
        state->setWordWrap(true); state->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(state);
        auto *buttons=new QHBoxLayout;
        if (camera) { buttons->addWidget(preview); buttons->addWidget(save); } else {
            for (QWidget *unused: {static_cast<QWidget *>(preview),static_cast<QWidget *>(save),
                                   static_cast<QWidget *>(devices),static_cast<QWidget *>(picture)}) {
                unused->setParent(this); unused->hide();
            }
        }
        buttons->addWidget(record); layout->addLayout(buttons);
        connect(record,&QPushButton::clicked,this,[this]{
            if (writer.state()!=QProcess::NotRunning) { writer.terminate();return; }
            if (capture.state()!=QProcess::NotRunning) { wantRecord=true; capture.closeWriteChannel();controls(); }
            else startRecording();
        });
        auto events=[this]{
            while (writer.canReadLine()) {
                QByteArray line=writer.readLine().trimmed();
                if (line=="RECORDING") {
                    elapsed.start(); timer.start(250); state->setText("Запись 00:00");
                } else if (!line.startsWith("TIME ") && !line.isEmpty()) writerError=QString::fromUtf8(line);
            }
        };
        if (camera) {
            writer.setReadChannel(QProcess::StandardError);
            connect(&writer,&QProcess::readyReadStandardError,this,events);
            connect(&writer,&QProcess::started,this,[this]{writer.write("F",1);});
            connect(&writer,&QProcess::readyReadStandardOutput,this,[this]{readFrame(&writer);});
        } else connect(&writer,&QProcess::readyReadStandardOutput,this,events);
        connect(&timer,&QTimer::timeout,this,[this]{
            qint64 s=elapsed.elapsed()/1000;
            state->setText(QString("● Запись   %1:%2").arg(s/60,2,10,QChar('0')).arg(s%60,2,10,QChar('0')));
        });
        connect(&writer,&QProcess::finished,this,[this](int rc,QProcess::ExitStatus how){
            timer.stop();
            QString err=QString::fromUtf8(writer.readAllStandardError()).trimmed();
            if (err.isEmpty()) err=writerError;
            if (camera) {latest={}; picture->setText("Просмотр выключен");}
            state->setText(!rc && how==QProcess::NormalExit ? file : err.isEmpty()?"Запись прервана":err);
            controls(); if (closing) close();
        });
        for (auto *proc:{&capture,&writer}) connect(proc,&QProcess::errorOccurred,this,[this,proc](QProcess::ProcessError e){
            if (e==QProcess::FailedToStart) { wantRecord=false;state->setText(proc->errorString());controls(); }
        });
        controls();
    }
    void closeEvent(QCloseEvent *event) override {
        closing=true; wantRecord=false;
        if (writer.state()!=QProcess::NotRunning) { writer.terminate();event->ignore(); }
        else if (capture.state()!=QProcess::NotRunning) { capture.closeWriteChannel();event->ignore(); }
        else event->accept();
    }
};

int main(int argc,char **argv) {
    QApplication app(argc,argv);
    bool camera=QFileInfo(app.applicationFilePath()).fileName().contains("camera") ||
        QFileInfo(QString::fromLocal8Bit(argv[0])).fileName().contains("camera");
    QString id=camera?"synapse-camera":"synapse-dictaphone";
    app.setDesktopFileName(id);
    app.setWindowIcon(QIcon("/usr/share/icons/hicolor/128x128/apps/"+id+".png"));
    Media main(camera); main.show(); return app.exec();
}
