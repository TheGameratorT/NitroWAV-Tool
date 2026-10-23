#include "MainWindow.hpp"

#include "ExportDialog.hpp"
#include "LoopPlayer.hpp"
#include "WaveformView.hpp"
#include "convert.hpp"
#include "decode.hpp"

#include <QAction>
#include <QCheckBox>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QStyle>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>

namespace
{
	QString describeFormat(const WavInfo& info)
	{
		if (info.tag == 0x11)
			return "IMA-ADPCM";
		if (info.tag == 1)
			return QString("PCM%1").arg(info.bits);
		return QString("tag %1, %2 bit").arg(info.tag).arg(info.bits);
	}

	QString channelName(size_t channels)
	{
		return channels == 1 ? "mono" : (channels == 2 ? "stereo" : QString("%1 channels").arg(channels));
	}
}

MainWindow::MainWindow()
{
	setWindowTitle("NitroWAV");
	resize(1200, 620);
	setAcceptDrops(true);

	player = new LoopPlayer(this);
	buildUi();

	timer = new QTimer(this);
	connect(timer, &QTimer::timeout, this, &MainWindow::tick);
	timer->start(16);

	connect(player, &LoopPlayer::finished, this, &MainWindow::updatePlayActions);
	if (!player->isAvailable())
		statusBar()->showMessage(tr("No audio device was found, so playback is disabled."));
	updatePlayActions();
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi()
{
	view = new WaveformView;
	setCentralWidget(view);

	connect(view, &WaveformView::loopChanged, this, [this](std::optional<Loop> l) { applyLoop(l); });
	connect(view, &WaveformView::seekRequested, this, [this](u32 frame) {
		player->seek(frame);
		timeLabel->setText(formatFrame(frame));
	});
	connect(view, &WaveformView::eventMoved, this, [this](int index, u32 frame) {
		events[index].frame = frame;
		updatingUi = true;
		eventTable->item(index, 1)->setText(QString::number(frame));
		updatingUi = false;
	});
	connect(view, &WaveformView::eventAddRequested, this, &MainWindow::addEventAt);
	connect(view, &WaveformView::eventDeleteRequested, this, &MainWindow::removeEvent);
	connect(view, &WaveformView::eventSelected, this, [this](int index) {
		updatingUi = true;
		if (index >= 0)
			eventTable->selectRow(index);
		else
			eventTable->clearSelection();
		updatingUi = false;
	});

	// Actions -------------------------------------------------------------------
	const auto make = [this](const QString& text, const QKeySequence& key, auto slot, QStyle::StandardPixmap icon = QStyle::StandardPixmap(-1)) {
		auto* action = new QAction(text, this);
		if (!key.isEmpty())
			action->setShortcut(key);
		if (int(icon) >= 0)
			action->setIcon(style()->standardIcon(icon));
		connect(action, &QAction::triggered, this, slot);
		return action;
	};
	const auto toggle = [](QAction* action, bool on) {
		action->setCheckable(true);
		action->setChecked(on);
		return action;
	};

	QAction* openAction = make(tr("&Open..."), QKeySequence::Open, [this] { open(); }, QStyle::SP_DialogOpenButton);
	exportAction = make(tr("&Export WAV..."), QKeySequence("Ctrl+E"), [this] { exportFile(); }, QStyle::SP_DialogSaveButton);
	QAction* quitAction = make(tr("&Quit"), QKeySequence::Quit, [this] { close(); });
	exportAction->setEnabled(false);

	playAction = make(tr("Play"), QKeySequence(Qt::Key_Space), [this] { togglePlay(); }, QStyle::SP_MediaPlay);
	stopAction = make(tr("Stop"), QKeySequence(Qt::Key_Return), [this] { stop(); }, QStyle::SP_MediaStop);
	loopPreviewAction = toggle(make(tr("Loop"), QKeySequence("L"), [this] {
		player->setLoopEnabled(loopPreviewAction->isChecked());
	}, QStyle::SP_BrowserReload), true);
	loopPreviewAction->setToolTip(tr("Play the loop instead of playing through once"));
	seamAction = make(tr("Hear the seam"), QKeySequence("P"), [this] { previewSeam(); }, QStyle::SP_MediaSeekForward);
	seamAction->setToolTip(tr("Play from just before the loop end to hear the wrap"));

	QAction* loopStartHere = make(tr("Set loop start at playhead"), QKeySequence("I"), [this] {
		if (audio)
			view->setLoopStartAt(player->playhead());
	});
	QAction* loopEndHere = make(tr("Set loop end at playhead"), QKeySequence("O"), [this] {
		if (audio)
			view->setLoopEndAt(player->playhead());
	});
	QAction* eventHere = make(tr("Add event at playhead"), QKeySequence("E"), [this] {
		if (audio)
			addEventAt(player->playhead());
	});

	QAction* zoomIn = make(tr("Zoom in"), QKeySequence("Ctrl+="), [this] { view->zoomBy(2); }, QStyle::SP_ArrowUp);
	QAction* zoomOut = make(tr("Zoom out"), QKeySequence("Ctrl+-"), [this] { view->zoomBy(0.5); }, QStyle::SP_ArrowDown);
	QAction* zoomFit = make(tr("Zoom to fit"), QKeySequence("Ctrl+0"), [this] { view->zoomToFit(); });
	QAction* zoomLoop = make(tr("Zoom to loop"), QKeySequence("Ctrl+L"), [this] { view->zoomToLoop(); });
	followAction = toggle(make(tr("Follow playhead"), QKeySequence(), [] {}), true);
	snapAction = toggle(make(tr("Snap to zero crossings"), QKeySequence(), [this] {
		view->setSnapToZero(snapAction->isChecked());
	}), true);
	snapAction->setToolTip(tr("Loop handles land on zero crossings. Hold Alt to place them freely."));

	QMenu* file = menuBar()->addMenu(tr("&File"));
	file->addActions({ openAction, exportAction });
	file->addSeparator();
	file->addAction(quitAction);

	QMenu* playback = menuBar()->addMenu(tr("&Playback"));
	playback->addActions({ playAction, stopAction, loopPreviewAction, seamAction });
	playback->addSeparator();
	playback->addActions({ loopStartHere, loopEndHere, eventHere });

	QMenu* viewMenu = menuBar()->addMenu(tr("&View"));
	viewMenu->addActions({ zoomIn, zoomOut, zoomFit, zoomLoop });
	viewMenu->addSeparator();
	viewMenu->addActions({ followAction, snapAction });

	QToolBar* bar = addToolBar(tr("Main"));
	bar->setMovable(false);
	bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	bar->addActions({ openAction, exportAction });
	bar->addSeparator();
	bar->addActions({ playAction, stopAction, loopPreviewAction, seamAction });
	bar->addSeparator();
	bar->addActions({ zoomIn, zoomOut, zoomFit, zoomLoop });
	bar->addSeparator();
	bar->addActions({ followAction, snapAction });

	// Side panel ----------------------------------------------------------------
	loopCheck = new QCheckBox(tr("Loop"));
	loopStartSpin = new QSpinBox;
	loopEndSpin = new QSpinBox;
	loopInfo = new QLabel;
	loopStartSpin->setRange(0, 0);
	loopEndSpin->setRange(0, 0);
	loopStartSpin->setGroupSeparatorShown(true);
	loopEndSpin->setGroupSeparatorShown(true);

	auto* loopBox = new QGroupBox(tr("Loop (frames, end exclusive)"));
	auto* loopForm = new QFormLayout(loopBox);
	loopForm->addRow(loopCheck);
	loopForm->addRow(tr("Start"), loopStartSpin);
	loopForm->addRow(tr("End"), loopEndSpin);
	loopForm->addRow(loopInfo);
	connect(loopCheck, &QCheckBox::clicked, this, &MainWindow::loopCheckChanged);
	connect(loopStartSpin, &QSpinBox::valueChanged, this, &MainWindow::loopSpinChanged);
	connect(loopEndSpin, &QSpinBox::valueChanged, this, &MainWindow::loopSpinChanged);

	eventTable = new QTableWidget(0, 3);
	eventTable->setHorizontalHeaderLabels({ tr("ID"), tr("Frame"), tr("Label") });
	eventTable->horizontalHeader()->setStretchLastSection(true);
	eventTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	eventTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	eventTable->setMinimumWidth(260);
	eventTable->verticalHeader()->hide();
	eventTable->setSelectionBehavior(QAbstractItemView::SelectRows);
	eventTable->setSelectionMode(QAbstractItemView::SingleSelection);
	connect(eventTable, &QTableWidget::cellChanged, this, &MainWindow::eventCellChanged);
	connect(eventTable, &QTableWidget::currentCellChanged, this, [this](int row) {
		if (!updatingUi)
			view->setSelectedEvent(row);
	});
	connect(eventTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
		if (column == 1 && row < int(events.size()))
		{
			player->seek(events[row].frame);
			view->setPlayhead(events[row].frame, true);
		}
	});

	auto* addButton = new QPushButton(tr("Add at playhead"));
	auto* removeButton = new QPushButton(tr("Remove"));
	connect(addButton, &QPushButton::clicked, this, [this] {
		if (audio)
			addEventAt(player->playhead());
	});
	connect(removeButton, &QPushButton::clicked, this, [this] {
		removeEvent(eventTable->currentRow());
	});

	auto* eventBox = new QGroupBox(tr("Events (cue points)"));
	auto* eventLayout = new QVBoxLayout(eventBox);
	eventLayout->addWidget(eventTable);
	auto* eventButtons = new QHBoxLayout;
	eventButtons->addWidget(addButton);
	eventButtons->addWidget(removeButton);
	eventLayout->addLayout(eventButtons);

	auto* panel = new QWidget;
	auto* panelLayout = new QVBoxLayout(panel);
	panelLayout->addWidget(loopBox);
	panelLayout->addWidget(eventBox, 1);

	auto* dock = new QDockWidget(tr("Loop and events"), this);
	dock->setWidget(panel);
	dock->setFeatures(QDockWidget::DockWidgetMovable);
	addDockWidget(Qt::RightDockWidgetArea, dock);

	// Status bar ----------------------------------------------------------------
	fileLabel = new QLabel(tr("Click to seek. Double-click to add an event. Ctrl+wheel zooms, the wheel or the ruler pans."));
	timeLabel = new QLabel("0:00.000");
	timeLabel->setMinimumWidth(80);
	statusBar()->addWidget(fileLabel, 1);
	statusBar()->addPermanentWidget(timeLabel);
}

