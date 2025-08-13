#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "m3u8d.h"
#include <atomic>
#include <QFileDialog>
#include <QIntValidator>
#include <QMessageBox>
#include <QFileDialog>
#include <QTimer>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCloseEvent>
#include <QJsonArray>
#include "curldialog.h"

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    ui->lineEdit_SaveDir->setPlaceholderText(QString::fromStdString(GetWd()));

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, [this](){
        //更新ui1
        {
            GetStatus_Resp resp = GetStatus();
            ui->progressBar->setValue(resp.Percent);
            ui->label_progressBar->setText(QString::fromStdString(resp.Title));
            if(!resp.StatusBar.empty())
                ui->statusBar->showMessage(QString::fromStdString(resp.StatusBar), 5*1000);
            updateDownloadUi(resp.IsDownloading);
        }

        //更新ui2
        {
            auto resp = MergeGetProgressPercent();
            ui->progressBar_merge->setValue(resp.Percent);
            if(!resp.SpeedText.empty())
                ui->statusBar->showMessage(QString::fromStdString(resp.SpeedText), 5*1000);
            ui->label_mergeProgressBar->setText(QString::fromStdString(resp.Title));
        }
    });
    m_timer->start(200);  // 从50ms改为200ms，减少UI更新频率
    this->updateDownloadUi(false);
    this->updateMergeUi(false);

    m_saveConfigTimer = new QTimer(this);
    m_saveConfigTimer->start(3000);
    connect(m_saveConfigTimer, &QTimer::timeout, [this](){
        saveUiConfig(false);
    });

    loadUiConfig();
    setWindowTitle("m3u8d-" + QString::fromStdString(GetVersion()));
    ui->lineEdit_M3u8Url->setFocus();
}

MainWindow::~MainWindow()
{
    m_timer->stop();
    m_saveConfigTimer->stop();
    CloseOldEnv();
    saveUiConfig(true);
    delete ui;
}

void MainWindow::on_pushButton_RunDownload_clicked()
{
    if (ui->lineEdit_M3u8Url->isEnabled()==false) {
        return;
    }
    StartDownload_Req req;
    req.M3u8Url = ui->lineEdit_M3u8Url->text().toStdString();
    req.Insecure = ui->checkBox_Insecure->isChecked();
    req.SaveDir = ui->lineEdit_SaveDir->text().toStdString();
    req.TsTempDir = ui->lineEdit_TsTempDir->text().toStdString();
    req.FileName = ui->lineEdit_FileName->text().toStdString();
    req.SkipTsExpr = ui->lineEdit_SkipTsExpr->text().toStdString();
    req.SetProxy = ui->lineEdit_SetProxy->text().toStdString();
    req.HeaderMap = m_HeaderMap;
    req.SkipRemoveTs = ui->checkBox_SkipRemoveTs->isChecked();
    req.ThreadCount = ui->lineEdit_ThreadCount->text().toInt();
    req.SkipMergeTs = ui->checkBox_SkipMergeTs->isChecked();
    req.DebugLog = ui->checkBox_DebugLog->isChecked();
    req.WithSkipLog = ui->checkBox_WithSkipLog->isChecked();
    req.UseServerSideTime = ui->checkBox_UseServerSideTime->isChecked();
    req.ProgressBarShow = true;  // 启用进度条优化，提升下载性能

    std::string errMsg = StartDownload(req);
    if(!errMsg.empty()) {
        Toast::Instance()->SetError(QString::fromStdString(errMsg));
        return;
    }
    m_syncUi.AddRunFnOn_OtherThread([=](){
        GetStatus_Resp resp = WaitDownloadFinish();
        m_syncUi.AddRunFnOn_UiThread([=](){
            if (resp.IsCancel) {
                return;
            }

            if (!resp.ErrMsg.empty()) {
                QMessageBox::warning(this, "下载错误", QString::fromStdString(resp.ErrMsg));
                return;
            }
            if (resp.IsSkipped) {
                Toast::Instance()->SetSuccess("已经下载过了: " + QString::fromStdString(resp.SaveFileTo));
                return;
            }
            if (resp.SaveFileTo.empty()) {
                Toast::Instance()->SetSuccess("下载成功");
                return;
            }
            Toast::Instance()->SetSuccess("下载成功, 保存路径" + QString::fromStdString(resp.SaveFileTo));
        });
    });
}

