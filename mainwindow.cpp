/* mainwindow.cpp */

#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "appsettings.h"
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QDirIterator>
#include <QSettings>
#include <QFormLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QHeaderView>
#include <QDateTime>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    Manager = new PipelineManager(this);
    connect(Manager, &PipelineManager::logMessage, this, &MainWindow::appendLog);
    connect(Manager, &PipelineManager::clipsFound, this, &MainWindow::fillClipsTable);
    connect(Manager, &PipelineManager::storiesFound, this, &MainWindow::fillStoriesTable);
    connect(Manager, &PipelineManager::queueFinished, this, [this]() {
        ui->start_pb->setEnabled(true);
    });

    // таблица клипов
    ui->clips_tw->setColumnCount(6);
    ui->clips_tw->setHorizontalHeaderLabels(
        {"#", "Название", "Начало", "Конец", "Длит.,с", "Почему вирально"});
    ui->clips_tw->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->clips_tw->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);

    AppSettings::instance().load();
    buildSettingsTab();
    loadSettingsToUi();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::appendLog(const QString& msg)
{
    ui->log_te->append(QString("[%1] %2")
                           .arg(QDateTime::currentDateTime().toString("HH:mm:ss"), msg));
}

static QString secToStr(double sec)
{
    int total = int(sec);
    return QString("%1:%2").arg(total / 60, 2, 10, QChar('0')).arg(total % 60, 2, 10, QChar('0'));
}

void MainWindow::fillClipsTable(const QList<Highlight>& clips)
{
    ui->clips_tw->setRowCount(clips.size());
    for (int i = 0; i < clips.size(); ++i) {
        const Highlight& h = clips[i];
        ui->clips_tw->setItem(i, 0, new QTableWidgetItem(QString::number(i + 1)));
        ui->clips_tw->setItem(i, 1, new QTableWidgetItem(h.title));
        ui->clips_tw->setItem(i, 2, new QTableWidgetItem(secToStr(h.startSec)));
        ui->clips_tw->setItem(i, 3, new QTableWidgetItem(secToStr(h.endSec)));
        ui->clips_tw->setItem(i, 4, new QTableWidgetItem(QString::number(h.endSec - h.startSec, 'f', 0)));
        ui->clips_tw->setItem(i, 5, new QTableWidgetItem(h.viralityReason));
    }
}

// ---------- выбор входных данных ----------

void MainWindow::on_browse_file_pb_clicked()
{
    QSettings settings("ContentCreator", "ContentMachine");
    QString lastPath = settings.value("LastVideoPath", QDir::homePath()).toString();

    QString filePath = QFileDialog::getOpenFileName(
        this, "Выбрать видеофайл", lastPath,
        "Видео (*.mkv *.mp4 *.m4v *.avi);;Все файлы (*.*)");

    if (!filePath.isEmpty()) {
        ui->browse_le->setText(filePath);
        settings.setValue("LastVideoPath", QFileInfo(filePath).absolutePath());
    }
}

void MainWindow::on_browse_folder_pb_clicked()
{
    QSettings settings("ContentCreator", "ContentMachine");
    QString lastPath = settings.value("LastVideoPath", QDir::homePath()).toString();

    QString dirPath = QFileDialog::getExistingDirectory(
        this, "Выбрать папку", lastPath,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (!dirPath.isEmpty()) {
        ui->browse_le->setText(dirPath);
        settings.setValue("LastVideoPath", dirPath);
    }
}

void MainWindow::on_start_pb_clicked()
{
    saveSettingsFromUi();

    const bool isStory = ui->mode_cb->currentIndex() >= 1;
    Manager->setMode(ui->mode_cb->currentIndex() == 1
                         ? PipelineManager::Mode::Stories
                         : isStory
                               ? PipelineManager::Mode::Stories3
                               : PipelineManager::Mode::Highlights);

    // таблица: у «Пересказа» другие колонки, чем у «Хайлайтов»
    if (isStory) {
        ui->clips_tw->setHorizontalHeaderLabels(
            {"#", "Название", "Склеек", "Длит.", "Описание", "Подпись"});
        ui->clips_tw->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
        ui->clips_tw->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    } else {
        ui->clips_tw->setHorizontalHeaderLabels(
            {"#", "Название", "Начало", "Конец", "Длит.,с", "Почему вирально"});
        ui->clips_tw->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    }
    ui->clips_tw->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);

    QString path = ui->browse_le->text().trimmed();
    QFileInfo fi(path);

    QStringList filesToProcess;
    if (fi.isFile()) {
        filesToProcess << fi.absoluteFilePath();
    } else if (fi.isDir()) {
        QStringList filters{"*.mkv", "*.mp4", "*.m4v", "*.avi"};
        QDirIterator it(path, filters, QDir::Files | QDir::NoSymLinks);
        while (it.hasNext())
            filesToProcess.append(it.next());
    }

    if (filesToProcess.isEmpty()) {
        appendLog("Не найдено видеофайлов по пути: " + path);
        return;
    }

    appendLog(QString("В очередь добавлено файлов: %1").arg(filesToProcess.size()));
    ui->start_pb->setEnabled(false);
    Manager->addFilesToQueue(filesToProcess);
}

