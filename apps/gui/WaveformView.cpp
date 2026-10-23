#include "WaveformView.hpp"

#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>
#include <climits>
#include <cmath>

namespace
{
	constexpr int RulerTicksHeight = 18;
	constexpr int RulerFlagsHeight = 18;
	constexpr int GrabRadius = 5;
	constexpr double MinFramesPerPixel = 1.0 / 64;

	const QColor LoopStartColor(60, 200, 110);
	const QColor LoopEndColor(229, 83, 75);
	const QColor EventColor(240, 160, 48);
	const QColor PlayheadColor(255, 210, 74);
	const QColor WaveColor(61, 155, 233);

	QString formatSeconds(double seconds, int decimals)
	{
		int minutes = int(seconds / 60);
		double rest = seconds - minutes * 60;
		return QString("%1:%2").arg(minutes).arg(rest, decimals ? decimals + 3 : 2, 'f', decimals, QChar('0'));
	}
}

WaveformView::WaveformView(QWidget* parent) : QAbstractScrollArea(parent)
{
	setFocusPolicy(Qt::StrongFocus);
	setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
	setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	viewport()->setMouseTracking(true);
	viewport()->setAutoFillBackground(false);

	connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
		fitMode = false;
		setFirstFrame(value);
	});
}

void WaveformView::setAudio(std::shared_ptr<const Audio> newAudio)
{
	audio = std::move(newAudio);
	peaks.build(audio);
	loop.reset();
	events.clear();
	selectedEvent = -1;
	playhead = 0;
	zoomToFit();
}

void WaveformView::setLoop(std::optional<Loop> newLoop)
{
	loop = newLoop;
	viewport()->update();
}

void WaveformView::setEvents(std::vector<Event> newEvents)
{
	events = std::move(newEvents);
	if (selectedEvent >= int(events.size()))
		selectedEvent = -1;
	viewport()->update();
}

void WaveformView::setSelectedEvent(int index)
{
	selectedEvent = index;
	viewport()->update();
}

void WaveformView::setPlayhead(u32 frame, bool follow)
{
	playhead = frame;
	if (follow && drag != Drag::Pan)
	{
		double visible = visibleFrames();
		if (frame < firstFrame || frame > firstFrame + visible * 0.95)
			setFirstFrame(frame - visible * 0.05);
	}
	viewport()->update();
}

int WaveformView::rulerHeight() const
{
	return RulerTicksHeight + RulerFlagsHeight;
}

u32 WaveformView::frameAtX(int x) const
{
	double frame = std::clamp(std::round(firstFrame + x * framesPerPixel), 0.0, double(totalFrames()));
	return u32(frame);
}

u32 WaveformView::snapped(u32 frame) const
{
	if (!audio || !snapToZero || (QGuiApplication::keyboardModifiers() & Qt::AltModifier))
		return frame;

	const u32 total = totalFrames();
	const auto level = [&](u32 i) {
		float sum = 0;
		for (const auto& ch : audio->channels)
			sum += ch[i];
		return sum;
	};

	// The first frame after the signal crosses zero, nearest to where it was dropped.
	const u32 radius = u32(std::max(32.0, 8 * framesPerPixel));
	for (u32 d = 0; d <= radius; d++)
	{
		for (int sign : { 1, -1 })
		{
			if (d == 0 && sign < 0)
				continue;
			s64 i = s64(frame) + sign * s64(d);
			if (i < 1 || i >= total)
				continue;
			if ((level(u32(i) - 1) < 0) != (level(u32(i)) < 0))
				return u32(i);
		}
	}
	return frame;
}

void WaveformView::setFirstFrame(double frame)
{
	double maxFirst = std::max(0.0, totalFrames() - visibleFrames());
	firstFrame = std::clamp(frame, 0.0, maxFirst);
	updateScrollBar();
	viewport()->update();
}

void WaveformView::updateScrollBar()
{
	QScrollBar* bar = horizontalScrollBar();
	double maxFirst = std::max(0.0, totalFrames() - visibleFrames());
	bar->blockSignals(true);
	bar->setRange(0, int(std::min<double>(maxFirst, INT_MAX)));
	bar->setPageStep(int(std::min<double>(visibleFrames(), INT_MAX)));
	bar->setSingleStep(std::max(1, int(visibleFrames() / 10)));
	bar->setValue(int(std::min<double>(firstFrame, INT_MAX)));
	bar->blockSignals(false);
}