void MainWindow::on_pushButton_SaveDir_clicked()
{
    QString dir = QFileDialog::getExistingDirectory(this);
    ui->lineEdit_SaveDir->setText(dir);
}

void MainWindow::on_pushButton_StopDownload_clicked()
{
    CloseOldEnv();
}

void MainWindow::on_pushButton_curlMode_clicked()
{
    StartDownload_Req req;
    req.M3u8Url = ui->lineEdit_M3u8Url->text().toStdString();
    req.Insecure = ui->checkBox_Insecure->isChecked();
    req.HeaderMap = m_HeaderMap;
    CurlDialog dlg(this);
    dlg.SetText(QString::fromStdString(RunDownload_Req_ToCurlStr(req)));
    dlg.show();
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    ParseCurl_Resp resp= dlg.Resp;
    ui->lineEdit_M3u8Url->setText(QString::fromStdString(resp.DownloadReq.M3u8Url));
    ui->checkBox_Insecure->setChecked(resp.DownloadReq.Insecure);
    this->m_HeaderMap = resp.DownloadReq.HeaderMap;
}

void MainWindow::on_pushButton_returnDownload_clicked()
{
    ui->stackedWidget->setCurrentIndex(0);
}

void MainWindow::on_pushButton_gotoMergeTs_clicked()
{
    ui->stackedWidget->setCurrentIndex(1);
}

void MainWindow::on_pushButton_startMerge_clicked()
{
    QString fileName = ui->lineEdit_mergeFileName->text();
    if(fileName.isEmpty())
        fileName = ui->lineEdit_mergeFileName->placeholderText();
    QString dir = ui->lineEdit_mergeDir->text();
    bool UseFirstTsMTime = ui->checkBox_UseFirstTsMTime->isChecked();
    bool SkipBadResolutionFps = ui->checkBox_SkipBadResolutionFps->isChecked();

    this->updateMergeUi(true);

    m_syncUi.AddRunFnOn_OtherThread([=](){
        auto resp = MergeTsDir(dir.toStdString(), fileName.toStdString(), UseFirstTsMTime, SkipBadResolutionFps);

        m_syncUi.AddRunFnOn_UiThread([=](){
            this->updateMergeUi(false);
            if(resp.ErrMsg.empty())
                Toast::Instance()->SetSuccess("合并成功!");
            else if(!resp.IsCancel)
                QMessageBox::warning(this, "合并错误", QString::fromStdString(resp.ErrMsg));
        });
    });
}

void MainWindow::on_pushButton_stopMerge_clicked()
{
    MergeStop();
}

void MainWindow::on_toolButton_selectMergeDir_clicked()
{
    QString dir = QFileDialog::getExistingDirectory(this);
    if(dir.isEmpty())
        return;
    ui->lineEdit_mergeDir->setText(dir);
}

void MainWindow::updateDownloadUi(bool runing)
{
    ui->lineEdit_M3u8Url->setEnabled(!runing);
    ui->lineEdit_SaveDir->setEnabled(!runing);
    ui->lineEdit_TsTempDir->setEnabled(!runing);
    ui->pushButton_SaveDir->setEnabled(!runing);
    ui->lineEdit_FileName->setEnabled(!runing);
    ui->lineEdit_SkipTsExpr->setEnabled(!runing);
    ui->pushButton_RunDownload->setEnabled(!runing);
    ui->checkBox_Insecure->setEnabled(!runing);
    if(runing == false)
        ui->pushButton_RunDownload->setText("开始下载");
    ui->lineEdit_SetProxy->setEnabled(!runing);
    ui->pushButton_StopDownload->setEnabled(runing);
    ui->pushButton_curlMode->setEnabled(!runing);
    ui->pushButton_TsTempDir->setEnabled(!runing);
    ui->checkBox_SkipRemoveTs->setEnabled(!runing);
    ui->lineEdit_ThreadCount->setEnabled(!runing);
    ui->checkBox_SkipMergeTs->setEnabled(!runing);
    ui->checkBox_DebugLog->setEnabled(!runing);
    ui->checkBox_WithSkipLog->setEnabled(!runing);
    ui->checkBox_UseServerSideTime->setEnabled(!runing);

    if(runing == false)
        ui->progressBar->setValue(0);

    ui->pushButton_gotoMergeTs->setEnabled(!runing);
}

