/* mediaextractor.cpp*/

#include "mediaextractor.h"
#include <QProcess>
#include <QDebug>


MediaExtractor::MediaExtractor() {}


void MediaExtractor::runFFprobe(QString path) {
    currentFilePath = path;

    QProcess *proc = new QProcess(this);
    QString program = "ffprobe";
    QStringList args; //Список аргуменотов для ffproba

    //извлечение данных о фильме: ffprobe -v quiet -print_format json -show_chapters -show_format -show_streams "film_name.mkv" > film_name_metadata.json
    args << "-v" << "error"
         << "-print_format" << "json"
         << "-show_streams"
         << "-show_chapters"
         << "-show_format"
         << currentFilePath;

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [=](int exitCode, QProcess::ExitStatus exitStatus) {

                if (exitStatus == QProcess::NormalExit && exitCode == 0) {
                    QByteArray output = proc->readAllStandardOutput();
                    emit ffprobeFinished(output); // тут у меня тех. долг так как эта функция вызывает только определенный сигнал, по хорошему переделать если будет несколько пайплайнов
                } else {
                    emit errorOccurred("Ошибка ffprobe: " + proc->readAllStandardError());
                }

                proc->deleteLater();
            });
    proc->start(program, args);
}