void WaveformView::zoomToFit()
{
	fitMode = true;
	double width = std::max(1, viewport()->width());
	framesPerPixel = std::max(totalFrames() / width, MinFramesPerPixel);
	setFirstFrame(0);
}

void WaveformView::zoomToLoop()
{
	if (!loop)
		return;
	fitMode = false;
	double width = std::max(1, viewport()->width());
	double length = loop->end - loop->start;
	framesPerPixel = std::max(length * 1.1 / width, MinFramesPerPixel);
	setFirstFrame(loop->start - length * 0.05);
}

void WaveformView::zoomBy(double factor, int anchorX)
{
	if (!audio)
		return;
	fitMode = false;
	double width = std::max(1, viewport()->width());
	if (anchorX < 0)
		anchorX = int(width / 2);

	double anchor = firstFrame + anchorX * framesPerPixel;
	double maxFpp = std::max(totalFrames() / width, MinFramesPerPixel);
	framesPerPixel = std::clamp(framesPerPixel / factor, MinFramesPerPixel, maxFpp);
	setFirstFrame(anchor - anchorX * framesPerPixel);
}

void WaveformView::resizeEvent(QResizeEvent* event)
{
	QAbstractScrollArea::resizeEvent(event);
	if (fitMode)
		zoomToFit();
	else
		setFirstFrame(firstFrame);
}

// Paint -----------------------------------------------------------------------

void WaveformView::paintEvent(QPaintEvent*)
{
	QPainter p(viewport());
	p.fillRect(viewport()->rect(), palette().color(QPalette::Base));
	if (!audio || audio->channels.empty())
	{
		p.setPen(palette().color(QPalette::PlaceholderText));
		p.drawText(viewport()->rect(), Qt::AlignCenter, tr("Open a WAV file (Ctrl+O) or drop one here"));
		return;
	}

	drawChannels(p);
	drawRuler(p);
	drawMarkers(p);
}

void WaveformView::drawRuler(QPainter& p)
{
	const int w = viewport()->width();
	const QColor text = palette().color(QPalette::Text);
	p.fillRect(0, 0, w, rulerHeight(), palette().color(QPalette::Window));
	p.setPen(palette().color(QPalette::Mid));
	p.drawLine(0, rulerHeight() - 1, w, rulerHeight() - 1);

	const double rate = audio->sampleRate;
	const double secondsPerPixel = framesPerPixel / rate;
	static const double steps[] = { 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 30, 60, 120, 300, 600, 1800 };
	double step = steps[std::size(steps) - 1];
	for (double s : steps)
	{
		if (s / secondsPerPixel >= 90)
		{
			step = s;
			break;
		}
	}
	int decimals = step >= 1 ? 0 : (step >= 0.1 ? 1 : (step >= 0.01 ? 2 : 3));

	const double startSeconds = firstFrame / rate;
	const double endSeconds = (firstFrame + w * framesPerPixel) / rate;
	const double minor = step / 5;
	p.setFont(font());
	for (double t = std::floor(startSeconds / minor) * minor; t <= endSeconds + minor; t += minor)
	{
		double x = xOf(t * rate);
		bool major = std::abs(std::remainder(t, step)) < minor / 2;
		p.setPen(major ? text : palette().color(QPalette::Mid));
		p.drawLine(QPointF(x, major ? 4 : RulerTicksHeight - 5), QPointF(x, RulerTicksHeight - 1));
		if (major && t >= -minor / 2)
		{
			p.setPen(text);
			p.drawText(QPointF(x + 3, 12), formatSeconds(std::max(t, 0.0), decimals));
		}
	}
}