void MainWindow::fillStoriesTable(const QList<StoryClip>& stories)
{
    ui->clips_tw->setRowCount(stories.size());
    for (int i = 0; i < stories.size(); ++i) {
        const StoryClip& s = stories[i];
        ui->clips_tw->setItem(i, 0, new QTableWidgetItem(QString::number(i + 1)));
        ui->clips_tw->setItem(i, 1, new QTableWidgetItem(s.title));
        ui->clips_tw->setItem(i, 2, new QTableWidgetItem(
            QString("склеек: %1").arg(s.segments.size())));
        ui->clips_tw->setItem(i, 3, new QTableWidgetItem(secToStr(s.totalSec())));
        ui->clips_tw->setItem(i, 4, new QTableWidgetItem(s.description));
        ui->clips_tw->setItem(i, 5, new QTableWidgetItem(s.caption));
    }
}

// ---------- вкладка настроек ----------
void MainWindow::buildSettingsTab()
{
    QFormLayout* form = new QFormLayout;

    auto pathRow = [this, form](const QString& label, QLineEdit*& le, bool isFile, const QString& filter = QString()) {
        le = new QLineEdit(this);
        QPushButton* btn = new QPushButton("...", this);
        btn->setFixedWidth(30);
        QHBoxLayout* row = new QHBoxLayout;
        row->addWidget(le);
        row->addWidget(btn);
        connect(btn, &QPushButton::clicked, this, [this, le, isFile, filter]() {
            QString p = isFile
                ? QFileDialog::getOpenFileName(this, "Выбрать файл", QString(), filter)
                : QFileDialog::getExistingDirectory(this, "Выбрать папку");
            if (!p.isEmpty())
                le->setText(p);
        });
        form->addRow(label, row);
    };

    pathRow("ffprobe:", leFfprobe, true, "ffprobe (ffprobe.exe);;Все файлы (*.*)");
    pathRow("ffmpeg:", leFfmpeg, true, "ffmpeg (ffmpeg.exe);;Все файлы (*.*)");
    pathRow("whisper-cli:", leWhisperCli, true, "whisper-cli (*.exe);;Все файлы (*.*)");
    pathRow("Модель whisper:", leWhisperModel, true, "ggml модель (*.bin);;Все файлы (*.*)");
    pathRow("VAD-модель (silero):", leWhisperVadModel, true, "ggml silero (*.bin);;Все файлы (*.*)");

    leLlmBaseUrl = new QLineEdit(this);
    form->addRow("LLM base URL:", leLlmBaseUrl);
    leLlmKey = new QLineEdit(this);
    leLlmKey->setEchoMode(QLineEdit::Password);
    form->addRow("LLM API key:", leLlmKey);
    leLlmModel = new QLineEdit(this);
    form->addRow("LLM модель:", leLlmModel);

    cbSubsForLlm = new QComboBox(this);
    cbSubsForLlm->addItem("Русские + Английские", "both");
    cbSubsForLlm->addItem("Только русские", "ru");
    cbSubsForLlm->addItem("Только английские", "en");
    form->addRow("Субтитры для LLM:", cbSubsForLlm);

    chkRenderRu = new QCheckBox("Рендерить русские версии", this);
    form->addRow(chkRenderRu);
    chkRenderEn = new QCheckBox("Рендерить английские версии", this);
    form->addRow(chkRenderEn);

    spMinClips = new QSpinBox(this); spMinClips->setRange(1, 20);
    form->addRow("Мин. клипов / историй V3:", spMinClips);
    spMaxClips = new QSpinBox(this); spMaxClips->setRange(1, 20);
    form->addRow("Макс. клипов / историй V3:", spMaxClips);
    spMinSec = new QSpinBox(this); spMinSec->setRange(5, 600); spMinSec->setSuffix(" сек");
    form->addRow("Мин. длина клипа:", spMinSec);
    spMaxSec = new QSpinBox(this); spMaxSec->setRange(10, 1200); spMaxSec->setSuffix(" сек");
    form->addRow("Макс. длина клипа:", spMaxSec);
    spChunkChars = new QSpinBox(this); spChunkChars->setRange(5000, 500000); spChunkChars->setSingleStep(5000);
    spChunkChars->setSuffix(" симв.");
    form->addRow("Размер чанка Скаута:", spChunkChars);

    leSubtitleStyle = new QLineEdit(this);
    leSubtitleStyle->setToolTip("ASS force_style. Пример: FontName=Segoe UI,FontSize=14,Bold=0,"
                                "PrimaryColour=&H00FFFFFF,Outline=0,Shadow=0,Alignment=2,MarginV=80");
    form->addRow("Стиль субтитров:", leSubtitleStyle);

    spPadStart = new QDoubleSpinBox(this); spPadStart->setRange(0.0, 2.0); spPadStart->setSingleStep(0.05); spPadStart->setSuffix(" с");
    form->addRow("Паддинг начала сегмента:", spPadStart);
    spPadEnd = new QDoubleSpinBox(this); spPadEnd->setRange(0.0, 2.0); spPadEnd->setSingleStep(0.05); spPadEnd->setSuffix(" с");
    form->addRow("Паддинг конца сегмента:", spPadEnd);
    spMergeGap = new QDoubleSpinBox(this); spMergeGap->setRange(0.0, 5.0); spMergeGap->setSingleStep(0.1); spMergeGap->setSuffix(" с");
    form->addRow("Слияние склеек ближе чем:", spMergeGap);

    chkDtw = new QCheckBox("Точное выравнивание сабов (whisper DTW)", this);
    chkDtw->setToolTip("Более точные word-таймкоды, чуть медленнее");
    form->addRow(chkDtw);
    chkSendFrames = new QCheckBox("LLM сама запрашивает кадры фрагментов (агент)", this);
    chkSendFrames->setToolTip("Вместо нарезки всего фильма модель читает транскрипт, намечает\n"
                              "кандидатов и просит показать кадры именно этих фрагментов.\n"
                              "Требует мультимодальной LLM (Kimi, GPT-4o, Qwen-VL и т.п.).");
    form->addRow(chkSendFrames);
    spFrameInterval = new QDoubleSpinBox(this); spFrameInterval->setRange(0.5, 10.0);
    spFrameInterval->setSingleStep(0.5); spFrameInterval->setSuffix(" с");
    spFrameInterval->setToolTip("Плотность кадров в запрошенном агентом фрагменте");
    form->addRow("Кадр каждые:", spFrameInterval);
    spAgentTurns = new QSpinBox(this); spAgentTurns->setRange(1, 12);
    form->addRow("Запросов кадров:", spAgentTurns);
    spAgentMaxFrames = new QSpinBox(this); spAgentMaxFrames->setRange(5, 200);
    spAgentMaxFrames->setToolTip("Лимит кадров на один запрос (больше — плотнее интервал)");
    form->addRow("Кадров на запрос:", spAgentMaxFrames);
    spCrossfade = new QDoubleSpinBox(this); spCrossfade->setRange(0.0, 1.0); spCrossfade->setSingleStep(0.05); spCrossfade->setSuffix(" с");
    spCrossfade->setToolTip("0 — без переходов");
    form->addRow("Кроссфейд склеек:", spCrossfade);

    spMinCuesPerHour = new QSpinBox(this); spMinCuesPerHour->setRange(0, 1000); spMinCuesPerHour->setSuffix(" /час");
    spMinCuesPerHour->setToolTip("Дорожка сабов с меньшей плотностью считается forced и отбраковывается.\n"
                                 "Полные сабы: 500–1500/час, forced: < 50/час. 0 — проверка выкл.");
    form->addRow("Мин. плотность сабов:", spMinCuesPerHour);

    cmbStoryTranscript = new QComboBox(this);
    cmbStoryTranscript->addItem("Whisper по всему фильму (точно, медленно)");
    cmbStoryTranscript->addItem("Субтитры из файла (быстро)");
    cmbStoryTranscript->setToolTip("Источник транскрипта для пересказа: whisper word-level даёт точные\n"
                                   "границы слов (кэшируется), сабы — точность до реплики.");
    form->addRow("Транскрипт пересказа:", cmbStoryTranscript);
    spEditPause = new QDoubleSpinBox(this); spEditPause->setRange(0.1, 2.0); spEditPause->setSingleStep(0.1); spEditPause->setSuffix(" с");
    form->addRow("Разрыв фразы от паузы:", spEditPause);
    spEditMaxPhrase = new QDoubleSpinBox(this); spEditMaxPhrase->setRange(4.0, 40.0);
    spEditMaxPhrase->setSingleStep(1.0); spEditMaxPhrase->setSuffix(" с");
    spEditMaxPhrase->setToolTip("В плотных диалогах пауз почти нет, поэтому фразы режутся ещё и по длине.\n"
                                "Рвём на границе предложения, как только фраза набрала 40% этой длины.");
    form->addRow("Макс. длина фразы:", spEditMaxPhrase);
    spEditPadding = new QSpinBox(this); spEditPadding->setRange(0, 500); spEditPadding->setSuffix(" мс");
    form->addRow("Паддинг склеек (whisper):", spEditPadding);
    spEditFade = new QSpinBox(this); spEditFade->setRange(0, 100); spEditFade->setSuffix(" мс");
    spEditFade->setToolTip("Аудио-фейды на границах склеек, убирают щелчки. 0 — выкл.");
    form->addRow("Фейд на склейках:", spEditFade);

    chkDiarization = new QCheckBox("Диаризация спикеров (pyannote, требует python+HF токен)", this);
    form->addRow(chkDiarization);
    lePythonPath = new QLineEdit(this);
    form->addRow("Python (venv):", lePythonPath);
    leHfToken = new QLineEdit(this); leHfToken->setEchoMode(QLineEdit::Password);
    form->addRow("Hugging Face токен:", leHfToken);
    lePyannoteModel = new QLineEdit(this);
    form->addRow("Модель pyannote:", lePyannoteModel);

    // ---------- Пересказ 3.0 ----------
    QGroupBox* s3Box = new QGroupBox("Пересказ 3.0 — сюжетная ветка", this);
    QFormLayout* s3 = new QFormLayout(s3Box);

    cbS3Asr = new QComboBox(this);
    cbS3Asr->addItem("GigaAM v3 CTC (быстро, GPU)", int(AppSettings::AsrEngine::GigaAmV3Ctc));
    cbS3Asr->addItem("GigaAM v3 e2e (точнее, медленно)", int(AppSettings::AsrEngine::GigaAmV3E2e));
    cbS3Asr->addItem("Parakeet v3 (быстро, GPU)", int(AppSettings::AsrEngine::ParakeetV3));
    cbS3Asr->addItem("whisper.cpp (CUDA)", int(AppSettings::AsrEngine::WhisperCpp));
    cbS3Asr->addItem("faster-whisper (CPU, медленно)", int(AppSettings::AsrEngine::FasterWhisper));
    s3->addRow("Движок ASR:", cbS3Asr);

    cbS3AudioMap = new QComboBox(this);
    cbS3AudioMap->addItem("inaSpeechSegmenter (точнее)", int(AppSettings::AudioMapEngine::InaSpeechSegmenter));
    cbS3AudioMap->addItem("zipformer audio-tagging", int(AppSettings::AudioMapEngine::Zipformer));
    cbS3AudioMap->addItem("выкл", int(AppSettings::AudioMapEngine::Off));
    s3->addRow("Карта музыки:", cbS3AudioMap);

    cbS3Diar = new QComboBox(this);
    cbS3Diar->addItem("sherpa-onnx (без токена)", int(AppSettings::DiarEngine::SherpaOnnx));
    cbS3Diar->addItem("pyannote (нужен HF токен)", int(AppSettings::DiarEngine::Pyannote));
    cbS3Diar->addItem("выкл", int(AppSettings::DiarEngine::Off));
    s3->addRow("Диаризация:", cbS3Diar);

    // локальный pathRow: строки v3 кладём в форму группы, а не в общую
    auto pathRowLocal = [this, s3](const QString& label, QLineEdit*& le, bool isFile,
                                   const QString& filter = QString()) {
        le = new QLineEdit(this);
        QPushButton* btn = new QPushButton("...", this);
        btn->setFixedWidth(30);
        QHBoxLayout* row = new QHBoxLayout;
        row->addWidget(le);
        row->addWidget(btn);
        connect(btn, &QPushButton::clicked, this, [this, le, isFile, filter]() {
            const QString p = isFile
                ? QFileDialog::getOpenFileName(this, "Выбрать файл", QString(), filter)
                : QFileDialog::getExistingDirectory(this, "Выбрать папку");
            if (!p.isEmpty())
                le->setText(p);
        });
        s3->addRow(label, row);
    };

    pathRowLocal("Python (.venv-asr):", leS3Python, true, "python (python.exe);;Все файлы (*.*)");
    pathRowLocal("Папка скриптов:", leS3ScriptsDir, false);
    pathRowLocal("Папка моделей:", leS3ModelDir, false);

    spS3Chunk = new QDoubleSpinBox(this);
    spS3Chunk->setRange(5.0, 25.0);
    spS3Chunk->setSingleStep(1.0);
    spS3Chunk->setSuffix(" с");
    spS3Chunk->setToolTip("Длина чанка ASR. Больше 25 с GigaAM не принимает (падает по форме входа).");
    s3->addRow("Чанк ASR:", spS3Chunk);

    chkS3Review = new QCheckBox("Ревизор: второй движок проверяет весь фильм", this);
    chkS3Review->setToolTip("Перекрёстная проверка транскрипта другим движком — ловит галлюцинации.");
    s3->addRow(chkS3Review);

    cbS3ReviewEngine = new QComboBox(this);
    cbS3ReviewEngine->addItem("Parakeet v3", int(AppSettings::AsrEngine::ParakeetV3));
    cbS3ReviewEngine->addItem("GigaAM v3 CTC", int(AppSettings::AsrEngine::GigaAmV3Ctc));
    cbS3ReviewEngine->addItem("whisper.cpp", int(AppSettings::AsrEngine::WhisperCpp));
    cbS3ReviewEngine->addItem("faster-whisper", int(AppSettings::AsrEngine::FasterWhisper));
    s3->addRow("Движок ревизора:", cbS3ReviewEngine);

    spS3ReviewChunk = new QDoubleSpinBox(this);
    spS3ReviewChunk->setRange(10.0, 120.0);
    spS3ReviewChunk->setSingleStep(5.0);
    spS3ReviewChunk->setSuffix(" с");
    s3->addRow("Чанк ревизора:", spS3ReviewChunk);

    spS3ReviewAgree = new QDoubleSpinBox(this);
    spS3ReviewAgree->setRange(0.0, 1.0);
    spS3ReviewAgree->setSingleStep(0.05);
    spS3ReviewAgree->setToolTip("Ниже этого согласия чанк считается подозрительным.");
    s3->addRow("Мин. согласие ревизора:", spS3ReviewAgree);

    spS3DiarThreshold = new QDoubleSpinBox(this);
    spS3DiarThreshold->setRange(0.1, 2.0);
    spS3DiarThreshold->setSingleStep(0.1);
    spS3DiarThreshold->setToolTip("Порог кластеризации sherpa-onnx. Работает наоборот ожиданиям:\n"
                                  "1.2 → ~3 спикера, 1.0 → ~14, 0.7 → ~33. Рабочее значение — 1.2.");
    s3->addRow("Порог диаризации:", spS3DiarThreshold);

    spS3DiarThreads = new QSpinBox(this);
    spS3DiarThreads->setRange(1, 32);
    spS3DiarThreads->setToolTip("Потоки CPU для диаризации. Главный рычаг скорости:\n"
                                "2 → ×3.9, 8 → ×6.2, 12 → ×6.4. GPU в sherpa не помогает.");
    s3->addRow("Потоки диаризации:", spS3DiarThreads);

    chkS3DropMusicLowConf = new QCheckBox("Выбрасывать неуверенные слова в музыке", this);
    chkS3DropMusicLowConf->setToolTip("music + logprob ниже порога → выбросить.\n"
                                      "music + нормальный logprob → оставить: под музыкой часто идёт диалог.");
    s3->addRow(chkS3DropMusicLowConf);

    spS3MinLogprob = new QDoubleSpinBox(this);
    spS3MinLogprob->setRange(-3.0, 0.0);
    spS3MinLogprob->setSingleStep(0.05);
    s3->addRow("Порог logprob:", spS3MinLogprob);

    chkS3DropMusicHard = new QCheckBox("Вырезать всю музыку (осторожно: режет диалог)", this);
    s3->addRow(chkS3DropMusicHard);

    chkS3QuoteCheck = new QCheckBox("Проверять цитаты по транскрипту", this);
    chkS3QuoteCheck->setToolTip("Ловит случай «LLM выдумала реплику» — такой цитаты в транскрипте не будет.");
    s3->addRow(chkS3QuoteCheck);

    spS3MinScore = new QSpinBox(this);
    spS3MinScore->setRange(0, 100);
    spS3MinScore->setToolTip("Клипы ниже порога рубрики отбрасываются. 0 — выкл.");
    s3->addRow("Мин. score:", spS3MinScore);

    spS3MinClip = new QSpinBox(this);
    spS3MinClip->setRange(5, 300);
    spS3MinClip->setSuffix(" с");
    s3->addRow("Мин. длина клипа:", spS3MinClip);

    spS3MaxClip = new QSpinBox(this);
    spS3MaxClip->setRange(10, 600);
    spS3MaxClip->setSuffix(" с");
    s3->addRow("Макс. длина клипа:", spS3MaxClip);

    spS3MaxSpan = new QSpinBox(this);
    spS3MaxSpan->setRange(0, 86400);
    spS3MaxSpan->setSpecialValueText("Весь фильм");
    spS3MaxSpan->setSuffix(" с");
    spS3MaxSpan->setToolTip("Охват исходного фильма между первым и последним фрагментом ветки.\n"
                            "0 — без ограничения. Длина готового пересказа задаётся отдельно.");
    s3->addRow("Охват исходного фильма:", spS3MaxSpan);

    spS3MaxSegments = new QSpinBox(this);
    spS3MaxSegments->setRange(1, 30);
    s3->addRow("Макс. склеек в клипе:", spS3MaxSegments);
    spS3MaxSegmentSec = new QSpinBox(this);
    spS3MaxSegmentSec->setRange(8, 90); spS3MaxSegmentSec->setSuffix(" с");
    s3->addRow("Ориентир длины блока V3:", spS3MaxSegmentSec);
    spS3SubtitleFontSize = new QSpinBox(this);
    spS3SubtitleFontSize->setRange(32, 90); spS3SubtitleFontSize->setSuffix(" px");
    s3->addRow("Размер субтитров V3:", spS3SubtitleFontSize);
    spS3SubtitleWords = new QSpinBox(this);
    spS3SubtitleWords->setRange(1, 6);
    s3->addRow("Слов в строке V3:", spS3SubtitleWords);

    QPushButton* saveBtn = new QPushButton("Сохранить настройки", this);
    connect(saveBtn, &QPushButton::clicked, this, [this]() {
        saveSettingsFromUi();
        appendLog("Настройки сохранены в " + AppSettings::instance().settingsFilePath());
    });
    form->addRow(saveBtn);

    // настроек много — общий контейнер с прокруткой
    QWidget* content = new QWidget(this);
    QVBoxLayout* outer = new QVBoxLayout(content);
    outer->addLayout(form);
    outer->addWidget(s3Box);
    outer->addStretch();

    QScrollArea* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);
    scroll->setFrameShape(QFrame::NoFrame);
    ui->settingsLayout->addWidget(scroll);
}