void MainWindow::updateMergeUi(bool runing)
{
    ui->lineEdit_mergeDir->setEnabled(!runing);
    ui->toolButton_selectMergeDir->setEnabled(!runing);
    ui->pushButton_stopMerge->setEnabled(runing);
    ui->pushButton_startMerge->setEnabled(!runing);
    ui->lineEdit_mergeFileName->setEnabled(!runing);
    ui->pushButton_returnDownload->setEnabled(!runing);
    ui->checkBox_UseFirstTsMTime->setEnabled(!runing);
    ui->checkBox_SkipBadResolutionFps->setEnabled(!runing);
}

void MainWindow::saveUiConfig(bool force)
{
    QJsonObject obj;

    obj["M3u8Url"] = ui->lineEdit_M3u8Url->text();
    obj["SaveDir"] = ui->lineEdit_SaveDir->text();
    obj["TsTempDir"] = ui->lineEdit_TsTempDir->text();
    obj["FileName"]= ui->lineEdit_FileName->text();
    obj["SkipTsExpr"] = ui->lineEdit_SkipTsExpr->text();
    obj["SetProxy"] = ui->lineEdit_SetProxy->text();
    obj["ThreadCount"] = ui->lineEdit_ThreadCount->text().toInt();
    obj["Insecure"] = ui->checkBox_Insecure->isChecked();
    obj["SkipRemoveTs"] = ui->checkBox_SkipRemoveTs->isChecked();
    obj["SkipMergeTs"] = ui->checkBox_SkipMergeTs->isChecked();
    obj["DebugLog"] = ui->checkBox_DebugLog->isChecked();
    obj["WithSkipLog"] = ui->checkBox_WithSkipLog->isChecked();
    obj["UseServerSideTime"] = ui->checkBox_UseServerSideTime->isChecked();
    obj["UseFirstTsMTime"] = ui->checkBox_UseFirstTsMTime->isChecked();
    obj["SkipBadResolutionFps"] = ui->checkBox_SkipBadResolutionFps->isChecked();

    QJsonDocument doc;
    doc.setObject(obj);
    QByteArray data = doc.toJson();

    if ((force == false) && (data == m_lastSaveConfig)) {
//        qDebug() << "skip write";
        return;
    }

    QFile file(getConfigFilePath());
    if(!file.open(QFile::WriteOnly)) {
//        qDebug() << "write err1";
        return;
    }
    qint64 writeBytes = file.write(data);
    if(writeBytes != data.length()) {
//        qDebug() << "write err2";
        return;
    }
    file.close();

    m_lastSaveConfig = data;

//    qDebug() << "write ok";
}

void setupCheckbox(QJsonObject& obj,QCheckBox *box, QString key)
{
    if(obj[key].isBool()) {
        bool value = obj[key].toBool();
        box->setChecked(value);
    }
}