void WaveformView::drawChannels(QPainter& p)
{
	const int w = viewport()->width();
	const int top = rulerHeight();
	const int height = viewport()->height() - top;
	const u32 total = audio->frames();
	const u32 count = u32(audio->channels.size());

	for (u32 ch = 0; ch < count; ch++)
	{
		const double laneTop = top + double(height) * ch / count;
		const double laneHeight = double(height) / count;
		const double mid = laneTop + laneHeight / 2;
		const double half = std::max(1.0, laneHeight / 2 - 3);

		p.setPen(palette().color(QPalette::Mid));
		p.drawLine(QPointF(0, mid), QPointF(w, mid));
		if (ch > 0)
			p.drawLine(QPointF(0, laneTop), QPointF(w, laneTop));

		p.setPen(QPen(WaveColor, 1));
		if (framesPerPixel >= 1)
		{
			QVector<QLineF> lines;
			lines.reserve(w);
			for (int x = 0; x < w; x++)
			{
				double a = firstFrame + x * framesPerPixel;
				if (a >= total)
					break;
				u64 first = u64(a);
				u64 last = std::max<u64>(first + 1, u64(std::ceil(a + framesPerPixel)));
				float lo, hi;
				peaks.range(ch, first, last, lo, hi);
				double y1 = mid - hi * half;
				double y2 = std::max(mid - lo * half, y1 + 1);
				lines.append(QLineF(x + 0.5, y1, x + 0.5, y2));
			}
			p.drawLines(lines);
		}
		else
		{
			// Zoomed in past one pixel per frame: draw the samples themselves.
			const std::vector<float>& src = audio->channels[ch];
			s64 i0 = std::max<s64>(0, s64(std::floor(firstFrame)) - 1);
			s64 i1 = std::min<s64>(total - 1, s64(std::ceil(firstFrame + w * framesPerPixel)) + 1);
			QPolygonF line;
			for (s64 i = i0; i <= i1; i++)
				line << QPointF(xOf(double(i)), mid - src[size_t(i)] * half);
			p.setRenderHint(QPainter::Antialiasing, true);
			p.drawPolyline(line);
			if (1 / framesPerPixel >= 8)
			{
				p.setBrush(WaveColor);
				for (const QPointF& pt : line)
					p.drawEllipse(pt, 2.5, 2.5);
				p.setBrush(Qt::NoBrush);
			}
			p.setRenderHint(QPainter::Antialiasing, false);
		}
	}
}

void WaveformView::drawMarkers(QPainter& p)
{
	const int w = viewport()->width();
	const int h = viewport()->height();
	const int top = rulerHeight();
	const int flagTop = RulerTicksHeight;
	const int flagH = RulerFlagsHeight - 2;
	QFont small = font();
	small.setPointSizeF(std::max(6.0, font().pointSizeF() - 1));

	// Fills the ruler flag row too, so the flags sit on the ruler drawn after.
	const auto flag = [&](double x, const QColor& color, const QString& label, bool toRight) {
		p.setFont(small);
		int width = p.fontMetrics().horizontalAdvance(label) + 8;
		QRectF r(toRight ? x : x - width, flagTop + 1, width, flagH);
		p.fillRect(r, color);
		p.setPen(QColor(20, 20, 20));
		p.drawText(r, Qt::AlignCenter, label);
	};

	if (loop)
	{
		double x0 = xOf(loop->start);
		double x1 = xOf(loop->end);
		double c0 = std::clamp(x0, 0.0, double(w));
		double c1 = std::clamp(x1, 0.0, double(w));
		p.fillRect(QRectF(c0, top, c1 - c0, h - top), QColor(60, 200, 110, 34));
		p.fillRect(QRectF(c1, top, w - c1, h - top), QColor(0, 0, 0, 90));

		p.setPen(QPen(LoopStartColor, 2));
		p.drawLine(QPointF(x0, top), QPointF(x0, h));
		p.setPen(QPen(LoopEndColor, 2));
		p.drawLine(QPointF(x1, top), QPointF(x1, h));
		flag(x0, LoopStartColor, tr("loop start"), true);
		flag(x1, LoopEndColor, tr("loop end"), false);
	}

	for (size_t i = 0; i < events.size(); i++)
	{
		const Event& e = events[i];
		double x = xOf(e.frame);
		if (x < -200 || x > w + 200)
			continue;
		bool selected = int(i) == selectedEvent;
		QColor color = selected ? EventColor.lighter(130) : EventColor;
		p.setPen(QPen(color, selected ? 2 : 1, Qt::DashLine));
		p.drawLine(QPointF(x, top), QPointF(x, h));
		flag(x, color, e.label.empty() ? QString("#%1").arg(e.id) : QString("#%1 %2").arg(e.id).arg(QString::fromStdString(e.label)), true);
	}

	double px = xOf(playhead);
	p.setPen(QPen(PlayheadColor, 1));
	p.drawLine(QPointF(px, 0), QPointF(px, h));
	p.setBrush(PlayheadColor);
	p.setPen(Qt::NoPen);
	p.drawPolygon(QPolygonF({ QPointF(px - 5, 0), QPointF(px + 5, 0), QPointF(px, 7) }));
	p.setBrush(Qt::NoBrush);
}

