#ifndef EVENTEDITOR_H
#define EVENTEDITOR_H

#include <QWidget>
#include <QVBoxLayout>

#include "evententry.h"

namespace Ui {
class EventEditor;
}

class EventEditor : public QWidget
{
    Q_OBJECT

public:
    explicit EventEditor(QWidget *parent = nullptr);
    ~EventEditor();

    QVector<EventEntry*> eventEntries;

    void append(int eventID, int sample);

    void clear();

private slots:
    void on_addEntry_pb_clicked();

private:
    Ui::EventEditor *ui;
};

#endif // EVENTEDITOR_H
