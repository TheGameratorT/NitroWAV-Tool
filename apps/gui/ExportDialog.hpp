#pragma once

#include "convert.hpp"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;

// Asks how to write the WAV: the same choices the CLI's conv command takes.
class ExportDialog : public QDialog
{
	Q_OBJECT

public:
	// canKeep: the input can be written again as it is. keepByDefault: it starts ticked.
	ExportDialog(QWidget* parent, u32 inputRate, const QString& suggestedPath, bool canKeep, bool keepByDefault);

	// Write the audio as it is, with the edited loop and events, instead of converting it.
	bool keepAudio() const;

	// Everything but the loop and events, which come from the editor.
	ConvertOptions options() const;
	QString path() const;

	void accept() override;

private:
	void browse();
	void updateEnabled();

	QComboBox* format;
	QSpinBox* rate;
	QCheckBox* keep;
	QCheckBox* mono;
	QSpinBox* block;
	QSpinBox* beam;
	QLineEdit* pathEdit;
};