// Mouse -----------------------------------------------------------------------

WaveformView::Hit WaveformView::hitTest(const QPoint& pos) const
{
	Hit best;
	double bestDistance = GrabRadius + 0.5;
	const auto consider = [&](double x, Drag kind, int event) {
		double d = std::abs(pos.x() - x);
		if (d < bestDistance)
		{
			bestDistance = d;
			best = { kind, event };
		}
	};

	for (size_t i = 0; i < events.size(); i++)
		consider(xOf(events[i].frame), Drag::Event, int(i));
	if (loop)
	{
		consider(xOf(loop->start), Drag::LoopStart, -1);
		consider(xOf(loop->end), Drag::LoopEnd, -1);
	}
	return best;
}

void WaveformView::setLoopFromView(std::optional<Loop> newLoop)
{
	loop = newLoop;
	viewport()->update();
	emit loopChanged(loop);
}

void WaveformView::setLoopStartAt(u32 frame)
{
	const u32 total = totalFrames();
	if (!total)
		return;
	frame = std::min(frame, total - 1);
	Loop l = loop ? *loop : Loop{ frame, total };
	l.start = frame;
	if (l.end <= frame)
		l.end = total;
	setLoopFromView(l);
}

void WaveformView::setLoopEndAt(u32 frame)
{
	const u32 total = totalFrames();
	if (!total)
		return;
	frame = std::clamp(frame, u32(1), total);
	Loop l = loop ? *loop : Loop{ 0, frame };
	l.end = frame;
	if (l.start >= frame)
		l.start = 0;
	setLoopFromView(l);
}

void WaveformView::mousePressEvent(QMouseEvent* event)
{
	setFocus();
	if (!audio)
		return;

	const QPoint pos = event->position().toPoint();
	if (event->button() == Qt::MiddleButton)
	{
		drag = Drag::Pan;
	}
	else if (event->button() == Qt::LeftButton)
	{
		Hit hit = hitTest(pos);
		if (hit.kind == Drag::Event)
		{
			selectedEvent = hit.event;
			dragEvent = hit.event;
			emit eventSelected(selectedEvent);
			drag = Drag::Event;
		}
		else if (hit.kind != Drag::None)
		{
			drag = hit.kind;
		}
		else if (pos.y() < rulerHeight())
		{
			drag = Drag::Pan;
		}
		else
		{
			if (selectedEvent >= 0)
			{
				selectedEvent = -1;
				emit eventSelected(-1);
			}
			drag = Drag::Seek;
			playhead = frameAtX(pos.x());
			emit seekRequested(std::min(playhead, totalFrames() - 1));
		}
	}
	else
	{
		return;
	}

	dragStartX = pos.x();
	dragStartFirst = firstFrame;
	if (drag == Drag::Pan)
		viewport()->setCursor(Qt::ClosedHandCursor);
	viewport()->update();
}

