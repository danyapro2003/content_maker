#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include "pipelinemanager.h"

class QLineEdit;
class QComboBox;
class QCheckBox;
class QSpinBox;
class QDoubleSpinBox;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void on_browse_file_pb_clicked();
    void on_browse_folder_pb_clicked();
    void on_start_pb_clicked();

private:
    void buildSettingsTab();
    void loadSettingsToUi();
    void saveSettingsFromUi();
    void appendLog(const QString& msg);
    void fillClipsTable(const QList<Highlight>& clips);
    void fillStoriesTable(const QList<StoryClip>& stories);

    Ui::MainWindow *ui;
    PipelineManager* Manager;

    // виджеты вкладки настроек
    QLineEdit* leFfprobe = nullptr;
    QLineEdit* leFfmpeg = nullptr;
    QLineEdit* leWhisperCli = nullptr;
    QLineEdit* leWhisperModel = nullptr;
    QLineEdit* leWhisperVadModel = nullptr;
    QLineEdit* leLlmBaseUrl = nullptr;
    QLineEdit* leLlmKey = nullptr;
    QLineEdit* leLlmModel = nullptr;
    QSpinBox* spLlmTimeout = nullptr;
    QComboBox* cbSubsForLlm = nullptr;
    QCheckBox* chkRenderRu = nullptr;
    QCheckBox* chkRenderEn = nullptr;
    QSpinBox* spMinClips = nullptr;
    QSpinBox* spMaxClips = nullptr;
    QSpinBox* spMinSec = nullptr;
    QSpinBox* spMaxSec = nullptr;
    QSpinBox* spChunkChars = nullptr;
    QLineEdit* leSubtitleStyle = nullptr;

    QDoubleSpinBox* spPadStart = nullptr;
    QDoubleSpinBox* spPadEnd = nullptr;
    QDoubleSpinBox* spMergeGap = nullptr;
    QCheckBox* chkDtw = nullptr;
    QCheckBox* chkSendFrames = nullptr;
    QDoubleSpinBox* spFrameInterval = nullptr;
    QSpinBox* spAgentTurns = nullptr;
    QSpinBox* spAgentMaxFrames = nullptr;
    QDoubleSpinBox* spCrossfade = nullptr;
    QSpinBox* spMinCuesPerHour = nullptr;

    QComboBox* cmbStoryTranscript = nullptr;
    QDoubleSpinBox* spEditPause = nullptr;
    QDoubleSpinBox* spEditMaxPhrase = nullptr;
    QSpinBox* spEditPadding = nullptr;
    QSpinBox* spEditFade = nullptr;
    QCheckBox* chkDiarization = nullptr;
    QLineEdit* lePythonPath = nullptr;
    QLineEdit* leHfToken = nullptr;
    QLineEdit* lePyannoteModel = nullptr;

    // ---- настройки «Пересказ 3.0» ----
    QComboBox* cbS3Asr = nullptr;
    QComboBox* cbS3AudioMap = nullptr;
    QComboBox* cbS3Diar = nullptr;
    QLineEdit* leS3Python = nullptr;
    QLineEdit* leS3ScriptsDir = nullptr;
    QLineEdit* leS3ModelDir = nullptr;
    QDoubleSpinBox* spS3Chunk = nullptr;
    QCheckBox* chkS3Review = nullptr;
    QComboBox* cbS3ReviewEngine = nullptr;
    QDoubleSpinBox* spS3ReviewChunk = nullptr;
    QDoubleSpinBox* spS3ReviewAgree = nullptr;
    QDoubleSpinBox* spS3DiarThreshold = nullptr;
    QSpinBox* spS3DiarThreads = nullptr;
    QCheckBox* chkS3DropMusicLowConf = nullptr;
    QDoubleSpinBox* spS3MinLogprob = nullptr;
    QCheckBox* chkS3DropMusicHard = nullptr;
    QCheckBox* chkS3QuoteCheck = nullptr;
    QSpinBox* spS3MinScore = nullptr;
    QSpinBox* spS3MinClip = nullptr;
    QSpinBox* spS3MaxClip = nullptr;
    QSpinBox* spS3MaxSpan = nullptr;
    QSpinBox* spS3MaxSegments = nullptr;
    QSpinBox* spS3MaxSegmentSec = nullptr;
    QSpinBox* spS3SubtitleFontSize = nullptr;
    QSpinBox* spS3SubtitleWords = nullptr;
};
#endif // MAINWINDOW_H
