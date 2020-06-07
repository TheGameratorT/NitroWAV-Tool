#include "evententry.h"
#include "ui_evententry.h"

#include <QFileDialog>

EventEntry::EventEntry(QWidget *parent, QVector<EventEntry*>* entries) :
    QWidget(parent),
    ui(new Ui::EventEntry)
{
    ui->setupUi(this);

    id = entries->size();
    entries->append(this);

    this->setFixedHeight(this->height());
    this->entries = entries;
}

EventEntry::~EventEntry()
{
    delete ui;
}

void EventEntry::on_remove_pb_clicked()
{
    entries->remove(id);

    for(int i = id; i < entries->size(); i++)
        (*entries)[i]->id -= 1;

    close();
    delete this;
}

void EventEntry::setEventID(int eventID) { ui->eventID_sb->setValue(eventID); }
void EventEntry::setSample(int sample) { ui->sample_sb->setValue(sample); }

void EventEntry::on_sample_sb_valueChanged(int arg1)
{
    this->sample = arg1;
}

void EventEntry::on_eventID_sb_valueChanged(int arg1)
{
    this->eventID = arg1;
}