void WaveformView::mouseMoveEvent(QMouseEvent* event)
{
	const QPoint pos = event->position().toPoint();
	const u32 total = totalFrames();

	switch (drag)
	{
	case Drag::None:
		if (!audio)
			break;
		if (hitTest(pos).kind != Drag::None)
			viewport()->setCursor(Qt::SizeHorCursor);
		else if (pos.y() < rulerHeight())
			viewport()->setCursor(Qt::OpenHandCursor);
		else
			viewport()->setCursor(Qt::ArrowCursor);
		break;

	case Drag::Pan:
		fitMode = false;
		setFirstFrame(dragStartFirst - (pos.x() - dragStartX) * framesPerPixel);
		break;

	case Drag::Seek:
		playhead = std::min(frameAtX(pos.x()), total - 1);
		emit seekRequested(playhead);
		viewport()->update();
		break;

	case Drag::LoopStart:
		if (loop)
		{
			u32 frame = std::min(snapped(frameAtX(pos.x())), loop->end - 1);
			setLoopFromView(Loop{ frame, loop->end });
		}
		break;

	case Drag::LoopEnd:
		if (loop)
		{
			u32 frame = std::max(snapped(frameAtX(pos.x())), loop->start + 1);
			setLoopFromView(Loop{ loop->start, std::min(frame, total) });
		}
		break;

	case Drag::Event:
		if (dragEvent >= 0 && dragEvent < int(events.size()))
		{
			u32 frame = std::min(frameAtX(pos.x()), total - 1);
			events[dragEvent].frame = frame;
			emit eventMoved(dragEvent, frame);
			viewport()->update();
		}
		break;
	}
}

void WaveformView::mouseReleaseEvent(QMouseEvent*)
{
	drag = Drag::None;
	dragEvent = -1;
	viewport()->unsetCursor();
}

void WaveformView::mouseDoubleClickEvent(QMouseEvent* event)
{
	if (!audio || event->button() != Qt::LeftButton)
		return;
	const QPoint pos = event->position().toPoint();
	if (pos.y() >= rulerHeight() && hitTest(pos).kind == Drag::None)
		emit eventAddRequested(std::min(frameAtX(pos.x()), totalFrames() - 1));
}

void WaveformView::wheelEvent(QWheelEvent* event)
{
	if (!audio)
		return;
	int delta = event->angleDelta().y() + event->angleDelta().x();
	if (event->modifiers() & Qt::ControlModifier)
	{
		zoomBy(std::pow(1.25, delta / 120.0), int(event->position().x()));
	}
	else
	{
		fitMode = false;
		setFirstFrame(firstFrame - delta / 120.0 * visibleFrames() / 8);
	}
	event->accept();
}

void WaveformView::keyPressEvent(QKeyEvent* event)
{
	switch (event->key())
	{
	case Qt::Key_Delete:
	case Qt::Key_Backspace:
		if (selectedEvent >= 0)
			emit eventDeleteRequested(selectedEvent);
		break;
	case Qt::Key_Left:
		fitMode = false;
		setFirstFrame(firstFrame - visibleFrames() / 8);
		break;
	case Qt::Key_Right:
		fitMode = false;
		setFirstFrame(firstFrame + visibleFrames() / 8);
		break;
	case Qt::Key_Home:
		fitMode = false;
		setFirstFrame(0);
		break;
	case Qt::Key_End:
		fitMode = false;
		setFirstFrame(totalFrames());
		break;
	default:
		QAbstractScrollArea::keyPressEvent(event);
	}
}

void WaveformView::contextMenuEvent(QContextMenuEvent* event)
{
	if (!audio)
		return;

	const QPoint pos = viewport()->mapFromGlobal(event->globalPos());
	const u32 frame = frameAtX(pos.x());
	const Hit hit = hitTest(pos);

	QMenu menu(this);
	menu.addAction(tr("Set loop start here"), this, [this, frame] { setLoopStartAt(snapped(frame)); });
	menu.addAction(tr("Set loop end here"), this, [this, frame] { setLoopEndAt(snapped(frame)); });
	if (loop)
		menu.addAction(tr("Clear loop"), this, [this] { setLoopFromView(std::nullopt); });
	menu.addSeparator();
	menu.addAction(tr("Add event here"), this, [this, frame] { emit eventAddRequested(std::min(frame, totalFrames() - 1)); });
	if (hit.kind == Drag::Event)
	{
		int index = hit.event;
		menu.addAction(tr("Delete event #%1").arg(events[index].id), this, [this, index] { emit eventDeleteRequested(index); });
	}
	menu.addSeparator();
	menu.addAction(tr("Zoom to fit"), this, [this] { zoomToFit(); });
	QAction* toLoop = menu.addAction(tr("Zoom to loop"), this, [this] { zoomToLoop(); });
	toLoop->setEnabled(loop.has_value());
	menu.exec(event->globalPos());
}
