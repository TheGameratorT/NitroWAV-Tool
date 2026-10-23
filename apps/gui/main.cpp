// nwavgui: edits the loop and events of a WAV and previews the loop, for the NWAV player.

#include "MainWindow.hpp"

#include <QApplication>
#include <QIcon>

int main(int argc, char** argv)
{
	QApplication app(argc, argv);
	QApplication::setOrganizationName("TheGameratorT");
	QApplication::setApplicationName("nwavgui");

	QIcon icon;
	for (int size : { 16, 32, 48, 256 })
		icon.addFile(QString(":/nwav_%1.png").arg(size), QSize(size, size));
	QApplication::setWindowIcon(icon);

	MainWindow window;
	window.show();
	if (argc > 1)
		window.openFile(QString::fromLocal8Bit(argv[1]));
	return app.exec();
}
