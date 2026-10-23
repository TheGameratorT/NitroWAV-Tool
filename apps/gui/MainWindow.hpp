#pragma once

#include "audio.hpp"

#include <QMainWindow>

#include <memory>

class LoopPlayer;
class QAction;
class QCheckBox;
class QLabel;
class QSpinBox;
class QTableWidget;
class QTimer;
class WaveformView;

// Holds what is being edited (the audio, its loop and its events) and keeps the
// waveform, the side panel and the player showing the same thing.
class MainWindow : public QMainWindow
{
	Q_OBJECT

public:
	MainWindow();
	~MainWindow() override;

	bool openFile(const QString& path);

	WaveformView* waveform() const { return view; }
	LoopPlayer* audioPlayer() const { return player; }
	const std::optional<Loop>& currentLoop() const { return loop; }
	const std::vector<Event>& currentEvents() const { return events; }

protected:
	void dragEnterEvent(QDragEnterEvent* event) override;
	void dropEvent(QDropEvent* event) override;

private:
	void buildUi();
	void open();
	void exportFile();
	void togglePlay();
	void stop();
	void previewSeam();
	void tick();
	void updatePlayActions();

	void applyLoop(std::optional<Loop> newLoop);
	void applyEvents(std::vector<Event> newEvents, int select = -1);
	void loopSpinChanged();
	void loopCheckChanged(bool checked);
	void eventCellChanged(int row, int column);
	void addEventAt(u32 frame);
	void removeEvent(int index);
	void fillEventRow(int row);
	QString formatFrame(u32 frame) const;

	std::shared_ptr<Audio> audio;
	QString filePath;
	std::shared_ptr<const std::vector<u8>> sourceFile; // The opened WAV, when the player can stream it as it is.
	bool sourceIsAdpcm = false;
	std::optional<Loop> loop;
	std::vector<Event> events;
	bool updatingUi = false;

	WaveformView* view = nullptr;
	LoopPlayer* player = nullptr;
	QTimer* timer = nullptr;

	QCheckBox* loopCheck = nullptr;
	QSpinBox* loopStartSpin = nullptr;
	QSpinBox* loopEndSpin = nullptr;
	QLabel* loopInfo = nullptr;
	QTableWidget* eventTable = nullptr;
	QLabel* timeLabel = nullptr;
	QLabel* fileLabel = nullptr;

	QAction* playAction = nullptr;
	QAction* stopAction = nullptr;
	QAction* loopPreviewAction = nullptr;
	QAction* seamAction = nullptr;
	QAction* followAction = nullptr;
	QAction* snapAction = nullptr;
	QAction* exportAction = nullptr;
	int lastState = -1;
};
