// Drives the editor offscreen: opens a WAV, edits its loop and events with the
// mouse, and saves screenshots of the waveform to look at.
//
// NWAV_TEST_OUT names the folder for the screenshots (default: the temp folder).

#include "LoopPlayer.hpp"
#include "MainWindow.hpp"
#include "WaveformView.hpp"
#include "convert.hpp"

#include "ExportDialog.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QMessageBox>
#include <QTimer>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <cstring>
#include <random>

namespace
{
	constexpr u32 Rate = 44100;

	Audio makeAudio()
	{
		Audio audio;
		audio.sampleRate = Rate;
		audio.channels.resize(2);
		std::mt19937 rng(7);
		std::uniform_real_distribution<float> noise(-0.05f, 0.05f);
		for (u32 i = 0; i < Rate * 6; i++)
		{
			double t = double(i) / Rate;
			double envelope = std::abs(std::sin(t * 2.3));
			audio.channels[0].push_back(float(0.7 * envelope * std::sin(t * 2 * M_PI * 220)) + noise(rng));
			audio.channels[1].push_back(float(0.5 * std::sin(t * 2 * M_PI * 330 + 1)) * float(envelope) + noise(rng));
		}
		audio.loop = Loop{ Rate * 2, Rate * 5 };
		audio.events.push_back({ 3, Rate, "intro" });
		return audio;
	}
}

class GuiSmokeTest : public QObject
{
	Q_OBJECT

private slots:
	void peaksBoundTheSamples()
	{
		auto audio = std::make_shared<Audio>(makeAudio());
		WaveformPeaks peaks;
		peaks.build(audio);

		std::mt19937 rng(3);
		for (int n = 0; n < 2000; n++)
		{
			u64 first = rng() % (audio->frames() - 1);
			u64 count = 1 + (rng() % 40000);
			u64 last = std::min<u64>(first + count, audio->frames());
			const auto& src = audio->channels[n & 1];
			auto [lo, hi] = std::minmax_element(src.begin() + first, src.begin() + last);

			float plo, phi;
			peaks.range(n & 1, first, last, plo, phi);
			QVERIFY2(plo <= *lo + 1e-6f && phi >= *hi - 1e-6f, "the peaks cover every sample");

			if (count < 256)
			{
				QCOMPARE(plo, *lo);
				QCOMPARE(phi, *hi);
			}
			else
			{
				// Wide ranges may reach past the ends, by up to a quarter of the range each way.
				u64 a = first > count / 2 ? first - count / 2 : 0;
				u64 b = std::min<u64>(last + count / 2, audio->frames());
				auto [wlo, whi] = std::minmax_element(src.begin() + a, src.begin() + b);
				QVERIFY2(plo >= *wlo - 1e-6f && phi <= *whi + 1e-6f, "the peaks do not reach far past the range");
			}
		}
	}

