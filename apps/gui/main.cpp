// nwavgui: edits the loop and events of a WAV and previews the loop, for the NWAV player.

#include "MainWindow.hpp"

#include <QApplication>

int main(int argc, char** argv)
{
	QApplication app(argc, argv);
	QApplication::setOrganizationName("TheGameratorT");
	QApplication::setApplicationName("nwavgui");

	MainWindow window;
	window.show();
	if (argc > 1)
		window.openFile(QString::fromLocal8Bit(argv[1]));
	return app.exec();
}
