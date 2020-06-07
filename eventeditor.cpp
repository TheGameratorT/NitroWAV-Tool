#include "eventeditor.h"
#include "ui_eventeditor.h"

EventEditor::EventEditor(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::EventEditor)
{
    ui->setupUi(this);
    ui->saLayout->setAlignment(Qt::AlignTop);
}

void EventEditor::on_addEntry_pb_clicked()
{
    if(eventEntries.size() < 256)
        append(0, 0);
}

void EventEditor::append(int eventID, int sample)
{
    EventEntry* eventEntry = new EventEntry(this, &eventEntries);
    eventEntry->setEventID(eventID);
    eventEntry->setSample(sample);
    ui->saLayout->addWidget(eventEntry);
}

void EventEditor::clear()
{
    //Delete all entries
    int entryCount = eventEntries.size();
    if(entryCount)
    {
        for(int i = 0; i < entryCount; i++)
        {
            eventEntries[0]->close();
            delete eventEntries[0];
            eventEntries.remove(0);
        }
    }
}

EventEditor::~EventEditor()
{
    clear();
    delete ui;
}