QString MainWindow::formatFrame(u32 frame) const
{
	double seconds = audio && audio->sampleRate ? double(frame) / audio->sampleRate : 0;
	int minutes = int(seconds / 60);
	return QString("%1:%2").arg(minutes).arg(seconds - minutes * 60, 6, 'f', 3, QChar('0'));
}

// Files -----------------------------------------------------------------------

void MainWindow::open()
{
	QSettings settings;
	QString filter = ffmpegAvailable() ? tr("Audio (*.wav *.nwav *.mp3 *.flac *.ogg *.oga *.opus *.m4a *.aac *.aif *.aiff *.wma);;All files (*)")
									   : tr("Audio (*.wav *.nwav);;All files (*)");
	QString path = QFileDialog::getOpenFileName(this, tr("Open audio"), settings.value("lastDir").toString(), filter);
	if (!path.isEmpty())
		openFile(path);
}

bool MainWindow::openFile(const QString& path)
{
	auto loaded = std::make_shared<Audio>();
	std::shared_ptr<const std::vector<u8>> bytes; // Kept when the file can be exported as it is.
	Source source;
	std::string error;
	bool legacy = path.endsWith(".nwav", Qt::CaseInsensitive);
	try
	{
		bool ok;
		if (legacy)
			ok = readNwav1(readFile(path.toStdString()), *loaded, error);
		else
			ok = loadAudio(path.toStdString(), *loaded, source, error);
		if (ok && source.wav && checkPlayable(source.wavInfo).empty())
			bytes = std::make_shared<const std::vector<u8>>(std::move(source.bytes));
		if (!ok)
		{
			QMessageBox::warning(this, tr("Cannot open"), QString("%1\n\n%2").arg(path, QString::fromStdString(error)));
			return false;
		}
	}
	catch (const std::exception& e)
	{
		QMessageBox::warning(this, tr("Cannot open"), QString("%1\n\n%2").arg(path, e.what()));
		return false;
	}
	if (loaded->frames() == 0)
	{
		QMessageBox::warning(this, tr("Cannot open"), tr("%1\n\nThe file has no audio.").arg(path));
		return false;
	}

	QSettings().setValue("lastDir", QFileInfo(path).absolutePath());
	player->stop();
	audio = loaded;
	filePath = path;
	sourceFile = bytes;
	sourceIsAdpcm = bytes && source.wavInfo.tag == 0x11;
	events.clear();
	loop.reset();

	view->setAudio(audio);
	player->load(audio);
	loopPreviewAction->setChecked(true);
	player->setLoopEnabled(true);
	applyLoop(audio->loop);
	applyEvents(audio->events);
	exportAction->setEnabled(true);

	QString text = legacy ? tr("legacy NWAV") : source.wav ? describeFormat(source.wavInfo) : QString::fromStdString(source.codec).toUpper();
	text += QString(", %1 Hz, %2, %3 frames (%4)")
				.arg(audio->sampleRate)
				.arg(channelName(audio->channels.size()))
				.arg(audio->frames())
				.arg(formatFrame(audio->frames()));
	if (audio->channels.size() > 2)
		text += tr(". Only the first two channels play here.");
	if (source.wav)
	{
		for (const std::string& problem : checkPlayable(source.wavInfo))
			text += tr(". The player cannot stream it: the file %1").arg(QString::fromStdString(problem));
	}
	fileLabel->setText(text);
	setWindowTitle(QString("%1 - NitroWAV").arg(QFileInfo(path).fileName()));
	updatePlayActions();
	return true;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
	if (event->mimeData()->hasUrls())
		event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
	const QList<QUrl> urls = event->mimeData()->urls();
	if (!urls.isEmpty())
		openFile(urls.first().toLocalFile());
}

