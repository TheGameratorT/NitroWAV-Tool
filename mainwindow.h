#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "eventeditor.h"

#include <QMainWindow>
#include <QFile>
#include <QFileInfo>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:

    struct WAVStruct
    {
        int ChunkID;
        int ChunkSize;
        int Format;

        int Subchunk1ID;
        int Subchunk1Size;
        signed short AudioFormat;
        signed short NumChannels;
        int SampleRate;
        int ByteRate;
        signed short BlockAlign;
        signed short BitsPerSample;

        int Subchunk2ID;
        int Subchunk2Size;

        char Data[];
    };

    struct NWAVEvent
    {
        int eventID;
        int sample;
    };

    struct NWAVHeader
    {
        int magic;
        int fileSize;
        int sampleRate;
        int loopStart;
        int loopEnd;
        unsigned char format;
        unsigned char stereo;
        unsigned char numEvents;
        unsigned char padding;
    };

    struct NWAVStruct
    {
        NWAVHeader header;
        QVector<NWAVEvent> events;
        QByteArray data;
    };

    void setEditMode(int mode);

    void printClrCon();
    void printCon(const QString& string);

    bool ReadWAV(QFile& file);

    void ReadRAW(QFile& file, QFileInfo& fInfo);

    QByteArray processRawData();

    QByteArray nwavStructToBin(NWAVStruct& nwav);

    QString ReadWAVTag(WAVStruct* wav, int fileSize, const QString& tagName);

    bool GetLoopSamples(WAVStruct* wav, int fileSize, int& loopStart, int& loopEnd);
    bool GetLoopMarkers(WAVStruct* wav, int fileSize, int& loopStart, int& loopEnd);

    EventEditor eventEditor;
    bool conv_warning_fired = false;
    QByteArray raw_data;
    QString lastSelectedDir;
    QString fileSuffix;

    int unalignedEvents = 0;
    int eventIDBlockSize = 0;
    int eventBlockSize = 0;

private slots:
    void on_search_pb_clicked();

    void on_wavPath_le_editingFinished();

    void on_convert_pb_clicked();

    void on_changeEvents_pb_clicked();

private:
    Ui::MainWindow *ui;
};
#endif // MAINWINDOW_H
