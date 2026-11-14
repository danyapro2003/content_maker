#include "storyprotocol.h"
#include "storytimeline.h"
#include "storynarrative.h"
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>
#include <stdexcept>

static QByteArray read(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(("Cannot read " + path).toStdString());
    return file.readAll();
}
void replayV3Run(const QString& directory, const QString& transcript)
{
    const auto phrases = Stories3Runner::parsePacked(transcript);
    if (phrases.isEmpty()) throw std::runtime_error("Empty transcript");
    const auto run = QJsonDocument::fromJson(read(directory + "/run.json")).object();
    const auto events = QJsonDocument::fromJson(read(directory + "/event_map.json")).object()["events"].toArray();
    const auto plans = QJsonDocument::fromJson(read(directory + "/narrative_plans.json")).object()["branches"].toArray();
    StoryTimeline::Options options;
    options.minSec = run["min_story_sec"].toDouble(60); options.maxSec = run["max_story_sec"].toDouble(190);
    for (const auto& phrase : phrases) options.sourceDuration = qMax(options.sourceDuration, phrase.endSec);
    QJsonArray rows;
    const QRegularExpression edit("^\\d+_branch_(\\d+)_(?:edit|repair)(?:_attempt_\\d+)?\\.response\\.txt$");
    for (const auto& name : QDir(directory).entryList({"*.response.txt"}, QDir::Files, QDir::Name)) {
        const auto match = edit.match(name);
        if (!match.hasMatch()) continue;
        const int branch = match.captured(1).toInt();
        QJsonObject plan;
        for (const auto& value : plans) if (value.toObject()["branch_id"].toInt() == branch) plan = value.toObject();
        const auto decoded = StoryProtocol::parseEdit(QString::fromUtf8(read(directory + "/" + name)), phrases);
        QJsonObject row{{"response", name}, {"branch", branch}, {"parse_ok", decoded.ok()}, {"issues", decoded.errors()}};
        if (decoded.ok()) {
            const auto compiled = StoryTimeline::compile(decoded.value, phrases, events, plan, options);
            row["compile_ok"] = compiled.ok(); row["issues"] = compiled.errors();
            row["normalizations"] = compiled.value.normalizations;
            row["duration_sec"] = compiled.value.story.totalSec();
        }
        rows.append(row);
    }
    QTextStream(stdout) << QJsonDocument(QJsonObject{{"offline", true}, {"source_run", directory},
        {"padding_sec", options.paddingSec}, {"results", rows}}).toJson(QJsonDocument::Indented);
}
