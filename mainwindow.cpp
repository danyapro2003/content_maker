/* mainwindow.cpp*/

#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QDirIterator>
#include <QSettings>
//#include "mediaextractor.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    Manager = new PipelineManager(this);
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::on_browse_pb_clicked() {
    QSettings settings("ContentCreator", "ContentMachine");

    QString lastPath = settings.value("LastVideoPath", QDir::homePath()).toString();

    // QString filePath = QFileDialog::getOpenFileName(
    //     this,
    //     "Choose file",
    //     lastPath,
    //     "Video files (*.mkv *.mp4 *.avi);;All files(*.*)"
    //     );

    QString filePath = QFileDialog::getExistingDirectory(
        this,
        "Choose folder",
        lastPath,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
        );

    if (!filePath.isEmpty()) {
        ui->browse_le->setText(filePath);

        QFileInfo fileInfo(filePath);
        QString folderPath = fileInfo.absolutePath();

        settings.setValue("LastVideoPath", folderPath);
    }
}
void MainWindow::on_getmeta_pb_clicked()
{
    QString dirPath = ui->browse_le->text();
    QDir dir(dirPath);

    if (!dir.exists()) {
        qWarning() << "Директория не найдена:" << dirPath;
        return;
    }

    // Фильтруем только видеоформаты
    QStringList filters;
    filters << "*.mkv" << "*.mp4" << "*.m4v" << "*.avi";

    QDirIterator it(dirPath, filters, QDir::Files | QDir::NoSymLinks);
    QStringList filesToProcess;

    while (it.hasNext()) {
        filesToProcess.append(it.next());
    }

    if (!filesToProcess.isEmpty()) {
        Manager->addFilesToQueue(filesToProcess);
    } else {
        qDebug() << "В выбранной папке нет подходящих видеофайлов.";
    }
}