void MainWindow::exportFile()
{
	if (!audio)
		return;

	QFileInfo input(filePath);
	QString suggested = input.dir().filePath(input.completeBaseName() + "_ds.wav");
	ExportDialog dialog(this, audio->sampleRate, suggested, sourceFile != nullptr, sourceIsAdpcm);
	if (dialog.exec() != QDialog::Accepted)
		return;

	// The worker gets its own copy with the loop and events as edited here.
	auto source = std::make_shared<Audio>(*audio);
	source->loop = loop;
	source->events = events;
	ConvertOptions options = dialog.options();
	options.noLoop = !loop;
	const std::string outPath = dialog.path().toStdString();
	const bool keep = dialog.keepAudio();
	const auto original = sourceFile;
	const std::optional<Loop> editedLoop = loop;
	const std::vector<Event> editedEvents = events;

	// Not a QProgressDialog: its setValue runs the event loop, and the finished
	// handler below could delete it from inside that call.
	auto* progress = new QWidget(this, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
	progress->setWindowTitle(tr("Exporting"));
	progress->setWindowModality(Qt::WindowModal);
	auto* progressLayout = new QVBoxLayout(progress);
	progressLayout->addWidget(new QLabel(tr("Writing %1...").arg(QFileInfo(dialog.path()).fileName())));
	auto* bar = new QProgressBar;
	bar->setRange(0, 100);
	bar->setMinimumWidth(320);
	progressLayout->addWidget(bar);
	progress->show();

	struct Result
	{
		QString error;
		QStringList warnings;
		qint64 size = 0;
		double seconds = 0;
		double duration = 0;
		std::optional<double> snr;
		u32 rate = 0;
		size_t channels = 0;
		Format format = Format::Ima;
		std::optional<Loop> loop;
		bool kept = false;
	};
	auto result = std::make_shared<Result>();

	QThread* worker = QThread::create([=] {
		try
		{
			if (keep)
			{
				std::vector<u8> file = rewriteWav(*original, editedLoop, editedEvents);
				writeFile(outPath, file);

				// Read the result back for the summary.
				Audio written;
				WavInfo info;
				std::string error;
				if (!readWav(file, written, info, error))
					throw std::runtime_error(error);
				result->kept = true;
				result->format = info.tag == 0x11 ? Format::Ima : (info.bits == 8 ? Format::Pcm8 : Format::Pcm16);
				result->size = qint64(file.size());
				result->rate = info.sampleRate;
				result->channels = info.channels;
				result->duration = double(info.frames) / info.sampleRate;
				result->loop = info.loop;
				if (info.sampleRate > MaxUsefulRate)
					result->warnings << tr("The rate is above the %1 Hz the DS mixes at.").arg(MaxUsefulRate);
				return;
			}

			std::vector<std::string> warnings;
			Audio prepared = prepareAudio(std::move(*source), options, warnings);
			for (const std::string& w : warnings)
				result->warnings << QString::fromStdString(w);

			result->format = options.format.value_or(Format::Ima);
			int lastPercent = -1;
			auto start = std::chrono::steady_clock::now();
			std::vector<u8> file = writeWav(prepared, result->format, options.block, options.beam, [&](u32 done, u32 total) {
				int percent = total ? int(100.0 * done / total) : 100;
				if (percent == lastPercent)
					return;
				lastPercent = percent;
				QMetaObject::invokeMethod(bar, [bar, percent] { bar->setValue(percent); }, Qt::QueuedConnection);
			});
			result->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
			writeFile(outPath, file);

			result->size = qint64(file.size());
			result->rate = prepared.sampleRate;
			result->channels = prepared.channels.size();
			result->duration = double(prepared.frames()) / prepared.sampleRate;
			result->loop = prepared.loop;
			if (result->format == Format::Ima)
				result->snr = encodedSnr(prepared, file);
		}
		catch (const std::exception& e)
		{
			result->error = QString::fromUtf8(e.what());
		}
	});

	connect(worker, &QThread::finished, this, [=] {
		progress->close();
		progress->deleteLater();
		worker->deleteLater();

		if (!result->error.isEmpty())
		{
			QMessageBox::critical(this, tr("Export failed"), result->error);
			return;
		}

		static const char* names[] = { "PCM8", "PCM16", "IMA-ADPCM" };
		QString text = tr("%1\n\n%2, %3 Hz, %4, %5 s\n%6 MB, %7 kbit/s")
						   .arg(QString::fromStdString(outPath))
						   .arg(names[int(result->format)])
						   .arg(result->rate)
						   .arg(channelName(result->channels))
						   .arg(result->duration, 0, 'f', 1)
						   .arg(result->size / 1048576.0, 0, 'f', 2)
						   .arg(result->duration > 0 ? result->size * 8 / result->duration / 1000 : 0, 0, 'f', 0);
		if (result->loop)
			text += tr("\nLoop %1:%2").arg(result->loop->start).arg(result->loop->end);
		if (result->kept)
			text += tr("\nThe audio was kept as it was, not encoded again.");
		if (result->snr)
			text += tr("\nQuality %1 dB SNR (encoded in %2 s)").arg(*result->snr, 0, 'f', 2).arg(result->seconds, 0, 'f', 1);
		if (!result->warnings.isEmpty())
			text += "\n\n" + result->warnings.join("\n");
		QMessageBox::information(this, tr("Exported"), text);
	});
	worker->start();
}

// Playback ----------------------------------------------------------------------

void MainWindow::togglePlay()
{
	if (!audio || !player->isAvailable())
		return;
	if (player->state() == LoopPlayer::State::Playing)
		player->pause();
	else
		player->play();
	updatePlayActions();
}

void MainWindow::stop()
{
	player->stop();
	updatePlayActions();
}

void MainWindow::previewSeam()
{
	if (!loop)
	{
		statusBar()->showMessage(tr("Set a loop first."), 3000);
		return;
	}
	loopPreviewAction->setChecked(true);
	player->setLoopEnabled(true);
	player->previewSeam();
	updatePlayActions();
}

void MainWindow::updatePlayActions()
{
	const bool playing = player->state() == LoopPlayer::State::Playing;
	playAction->setText(playing ? tr("Pause") : tr("Play"));
	playAction->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));

	const bool usable = audio && player->isAvailable();
	playAction->setEnabled(usable);
	stopAction->setEnabled(usable);
	loopPreviewAction->setEnabled(usable);
	seamAction->setEnabled(usable);
}

