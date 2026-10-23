#pragma once

#include "WaveformPeaks.hpp"
#include "audio.hpp"

#include <QAbstractScrollArea>

// Draws an Audio as a waveform that pans and zooms, with draggable loop handles
// and event markers. It keeps its own copy of the loop and events to draw, and
// reports edits by signal. The setters never emit, so the owner can call them
// from the handlers of those signals.
//
//   click the waveform: seek          drag the ruler, or the middle button: pan
//   wheel: pan                        Ctrl + wheel: zoom around the pointer
//   drag a handle or flag: move it    double click: add an event
//   Delete: remove the selected event Alt: no snapping to zero crossings
class WaveformView : public QAbstractScrollArea
{
	Q_OBJECT

public:
	explicit WaveformView(QWidget* parent = nullptr);

	void setAudio(std::shared_ptr<const Audio> audio);
	void setLoop(std::optional<Loop> loop);
	void setEvents(std::vector<Event> events);
	void setSelectedEvent(int index);
	void setSnapToZero(bool snap) { snapToZero = snap; }

	// Moves the playhead. With follow, the view scrolls to keep it in sight.
	void setPlayhead(u32 frame, bool follow);

	void zoomToFit();
	void zoomToLoop();
	void zoomBy(double factor, int anchorX = -1);

	// Edits the loop the way a drag would, creating it if there is none. Emits loopChanged.
	void setLoopStartAt(u32 frame);
	void setLoopEndAt(u32 frame);

	u32 frameAtX(int x) const;

	// What a context menu or key shortcut at a frame would set. Snaps to a zero crossing.
	u32 snapped(u32 frame) const;

signals:
	void loopChanged(std::optional<Loop> loop);
	void eventMoved(int index, u32 frame);
	void eventAddRequested(u32 frame);
	void eventDeleteRequested(int index);
	void eventSelected(int index);
	void seekRequested(u32 frame);

protected:
	void paintEvent(QPaintEvent* event) override;
	void resizeEvent(QResizeEvent* event) override;
	void mousePressEvent(QMouseEvent* event) override;
	void mouseMoveEvent(QMouseEvent* event) override;
	void mouseReleaseEvent(QMouseEvent* event) override;
	void mouseDoubleClickEvent(QMouseEvent* event) override;
	void wheelEvent(QWheelEvent* event) override;
	void keyPressEvent(QKeyEvent* event) override;
	void contextMenuEvent(QContextMenuEvent* event) override;

private:
	enum class Drag
	{
		None,
		Pan,
		Seek,
		LoopStart,
		LoopEnd,
		Event
	};

	struct Hit
	{
		Drag kind = Drag::None;
		int event = -1;
	};

	u32 totalFrames() const { return audio ? audio->frames() : 0; }
	double visibleFrames() const { return viewport()->width() * framesPerPixel; }
	double xOf(double frame) const { return (frame - firstFrame) / framesPerPixel; }
	int rulerHeight() const;
	Hit hitTest(const QPoint& pos) const;
	void setFirstFrame(double frame);
	void updateScrollBar();
	void setLoopFromView(std::optional<Loop> loop);
	void drawRuler(QPainter& p);
	void drawChannels(QPainter& p);
	void drawMarkers(QPainter& p);

	std::shared_ptr<const Audio> audio;
	WaveformPeaks peaks;
	std::optional<Loop> loop;
	std::vector<Event> events;
	int selectedEvent = -1;
	u32 playhead = 0;
	bool snapToZero = true;

	double framesPerPixel = 1;
	double firstFrame = 0;
	bool fitMode = true; // Shows everything, until the user pans or zooms.

	Drag drag = Drag::None;
	int dragEvent = -1;
	int dragStartX = 0;
	double dragStartFirst = 0;
};
