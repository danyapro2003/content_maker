/* diarizerunner.cpp */

#include "diarizerunner.h"
#include "appsettings.h"
#include <QProcess>
#include <QFile>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

DiarizeRunner::DiarizeRunner() {}

void DiarizeRunner::diarize(const QString& wavPath, const QString& outJsonPath)
{
    const AppSettings& s = AppSettings::instance();
    m_outPath = outJsonPath;

    QString script = QCoreApplication::applicationDirPath() + "/scripts/diarize.py";
    if (!QFile::exists(script)) {
        emit errorOccurred("DiarizeRunner: не найден скрипт " + script);
        return;
    }

    QStringList args;
    args << script << wavPath
         << "--out" << outJsonPath
         << "--model" << s.pyannoteModel;
    if (!s.hfToken.isEmpty())
        args << "--hf-token" << s.hfToken;

    m_proc = new QProcess(this);
    m_proc->setProgram(s.pythonPath);
    m_proc->setArguments(args);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);

    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus) {
                QByteArray out = m_proc->readAll();
                m_proc->deleteLater();
                m_proc = nullptr;
                if (exitCode == 0 && QFile::exists(m_outPath)) {
                    emit diarizationDone(m_outPath);
                } else {
                    emit errorOccurred("Диаризация не удалась (код "
                                       + QString::number(exitCode) + "): "
                                       + QString::fromUtf8(out).right(500));
                }
            });

    m_proc->start();
    if (!m_proc->waitForStarted(10000)) {
        emit errorOccurred("DiarizeRunner: не удалось запустить python: " + s.pythonPath);
        m_proc->deleteLater();
        m_proc = nullptr;
    }
}

QList<DiarSegment> DiarizeRunner::parseJson(const QString& path)
{
    QList<DiarSegment> segs;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return segs;

    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray())
        return segs;

    for (const QJsonValue& v : doc.array()) {
        QJsonObject o = v.toObject();
        DiarSegment seg;
        seg.startSec = o.value("start").toDouble();
        seg.endSec = o.value("end").toDouble();
        seg.speaker = o.value("speaker").toString();
        if (seg.endSec > seg.startSec)
            segs.append(seg);
    }
    return segs;
}