void MainWindow::tick()
{
	if (!audio || !player->isAvailable())
		return;
	const LoopPlayer::State state = player->state();
	if (int(state) != lastState)
	{
		lastState = int(state);
		updatePlayActions();
	}

	const u32 frame = player->playhead();
	view->setPlayhead(frame, followAction->isChecked() && state == LoopPlayer::State::Playing);
	timeLabel->setText(formatFrame(frame));
}

// Loop and events ---------------------------------------------------------------

void MainWindow::applyLoop(std::optional<Loop> newLoop)
{
	loop = newLoop;
	updatingUi = true;

	const int frames = int(audio ? audio->frames() : 0);
	loopCheck->setChecked(loop.has_value());
	loopStartSpin->setEnabled(loop.has_value());
	loopEndSpin->setEnabled(loop.has_value());
	if (loop)
	{
		loopStartSpin->setRange(0, int(loop->end) - 1);
		loopEndSpin->setRange(int(loop->start) + 1, frames);
		loopStartSpin->setValue(int(loop->start));
		loopEndSpin->setValue(int(loop->end));
		u32 length = loop->end - loop->start;
		loopInfo->setText(tr("Length %1 frames (%2)").arg(length).arg(formatFrame(length)));
	}
	else
	{
		loopStartSpin->setRange(0, frames);
		loopEndSpin->setRange(0, frames);
		loopStartSpin->setValue(0);
		loopEndSpin->setValue(0);
		loopInfo->setText(tr("No loop: the file plays once."));
	}

	updatingUi = false;
	view->setLoop(loop);
	player->setLoop(loop);
}

