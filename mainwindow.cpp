#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QDir>
#include <QFileDialog>
#include <QMessageBox>
#include <QByteArray>
#include <QDebug>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    lastSelectedDir = QDir::homePath() + "/Desktop";
}

MainWindow::~MainWindow()
{
    delete ui;
}

#define RIFF 0x46464952
#define WAVE 0x45564157
#define NWAV 0x5641574E
#define smpl 0x6C706D73

#define STRM_BUF_PAGESIZE (64 * 32)

void MainWindow::printClrCon()
{
    ui->console->clear();
    conv_warning_fired = false;
    qApp->processEvents();
}

void MainWindow::printCon(const QString& string)
{
    ui->console->append(string);
    qApp->processEvents();
}

void MainWindow::setEditMode(int mode)
{
    ui->sampleRate_sb->setEnabled(mode == 2);
    ui->loopStart_sb->setEnabled(mode != 0);
    ui->loopEnd_sb->setEnabled(mode != 0);
    ui->pcm8_rb->setEnabled(mode == 2);
    ui->pcm16_rb->setEnabled(mode == 2);
    ui->stereo_cb->setEnabled(mode == 2);
    ui->changeEvents_pb->setEnabled(mode != 0);

    if(mode == 0)
    {
        ui->sampleRate_sb->setValue(32728);
        ui->loopStart_sb->setValue(0);
        ui->loopEnd_sb->setValue(0);
        ui->pcm8_rb->setChecked(true);
        ui->pcm16_rb->setChecked(false);
        ui->stereo_cb->setChecked(false);
    }
}

QString MainWindow::ReadWAVTag(WAVStruct* wav, int fileSize, const QString& tagName)
{
    for(int i = wav->Subchunk2Size; i < fileSize; i++)
    {
        if(QString(&wav->Data[i]) == tagName)
        {
            return QString(&wav->Data[i + tagName.length() + 1]).remove("TXXX");
        }
    }

    return "";
}

//Gets the loop points from SMPL block
bool MainWindow::GetLoopSamples(WAVStruct* wav, int fileSize, int& loopStart, int& loopEnd)
{
    for(int i = wav->Subchunk2Size; i < fileSize; i++)
    {
        int* data = reinterpret_cast<int*>(&wav->Data[i]);
        if(data[0] == smpl)
        {
            loopStart = data[13];
            loopEnd = data[14];
            return true;
        }
    }
    return false;
}

//Gets the loop points from WAV ID3 markers
bool MainWindow::GetLoopMarkers(WAVStruct* wav, int fileSize, int& loopStart, int& loopEnd)
{
    bool ok;

    QString aloopStart = ReadWAVTag(wav, fileSize, "loopStart");
    loopStart = aloopStart.toInt(&ok);

    if(!ok)
    {
        printCon("Warning: loopStart tag is invalid.");
        printCon("tagError1: Invalid number: " + aloopStart);
        return false;
    }

    QString aloopEnd = ReadWAVTag(wav, fileSize, "loopEnd");
    loopEnd = aloopEnd.toInt(&ok);

    if(loopEnd == 0 || !ok)
    {
        printCon("Warning: loopEnd tag is invalid.");
        if(!ok)
            printCon("tagError1: Invalid number: " + aloopEnd);
        if(loopEnd == 0)
            printCon("tagError2: loopEnd can't be 0.");
        return false;
    }

    return true;
}