void MainWindow::loadSettingsToUi()
{
    const AppSettings& s = AppSettings::instance();
    leFfprobe->setText(s.ffprobePath);
    leFfmpeg->setText(s.ffmpegPath);
    leWhisperCli->setText(s.whisperCliPath);
    leWhisperModel->setText(s.whisperModelPath);
    leWhisperVadModel->setText(s.whisperVadModelPath);
    leLlmBaseUrl->setText(s.llmBaseUrl);
    leLlmKey->setText(s.llmApiKey);
    leLlmModel->setText(s.llmModel);

    QString subs = s.subsForLlm == AppSettings::SubsForLlm::RuOnly ? "ru"
                 : s.subsForLlm == AppSettings::SubsForLlm::EnOnly ? "en" : "both";
    cbSubsForLlm->setCurrentIndex(cbSubsForLlm->findData(subs));

    chkRenderRu->setChecked(s.renderRu);
    chkRenderEn->setChecked(s.renderEn);
    spMinClips->setValue(s.minClips);
    spMaxClips->setValue(s.maxClips);
    spMinSec->setValue(s.minClipSec);
    spMaxSec->setValue(s.maxClipSec);
    spChunkChars->setValue(s.scoutChunkChars);
    leSubtitleStyle->setText(s.subtitleStyle);
    spPadStart->setValue(s.padStartSec);
    spPadEnd->setValue(s.padEndSec);
    spMergeGap->setValue(s.mergeGapSec);
    chkDtw->setChecked(s.whisperDtw);
    chkSendFrames->setChecked(s.agentFramesEnabled);
    spFrameInterval->setValue(s.agentFrameIntervalSec);
    spAgentTurns->setValue(s.agentMaxTurns);
    spAgentMaxFrames->setValue(s.agentMaxFramesPerTurn);
    spCrossfade->setValue(s.storyCrossfadeSec);
    spMinCuesPerHour->setValue(s.minSubsCuesPerHour);
    cmbStoryTranscript->setCurrentIndex(s.storyWhisperTranscript ? 0 : 1);
    spEditPause->setValue(s.editPauseSec);
    spEditMaxPhrase->setValue(s.editMaxPhraseSec);
    spEditPadding->setValue(s.editPaddingMs);
    spEditFade->setValue(s.editAudioFadeMs);
    chkDiarization->setChecked(s.diarizationEnabled);
    lePythonPath->setText(s.pythonPath);
    leHfToken->setText(s.hfToken);
    lePyannoteModel->setText(s.pyannoteModel);

    // ---- Пересказ 3.0 ----
    cbS3Asr->setCurrentIndex(qMax(0, cbS3Asr->findData(int(s.s3AsrEngine))));
    cbS3AudioMap->setCurrentIndex(qMax(0, cbS3AudioMap->findData(int(s.s3AudioMapEngine))));
    cbS3Diar->setCurrentIndex(qMax(0, cbS3Diar->findData(int(s.s3DiarEngine))));
    leS3Python->setText(s.s3PythonPath);
    leS3ScriptsDir->setText(s.s3ScriptsDir);
    leS3ModelDir->setText(s.s3ModelDir);
    spS3Chunk->setValue(s.s3ChunkSec);
    chkS3Review->setChecked(s.s3ReviewEnabled);
    cbS3ReviewEngine->setCurrentIndex(qMax(0, cbS3ReviewEngine->findData(int(s.s3ReviewEngine))));
    spS3ReviewChunk->setValue(s.s3ReviewChunkSec);
    spS3ReviewAgree->setValue(s.s3ReviewMinAgree);
    spS3DiarThreshold->setValue(s.s3DiarThreshold);
    spS3DiarThreads->setValue(s.s3DiarThreads);
    chkS3DropMusicLowConf->setChecked(s.s3DropMusicLowConf);
    spS3MinLogprob->setValue(s.s3MinLogprob);
    chkS3DropMusicHard->setChecked(s.s3DropMusicHard);
    chkS3QuoteCheck->setChecked(s.s3QuoteCheck);
    spS3MinScore->setValue(int(s.s3MinScore));
    spS3MinClip->setValue(int(s.s3MinClipSec));
    spS3MaxClip->setValue(int(s.s3MaxClipSec));
    spS3MaxSpan->setValue(int(s.s3MaxSpanSec));
    spS3MaxSegments->setValue(s.s3MaxSegments);
    spS3MaxSegmentSec->setValue(int(s.s3MaxSegmentSec));
    spS3SubtitleFontSize->setValue(s.s3SubtitleFontSize);
    spS3SubtitleWords->setValue(s.s3SubtitleWords);
}

