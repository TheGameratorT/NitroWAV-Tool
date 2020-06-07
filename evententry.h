#ifndef EVENTENTRY_H
#define EVENTENTRY_H

#include <QWidget>
#include <QFileInfo>

namespace Ui {
class EventEntry;
}

class EventEntry : public QWidget
{
    Q_OBJECT

public:
    explicit EventEntry(QWidget *parent, QVector<EventEntry*>* entries);
    ~EventEntry();

    QVector<EventEntry*>* entries;
    int id;
    int eventID = 0;
    int sample = 0;

    void setEventID(int eventID);
    void setSample(int sample);


private slots:

    void on_remove_pb_clicked();

    void on_sample_sb_valueChanged(int arg1);

    void on_eventID_sb_valueChanged(int arg1);

private:
    Ui::EventEntry *ui;
};

#endif // EVENTENTRY_H