//Returns if RAW file can be read.
bool MainWindow::ReadWAV(QFile& file)
{
    file.seek(0);

    QByteArray input_file = file.readAll();
    WAVStruct* wav = reinterpret_cast<WAVStruct*>(input_file.data());

    printCon("Info: Checking for RIFF and WAVE header...");
    if(wav->ChunkID == RIFF &&
       wav->Format == WAVE)
    {
        printCon("Info: Checking if PCM format...");
        if(wav->NumChannels > 2)
        {
            QMessageBox::critical(this, "Error.", "WAV file is not PCM format.\nCheck your exporting options before trying again.");
            printCon("Error: WAV file is not PCM format. Check your exporting options before trying again.");
            return false;
        }

        bool plural = wav->NumChannels > 1;
        QString channel = plural ? " channels." : "channel.";

        printCon("Info: Found " + QString::number(wav->NumChannels) + channel);
        if(wav->NumChannels > 2)
        {
            QMessageBox::critical(this, "Error.", "WAV file has more than 2 channels.\nMake sure it only has 1 or 2 before trying again.");
            printCon("Error: WAV file has more than 2 channels. Make sure it only has 1 or 2 before trying again.");
            return false;
        }

        printCon("Info: Found " + QString::number(wav->SampleRate) + " sample rate.");
        if(wav->SampleRate > 32728)
        {
            QMessageBox::critical(this, "Error.", "WAV file exceeds maximum sample rate 32728.\nLower it before trying again.");
            printCon("Error: WAV file exceeds maximum sample rate 32728. Lower it before trying again.");
            return false;
        }

        printCon("Info: Found " + QString::number(wav->BitsPerSample) + " bits per sample.");
        if(wav->BitsPerSample != 8 && wav->BitsPerSample != 16)
        {
            QMessageBox::critical(this, "Error.", "The audio must be either PCM-8 or PCM-16.\nCheck your exporting options before trying again.");
            printCon("Error: The audio must be either PCM-8 or PCM-16. Check your exporting options before trying again.");
            return false;
        }

        ui->sampleRate_sb->setValue(wav->SampleRate);
        ui->stereo_cb->setChecked(wav->NumChannels == 2);

        ui->pcm8_rb->setChecked(wav->BitsPerSample == 8);
        ui->pcm16_rb->setChecked(wav->BitsPerSample == 16);

        int tagStart = wav->Subchunk2Size + static_cast<int>(sizeof(WAVStruct));
        int fileSize = input_file.size();
        if(fileSize > tagStart)
        {
            printCon("Info: Searching for loopStart and loopEnd...");
            int loopStart[3];
            int loopEnd[3];

            bool gotSamples = GetLoopSamples(wav, fileSize, loopStart[1], loopEnd[1]);
            bool gotMarkers = GetLoopMarkers(wav, fileSize, loopStart[2], loopEnd[2]);
            bool useMarkers = false;
            if(gotMarkers || gotSamples)
            {
                if(gotMarkers && !gotSamples)
                {
                    useMarkers = true;
                }
                else
                {
                    QMessageBox msgBox;
                    msgBox.setIcon(QMessageBox::Information);
                    msgBox.setWindowTitle("Conflicting loop data.");
                    msgBox.setText("Both loop points and loop markers were found, which one do you want to use?");
                    msgBox.addButton("Loop Points", QMessageBox::YesRole);
                    QAbstractButton* pButtonMarkers = msgBox.addButton("Loop Markers", QMessageBox::NoRole);
                    msgBox.exec();

                    if (msgBox.clickedButton() == pButtonMarkers)
                        useMarkers = true;
                }
            }
            else
            {
                goto loop_search_fail;
            }

            loopStart[0] = loopStart[1 + useMarkers];
            loopEnd[0] = loopEnd[1 + useMarkers];

            if(loopStart[0] > loopEnd[0])
            {
                QMessageBox::critical(this, "Error.", "loopStart can't be bigger than loopEnd, please try again after changing the tags.");
                printCon("Error: loopStart can't be bigger than loopEnd.");
                goto loop_search_fail;
            }

            printCon("Info: Found loopStart with value: " + QString::number(loopStart[0]) + ".");
            printCon("Info: Found loopEnd with value: " + QString::number(loopEnd[0]) + ".");

            ui->loopStart_sb->setValue(loopStart[0]);
            ui->loopEnd_sb->setValue(loopEnd[0]);
            goto loop_seach_succeed;

            loop_search_fail:
            printCon("Info: No valid loop data was found.");

            loop_seach_succeed:
            printCon("Info: Searching for events...");
            for(int i = wav->Subchunk2Size; i < fileSize; i++)
            {
                QString tagName(&wav->Data[i]);

                if(tagName.startsWith("event"))
                {
                    QString trimmed = tagName;
                    trimmed = trimmed.remove("event");
                    QStringList list = trimmed.split('_');
                    QString eventID = list[1];
                    QString sample = QString(&wav->Data[i + tagName.length() + 1]).remove("TXXX");

                    bool ok[2];
                    int eventIDVal = eventID.toInt(&ok[0]);
                    int sampleVal = sample.toInt(&ok[1]);

                    if(!ok[0] || !ok[1])
                    {
                        printCon("Warning: Invalid event named: " + tagName + ".");
                    }
                    else
                    {
                        printCon("Info: Found " + tagName + " with value: " + sample + ".");
                        eventEditor.append(eventIDVal, sampleVal);
                    }
                }
            }
        }

        printCon("Info: Loading WAV raw data...");
        raw_data.resize(wav->Subchunk2Size);
        memcpy(raw_data.data(), wav->Data, wav->Subchunk2Size);

        printCon("Info: Converting unsigned PCM-8 to signed PCM-8...");
        if(wav->BitsPerSample == 8)
            for(int i = 0; i < raw_data.size(); i++)
                raw_data.data()[i] -= 0x80;

        printCon("Info: Done and ready for conversion!");

        setEditMode(1);

        return false;
    }
    printCon("Info: Header not found, proceeding to RAW verification...");

    return true;
}