void MainWindow::saveSettingsFromUi()
{
    AppSettings& s = AppSettings::instance();
    s.ffprobePath = leFfprobe->text().trimmed();
    s.ffmpegPath = leFfmpeg->text().trimmed();
    s.whisperCliPath = leWhisperCli->text().trimmed();
    s.whisperModelPath = leWhisperModel->text().trimmed();
    s.whisperVadModelPath = leWhisperVadModel->text().trimmed();
    s.llmBaseUrl = leLlmBaseUrl->text().trimmed();
    s.llmApiKey = leLlmKey->text().trimmed();
    s.llmModel = leLlmModel->text().trimmed();

    QString subs = cbSubsForLlm->currentData().toString();
    s.subsForLlm = subs == "ru" ? AppSettings::SubsForLlm::RuOnly
                 : subs == "en" ? AppSettings::SubsForLlm::EnOnly
                                : AppSettings::SubsForLlm::Both;

    s.renderRu = chkRenderRu->isChecked();
    s.renderEn = chkRenderEn->isChecked();
    s.minClips = spMinClips->value();
    s.maxClips = spMaxClips->value();
    s.minClipSec = spMinSec->value();
    s.maxClipSec = spMaxSec->value();
    s.scoutChunkChars = spChunkChars->value();
    s.subtitleStyle = leSubtitleStyle->text().trimmed();
    s.padStartSec = spPadStart->value();
    s.padEndSec = spPadEnd->value();
    s.mergeGapSec = spMergeGap->value();
    s.whisperDtw = chkDtw->isChecked();
    s.agentFramesEnabled = chkSendFrames->isChecked();
    s.agentFrameIntervalSec = spFrameInterval->value();
    s.agentMaxTurns = spAgentTurns->value();
    s.agentMaxFramesPerTurn = spAgentMaxFrames->value();
    s.storyCrossfadeSec = spCrossfade->value();
    s.minSubsCuesPerHour = spMinCuesPerHour->value();
    s.storyWhisperTranscript = (cmbStoryTranscript->currentIndex() == 0);
    s.editPauseSec = spEditPause->value();
    s.editMaxPhraseSec = spEditMaxPhrase->value();
    s.editPaddingMs = spEditPadding->value();
    s.editAudioFadeMs = spEditFade->value();
    s.diarizationEnabled = chkDiarization->isChecked();
    s.pythonPath = lePythonPath->text().trimmed();
    s.hfToken = leHfToken->text().trimmed();
    s.pyannoteModel = lePyannoteModel->text().trimmed();

    // ---- Пересказ 3.0 ----
    s.s3AsrEngine = AppSettings::AsrEngine(
        cbS3Asr->currentData().isValid() ? cbS3Asr->currentData().toInt()
                                         : int(s.s3AsrEngine));
    s.s3AudioMapEngine = AppSettings::AudioMapEngine(
        cbS3AudioMap->currentData().isValid() ? cbS3AudioMap->currentData().toInt()
                                              : int(s.s3AudioMapEngine));
    s.s3DiarEngine = AppSettings::DiarEngine(
        cbS3Diar->currentData().isValid() ? cbS3Diar->currentData().toInt()
                                          : int(s.s3DiarEngine));
    s.s3PythonPath = leS3Python->text().trimmed();
    s.s3ScriptsDir = leS3ScriptsDir->text().trimmed();
    s.s3ModelDir = leS3ModelDir->text().trimmed();
    s.s3ChunkSec = spS3Chunk->value();
    s.s3ReviewEnabled = chkS3Review->isChecked();
    s.s3ReviewEngine = AppSettings::AsrEngine(
        cbS3ReviewEngine->currentData().isValid() ? cbS3ReviewEngine->currentData().toInt()
                                                  : int(s.s3ReviewEngine));
    s.s3ReviewChunkSec = spS3ReviewChunk->value();
    s.s3ReviewMinAgree = spS3ReviewAgree->value();
    s.s3DiarThreshold = spS3DiarThreshold->value();
    s.s3DiarThreads = spS3DiarThreads->value();
    s.s3DropMusicLowConf = chkS3DropMusicLowConf->isChecked();
    s.s3MinLogprob = spS3MinLogprob->value();
    s.s3DropMusicHard = chkS3DropMusicHard->isChecked();
    s.s3QuoteCheck = chkS3QuoteCheck->isChecked();
    s.s3MinScore = spS3MinScore->value();
    s.s3MinClipSec = spS3MinClip->value();
    s.s3MaxClipSec = spS3MaxClip->value();
    s.s3MaxSpanSec = spS3MaxSpan->value();
    s.s3MaxSegments = spS3MaxSegments->value();
    s.s3MaxSegmentSec = spS3MaxSegmentSec->value();
    s.s3SubtitleFontSize = spS3SubtitleFontSize->value();
    s.s3SubtitleWords = spS3SubtitleWords->value();
    s.save();
}