void MainWindow::loopSpinChanged()
{
	if (updatingUi || !audio)
		return;
	applyLoop(Loop{ u32(loopStartSpin->value()), u32(loopEndSpin->value()) });
}

void MainWindow::loopCheckChanged(bool checked)
{
	if (!audio)
	{
		loopCheck->setChecked(false);
		return;
	}
	applyLoop(checked ? std::optional<Loop>(Loop{ 0, audio->frames() }) : std::nullopt);
}

void MainWindow::fillEventRow(int row)
{
	const Event& e = events[row];
	const bool was = updatingUi;
	updatingUi = true;
	eventTable->setItem(row, 0, new QTableWidgetItem(QString::number(e.id)));
	eventTable->setItem(row, 1, new QTableWidgetItem(QString::number(e.frame)));
	eventTable->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(e.label)));
	updatingUi = was;
}

void MainWindow::applyEvents(std::vector<Event> newEvents, int select)
{
	events = std::move(newEvents);
	updatingUi = true;
	eventTable->setRowCount(int(events.size()));
	updatingUi = false;
	for (int row = 0; row < int(events.size()); row++)
		fillEventRow(row);

	updatingUi = true;
	if (select >= 0 && select < int(events.size()))
		eventTable->selectRow(select);
	else
		eventTable->clearSelection();
	updatingUi = false;

	view->setEvents(events);
	view->setSelectedEvent(select);
}