void MainWindow::ReadRAW(QFile& file, QFileInfo& fInfo)
{
    file.seek(0);

    printCon("Info: Checking RAW extension...");
    if(fInfo.suffix().toLower() != "raw")
    {
        QMessageBox::StandardButton reply =
        QMessageBox::warning(this, "RAW File.",
                             "The detected file is not a WAV and it is missing the RAW file extension. Do you still want to import it?",
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

        if (reply == QMessageBox::No)
        {
            printCon("Info: Operation canceled by the user.");
            return;
        }
    }
    printCon("Info: Loading raw data...");
    raw_data = file.readAll();
    printCon("Warning: Don't forget to adjust the settings!");
    printCon("Info: Done and ready for conversion!");
    setEditMode(2);
}

void MainWindow::on_search_pb_clicked()
{
    QString fileName = QFileDialog::getOpenFileName(this, "Open sound file", lastSelectedDir, "Sound Files (*.raw *.wav)");
    if(fileName == "")
        return;
    lastSelectedDir = fileName;
    ui->wavPath_le->setText(fileName);
    ui->wavPath_le->editingFinished();
}

void MainWindow::on_wavPath_le_editingFinished()
{
    static QString lastPath;
    QString newPath = ui->wavPath_le->text();
    if(lastPath == newPath)
        return;
    lastPath = newPath;

    printClrCon();
    raw_data.clear();
    eventEditor.clear();
    setEditMode(0);

    if(newPath == "")
        return;

    QFileInfo fInfo(newPath);
    if(!fInfo.exists())
    {
        QMessageBox::warning(this, "Invalid path.", "The path you introduced is invalid, please try again.");
        printCon("Error: The path you introduced is invalid, please try again.");
        return;
    }

    QFile file(newPath);
    if(!file.open(QIODevice::ReadOnly))
    {
        QMessageBox::critical(this, "Error.", "Could not open file for reading, please try again.");
        printCon("Error: Could not open file for reading, please try again.");
        return;
    }

    fileSuffix = "." + fInfo.suffix();

    if(ReadWAV(file))
        ReadRAW(file, fInfo);

    file.close();
}

QByteArray MainWindow::processRawData()
{
    if(ui->stereo_cb->isChecked())
    {
        int dataSize = raw_data.size();
        int chSize = dataSize / 2;

        QByteArray buffer;
        QByteArray left;
        QByteArray right;
        buffer.resize(dataSize);
        left.resize(chSize);
        right.resize(chSize);

        //Desinterleave stereo
        printCon("Info: Desinterleaving stereo...");
        if(ui->pcm8_rb->isChecked())
        {
            for(int i = 0, j = 0; i < chSize; i++)
            {
                left[i] = raw_data[j++];
                right[i] = raw_data[j++];
            }
        }
        else
        {
            short* _left = reinterpret_cast<short*>(left.data());
            short* _right = reinterpret_cast<short*>(right.data());
            short* _raw_data = reinterpret_cast<short*>(raw_data.data());
            for(int i = 0, j = 0; i < chSize / 2; i++)
            {
                _left[i] = _raw_data[j++];
                _right[i] = _raw_data[j++];
            }
        }

        //Interleave with page size
        printCon("Info: Interleaving stereo with page size 0x" + QString::number(STRM_BUF_PAGESIZE, 16) + " interval per channel...");
        char* pBuffer = buffer.data();
        char* pChannels[2] = { left.data(), right.data() };

        for(int bCursor = 0, cCursor = 0; cCursor < chSize;)
        {
            int len = STRM_BUF_PAGESIZE;
            int remain = chSize - cCursor;
            if(remain < len)
                len = remain;

            for(int c = 0; c < 2; c++)
            {
                memcpy(&pBuffer[bCursor], &pChannels[c][cCursor], len);
                bCursor += len;
            }
            cCursor += len;
        }

        printCon("Info: Stereo conversion finished.");
        return buffer;
    }
    else
    {
        return raw_data;
    }
}

QByteArray MainWindow::nwavStructToBin(NWAVStruct& nwav)
{
    QByteArray result;
    result.resize(nwav.header.fileSize);
    char* dest = result.data();

    memcpy(dest,
           &nwav.header, sizeof(NWAVHeader));
    dest += sizeof(NWAVHeader);

    if(nwav.header.numEvents)
    {
        int* lDest = reinterpret_cast<int*>(dest + eventIDBlockSize);
        for(int i = 0; i < nwav.header.numEvents; i++)
        {
            dest[i] = nwav.events[i].eventID;
            lDest[i] = nwav.events[i].sample;
        }
        dest += eventBlockSize;
    }

    memcpy(dest,
           nwav.data.data(), nwav.data.size());

    return result;
}

void MainWindow::on_convert_pb_clicked()
{
    if(raw_data.size() == 0)
    {
        QMessageBox::warning(this, "Warning.", "Input file is missing, please try again after selecting one.");
        if(!conv_warning_fired)
        {
            printCon("Warning: Input file is missing, please try again after selecting one.");
            conv_warning_fired = true;
        }
        return;
    }

    int loopStart = ui->loopStart_sb->value();
    int loopEnd = ui->loopEnd_sb->value();

    if(loopStart != 0 || loopEnd != 0)
    {
        int diff = loopEnd - loopStart;
        if(diff < 0)
        {
            printCon("Error: loopStart is bigger than loopEnd.");
            return;
        }
        else if(diff < STRM_BUF_PAGESIZE)
        {
            printCon("Error: Diference between loopStart and loopEnd must be at least: " + QString::number(STRM_BUF_PAGESIZE) + ".");
            return;
        }
    }

    QString fileName = QFileDialog::getSaveFileName(this, "Save sound file", lastSelectedDir.remove(fileSuffix), "Sound Files (*.nwav)");
    if(fileName == "")
        return;
    lastSelectedDir = fileName;

    printClrCon();

    QFile file(fileName);
    if(!file.open(QIODevice::WriteOnly))
    {
        QMessageBox::critical(this, "Error.", "Could not open file for writing, please try again.");
        printCon("Error: Could not open file for writing, please try again.");
        return;
    }

    QByteArray procRawData = processRawData();

    int eventCount = eventEditor.eventEntries.size();

    unalignedEvents = (eventCount % 4);
    eventIDBlockSize = eventCount + (4 - unalignedEvents);
    eventBlockSize = eventIDBlockSize + (eventCount * 4);
    int dataOffset = sizeof(NWAVHeader) + eventBlockSize;

    int nwavSize = dataOffset;
    nwavSize += procRawData.size();

    NWAVStruct nwav;
    nwav.header.magic = NWAV;
    nwav.header.fileSize = nwavSize;
    nwav.header.sampleRate = ui->sampleRate_sb->value();
    nwav.header.loopStart = loopStart;
    nwav.header.loopEnd = loopEnd;
    nwav.header.format = ui->pcm16_rb->isChecked();
    nwav.header.stereo = ui->stereo_cb->isChecked();
    nwav.header.numEvents = eventCount;
    nwav.header.padding = 0;
    nwav.events.resize(eventCount);
    for(int i = 0; i < eventCount; i++)
    {
        nwav.events[i].eventID = eventEditor.eventEntries[i]->eventID;
        nwav.events[i].sample = eventEditor.eventEntries[i]->sample;
    }
    nwav.data = procRawData;

    QByteArray resultData = nwavStructToBin(nwav);

    printCon("Info: Writing to file...");
    file.write(resultData);

    file.close();
    printCon("Done! The file was saved to the destination.");
}

void MainWindow::on_changeEvents_pb_clicked()
{
    eventEditor.show();
}