	void editsLoopAndEvents()
	{
		QTemporaryDir dir;
		QString path = dir.filePath("test.wav");
		Audio audio = makeAudio();
		writeFile(path.toStdString(), writeWav(audio, Format::Pcm16, 512));

		QString out = qEnvironmentVariable("NWAV_TEST_OUT", QDir::tempPath());
		QDir().mkpath(out);

		MainWindow window;
		window.resize(1200, 600);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		QVERIFY(window.openFile(path));

		WaveformView* view = window.waveform();
		QWidget* viewport = view->viewport();
		const u32 total = audio.frames();

		// What the file held.
		QVERIFY(window.currentLoop().has_value());
		QCOMPARE(window.currentLoop()->start, Rate * 2);
		QCOMPARE(window.currentLoop()->end, Rate * 5);
		QCOMPARE(int(window.currentEvents().size()), 1);
		QCOMPARE(window.currentEvents()[0].id, 3u);
		QVERIFY(viewport->grab().save(out + "/1_fit.png"));
		QVERIFY(window.grab().save(out + "/0_window.png"));

		// Drag the loop start by 100 pixels. It lands within the snap radius of there.
		const double fpp = double(total) / viewport->width();
		const int y = viewport->height() / 2;
		const int startX = int(std::round(Rate * 2 / fpp));
		QTest::mousePress(viewport, Qt::LeftButton, {}, QPoint(startX, y));
		QTest::mouseMove(viewport, QPoint(startX + 50, y));
		QTest::mouseMove(viewport, QPoint(startX + 100, y));
		QTest::mouseRelease(viewport, Qt::LeftButton, {}, QPoint(startX + 100, y));
		const double expected = Rate * 2 + 100 * fpp;
		QVERIFY(window.currentLoop().has_value());
		QVERIFY2(std::abs(double(window.currentLoop()->start) - expected) <= std::max(32.0, 8 * fpp) + 1,
				 qPrintable(QString("start %1, expected near %2").arg(window.currentLoop()->start).arg(expected)));
		QCOMPARE(window.currentLoop()->end, Rate * 5);

		// A handle cannot cross the other one.
		const int endX = int(std::round(Rate * 5 / fpp));
		QTest::mousePress(viewport, Qt::LeftButton, {}, QPoint(endX, y));
		QTest::mouseMove(viewport, QPoint(0, y));
		QTest::mouseRelease(viewport, Qt::LeftButton, {}, QPoint(0, y));
		QVERIFY(window.currentLoop()->end > window.currentLoop()->start);

		// Put the end back.
		view->setLoopEndAt(Rate * 5);
		QCOMPARE(window.currentLoop()->end, Rate * 5);

		// Double click adds an event with a fresh ID.
		QTest::mouseDClick(viewport, Qt::LeftButton, {}, QPoint(viewport->width() / 2, y));
		QCOMPARE(int(window.currentEvents().size()), 2);
		QCOMPARE(window.currentEvents()[1].id, 1u);
		QVERIFY(std::abs(double(window.currentEvents()[1].frame) - total / 2.0) <= fpp + 1);

		// Drag it, then delete it with the keyboard.
		const int eventX = int(std::round(window.currentEvents()[1].frame / fpp));
		QTest::mousePress(viewport, Qt::LeftButton, {}, QPoint(eventX, y));
		QTest::mouseMove(viewport, QPoint(eventX + 40, y));
		QTest::mouseRelease(viewport, Qt::LeftButton, {}, QPoint(eventX + 40, y));
		QVERIFY(std::abs(double(window.currentEvents()[1].frame) - (total / 2.0 + 40 * fpp)) <= fpp + 1);
		QVERIFY(viewport->grab().save(out + "/2_edited.png"));
		QTest::keyClick(view, Qt::Key_Delete);
		QCOMPARE(int(window.currentEvents().size()), 1);

		// Ctrl+wheel zooms around the pointer, the wheel pans.
		const u32 before = view->frameAtX(viewport->width()) - view->frameAtX(0);
		const u32 underPointer = view->frameAtX(300);
		QWheelEvent zoom(QPointF(300, y), viewport->mapToGlobal(QPointF(300, y)), {}, QPoint(0, 120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
		QApplication::sendEvent(viewport, &zoom);
		const u32 after = view->frameAtX(viewport->width()) - view->frameAtX(0);
		QVERIFY2(after < before, "zooming in shows fewer frames");
		QVERIFY2(std::abs(double(view->frameAtX(300)) - underPointer) <= before / double(viewport->width()) * 1.5,
				 "the frame under the pointer stays put");

		const u32 firstBefore = view->frameAtX(0);
		QWheelEvent pan(QPointF(300, y), viewport->mapToGlobal(QPointF(300, y)), {}, QPoint(0, -120), Qt::NoButton, {}, Qt::NoScrollPhase, false);
		QApplication::sendEvent(viewport, &pan);
		QVERIFY2(view->frameAtX(0) > firstBefore, "scrolling pans toward the end");

		view->zoomToLoop();
		QVERIFY(viewport->grab().save(out + "/3_loop.png"));

		// Down to the samples themselves, around the loop start.
		view->zoomToFit();
		view->setPlayhead(window.currentLoop()->start, false);
		view->zoomBy(4000, int(std::round(window.currentLoop()->start / fpp)));
		QVERIFY(viewport->grab().save(out + "/4_samples.png"));
	}

	void exportsWhatWasEdited()
	{
		QTemporaryDir dir;
		QString path = dir.filePath("test.wav");
		Audio audio = makeAudio();
		writeFile(path.toStdString(), writeWav(audio, Format::Pcm16, 512));

		MainWindow window;
		QVERIFY(window.openFile(path));
		window.waveform()->setLoopStartAt(Rate);
		window.waveform()->setLoopEndAt(Rate * 4);
		window.waveform()->eventAddRequested(Rate * 3);
		QCOMPARE(int(window.currentEvents().size()), 2);

		// Accept the export dialog, then the message that says it is done.
		QString result;
		QTimer clicker;
		connect(&clicker, &QTimer::timeout, [&] {
			QWidget* modal = QApplication::activeModalWidget();
			if (auto* box = qobject_cast<QMessageBox*>(modal))
			{
				result = box->text();
				box->accept();
			}
			else if (auto* dialog = qobject_cast<ExportDialog*>(modal))
			{
				dialog->accept();
			}
		});
		clicker.start(50);

		QAction* exportAction = nullptr;
		for (QAction* action : window.findChildren<QAction*>())
		{
			if (action->text().contains("Export"))
				exportAction = action;
		}
		QVERIFY(exportAction);
		exportAction->trigger();
		QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 15000);
		clicker.stop();
		QVERIFY2(result.contains("IMA-ADPCM"), qPrintable(result));

		// The suggested name sits next to the input. It holds the edited loop and events.
		Audio written;
		WavInfo info;
		std::string error;
		QVERIFY(readWav(readFile(dir.filePath("test_ds.wav").toStdString()), written, info, error));
		QVERIFY(info.loop.has_value());
		QCOMPARE(info.tag, u16(0x11));
		// The dialog offers the DS rate for a higher input, and the loop and events scale with it.
		QCOMPARE(info.sampleRate, MaxUsefulRate);
		const auto scaled = [](u32 frame) { return u32(std::llround(double(frame) * MaxUsefulRate / Rate)); };
		QCOMPARE(info.loop->start, scaled(Rate));
		QCOMPARE(info.loop->end, scaled(Rate * 4));
		QCOMPARE(int(info.events.size()), 2);
		QCOMPARE(info.events[0].id, 3u);
		QCOMPARE(info.events[1].frame, scaled(Rate * 3));
		QVERIFY(checkPlayable(info).empty());
	}

	void exportsAdpcmWithoutEncodingAgain()
	{
		QTemporaryDir dir;
		QString path = dir.filePath("song.wav");
		std::vector<u8> original = writeWav(makeAudio(), Format::Ima, 512, 1);
		writeFile(path.toStdString(), original);

		MainWindow window;
		QVERIFY(window.openFile(path));
		window.waveform()->setLoopStartAt(Rate);
		window.waveform()->setLoopEndAt(Rate * 5);

		// The dialog offers to keep the audio, and starts that way for ADPCM.
		bool ticked = false, enabled = false, formatLocked = false;
		QString result;
		QTimer clicker;
		connect(&clicker, &QTimer::timeout, [&] {
			QWidget* modal = QApplication::activeModalWidget();
			if (auto* box = qobject_cast<QMessageBox*>(modal))
			{
				result = box->text();
				box->accept();
			}
			else if (auto* dialog = qobject_cast<ExportDialog*>(modal))
			{
				ticked = dialog->keepAudio();
				for (QCheckBox* check : dialog->findChildren<QCheckBox*>())
				{
					if (check->text().startsWith("Keep"))
						enabled = check->isEnabled();
				}
				for (QComboBox* combo : dialog->findChildren<QComboBox*>())
					formatLocked = !combo->isEnabled();
				dialog->accept();
			}
		});
		clicker.start(50);

		QAction* exportAction = nullptr;
		for (QAction* action : window.findChildren<QAction*>())
		{
			if (action->text().contains("Export"))
				exportAction = action;
		}
		QVERIFY(exportAction);
		exportAction->trigger();
		QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 15000);
		clicker.stop();
		QVERIFY(ticked && enabled && formatLocked);
		QVERIFY2(result.contains("kept as it was"), qPrintable(result));

		// The audio data is the one that was opened. Only the loop and events changed.
		std::vector<u8> written = readFile(dir.filePath("song_ds.wav").toStdString());
		const auto dataChunk = [](const std::vector<u8>& file) {
			for (size_t pos = 12; pos + 8 <= file.size();)
			{
				u32 size = u32(file[pos + 4]) | u32(file[pos + 5]) << 8 | u32(file[pos + 6]) << 16 | u32(file[pos + 7]) << 24;
				if (std::memcmp(&file[pos], "data", 4) == 0)
					return std::vector<u8>(file.begin() + pos + 8, file.begin() + pos + 8 + size);
				pos += 8 + size + (size & 1);
			}
			return std::vector<u8>();
		};
		std::vector<u8> before = dataChunk(original), after = dataChunk(written);
		QVERIFY(!after.empty());
		QVERIFY2(after.size() <= before.size(), "the tail after the loop end is cut");
		QVERIFY(std::equal(after.begin(), after.end(), before.begin()));

		Audio audio;
		WavInfo info;
		std::string error;
		QVERIFY(readWav(written, audio, info, error));
		QCOMPARE(info.tag, u16(0x11));
		QVERIFY(info.loop.has_value());
		QCOMPARE(info.loop->start, Rate);
		QCOMPARE(info.loop->end, Rate * 5);
	}

	void playerFollowsTheSeek()
	{
		MainWindow window;
		QTemporaryDir dir;
		QString path = dir.filePath("test.wav");
		Audio audio = makeAudio();
		writeFile(path.toStdString(), writeWav(audio, Format::Pcm16, 512));
		QVERIFY(window.openFile(path));

		LoopPlayer* player = window.audioPlayer();
		if (!player->isAvailable())
			QSKIP("no OpenAL device");

		player->seek(Rate * 3);
		QCOMPARE(player->playhead(), Rate * 3);
		player->play();
		QTRY_VERIFY(player->playhead() > Rate * 3);
		player->pause();
		QCOMPARE(int(player->state()), int(LoopPlayer::State::Paused));
		u32 paused = player->playhead();
		QTest::qWait(100);
		QCOMPARE(player->playhead(), paused);
		player->stop();
		QCOMPARE(player->playhead(), 0u);
	}
};

QTEST_MAIN(GuiSmokeTest)
#include "gui_smoke_test.moc"