void MainWindow::eventCellChanged(int row, int column)
{
	if (updatingUi || row < 0 || row >= int(events.size()))
		return;

	const QString text = eventTable->item(row, column)->text().trimmed();
	bool ok = true;
	if (column == 0)
	{
		u32 id = text.toUInt(&ok);
		if (ok)
			events[row].id = id;
	}
	else if (column == 1)
	{
		u32 frame = text.toUInt(&ok);
		ok = ok && frame < audio->frames();
		if (ok)
			events[row].frame = frame;
	}
	else
	{
		events[row].label = text.toStdString();
	}

	if (!ok)
		statusBar()->showMessage(tr("That is not a valid value."), 3000);
	fillEventRow(row);
	view->setEvents(events);

	if (column == 0 && ok)
	{
		for (int other = 0; other < int(events.size()); other++)
		{
			if (other != row && events[other].id == events[row].id)
			{
				statusBar()->showMessage(tr("Another event already uses ID %1.").arg(events[row].id), 4000);
				break;
			}
		}
	}
}

void MainWindow::addEventAt(u32 frame)
{
	if (!audio)
		return;
	u32 id = 1;
	while (std::any_of(events.begin(), events.end(), [id](const Event& e) { return e.id == id; }))
		id++;

	std::vector<Event> next = events;
	next.push_back({ id, std::min(frame, audio->frames() - 1), {} });
	applyEvents(std::move(next), int(events.size()));
}

void MainWindow::removeEvent(int index)
{
	if (index < 0 || index >= int(events.size()))
		return;
	std::vector<Event> next = events;
	next.erase(next.begin() + index);
	applyEvents(std::move(next));
}