void MainWindow::loadUiConfig()
{
    QFile file(getConfigFilePath());
    if(!file.open(QFile::ReadOnly)) {
        return;
    }
    QByteArray data = file.readAll();
    file.close();

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if(!doc.isObject()) {
        return;
    }
    QJsonObject obj = doc.object();

    QString m3u8Url = obj["M3u8Url"].toString();
    if(!m3u8Url.isEmpty()) {
        ui->lineEdit_M3u8Url->setText(m3u8Url);
    }
    QString saveDir = obj["SaveDir"].toString();
    if(!saveDir.isEmpty()) {
        ui->lineEdit_SaveDir->setText(saveDir);
    }
    QString tsTempDir = obj["TsTempDir"].toString();
    if(!tsTempDir.isEmpty()) {
        ui->lineEdit_TsTempDir->setText(tsTempDir);
    }
    QString fileName = obj["FileName"].toString();
    if(!fileName.isEmpty()) {
        ui->lineEdit_FileName->setText(fileName);
    }
    QString skipTsExpr = obj["SkipTsExpr"].toString();
    if(!skipTsExpr.isEmpty()) {
        ui->lineEdit_SkipTsExpr->setText(skipTsExpr);
    }
    QString setProxy = obj["SetProxy"].toString();
    if(!setProxy.isEmpty()) {
        ui->lineEdit_SetProxy->setText(setProxy);
    }
    int threadCount = obj["ThreadCount"].toInt();
    if(threadCount > 0) {
        ui->lineEdit_ThreadCount->setText(QString::number(threadCount));
    }
    setupCheckbox(obj, ui->checkBox_Insecure,  "Insecure");
    setupCheckbox(obj, ui->checkBox_SkipRemoveTs, "SkipRemoveTs");
    setupCheckbox(obj, ui->checkBox_SkipMergeTs, "SkipMergeTs");
    setupCheckbox(obj, ui->checkBox_DebugLog, "DebugLog");
    setupCheckbox(obj, ui->checkBox_WithSkipLog, "WithSkipLog");
    setupCheckbox(obj, ui->checkBox_UseServerSideTime, "UseServerSideTime");
    setupCheckbox(obj, ui->checkBox_UseFirstTsMTime, "UseFirstTsMTime");
    setupCheckbox(obj, ui->checkBox_SkipBadResolutionFps, "SkipBadResolutionFps");
}

QString MainWindow::getConfigFilePath()
{
    QString dp =QApplication::applicationDirPath() + QDir::separator() + "m3u8d_config.json";
    return QDir::cleanPath(dp);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    do {
        GetStatus_Resp status = GetStatus();
        if(!status.IsDownloading) {
            break;
        }
        // 创建 QMessageBox 对象
        QMessageBox msgBox;
        // 设置标题和消息
        msgBox.setText("正在下载文件，是否关闭窗口？");
        msgBox.setWindowTitle("确认关闭窗口");

        // 添加按钮并设置默认按钮
        msgBox.addButton(QMessageBox::Yes);
        msgBox.addButton(QMessageBox::No);
        msgBox.setDefaultButton(QMessageBox::No);

        // 显示弹窗并根据用户选择执行相应操作
        int result = msgBox.exec();
        if (result != QMessageBox::Yes) {
            event->ignore();
            return;
        }
    } while(false);

    event->accept();
}

void MainWindow::on_lineEdit_M3u8Url_editingFinished()
{
    QString originUrl = ui->lineEdit_M3u8Url->text();
    QString urlStr = QString::fromStdString(FindUrlInStr(originUrl.toStdString()));
    if(urlStr.isEmpty()) {
        if(originUrl.isEmpty() == false)
        {
            Toast::Instance()->SetWaring("m3u8 url不合法");
        }
        return;
    }
    if(urlStr != originUrl) {
        ui->lineEdit_M3u8Url->setText(urlStr);
        Toast::Instance()->SetTips("自动识别修改了url");
        return;
    }
    QString fileName = QString::fromStdString(GetFileNameFromUrl(urlStr.toStdString()));
    if (fileName.isEmpty()) {
        return;
    }
    ui->lineEdit_FileName->setPlaceholderText(fileName);
}

void MainWindow::on_pushButton_TsTempDir_clicked()
{
    QString saveDir = ui->lineEdit_SaveDir->text();
    if(saveDir.isEmpty())
        saveDir = ui->lineEdit_SaveDir->placeholderText();

    QString dir = QFileDialog::getExistingDirectory(this, "", saveDir);
    ui->lineEdit_TsTempDir->setText(dir);
}

void MainWindow::on_pushButton_SetOutputMp4Name_clicked()
{
    QString mp4Name = QFileDialog::getSaveFileName(this, "", ui->lineEdit_mergeDir->text(), "(*.mp4)");
    if(mp4Name.isEmpty()) {
        return;
    }
    ui->lineEdit_mergeFileName->setText(mp4Name);
}
