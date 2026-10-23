#include "ExportDialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>

ExportDialog::ExportDialog(QWidget* parent, u32 inputRate, const QString& suggestedPath, bool canKeep, bool keepByDefault) : QDialog(parent)
{
	setWindowTitle(tr("Export WAV"));
	setMinimumWidth(460);

	format = new QComboBox;
	format->addItem(tr("IMA-ADPCM (a quarter of PCM16)"), int(Format::Ima));
	format->addItem(tr("PCM8"), int(Format::Pcm8));
	format->addItem(tr("PCM16"), int(Format::Pcm16));

	rate = new QSpinBox;
	rate->setRange(0, 192000);
	rate->setSuffix(tr(" Hz"));
	rate->setSpecialValueText(tr("Keep (%1 Hz)").arg(inputRate));
	rate->setValue(inputRate > MaxUsefulRate ? MaxUsefulRate : 0);
	auto* ds = new QPushButton(tr("%1 Hz").arg(MaxUsefulRate));
	ds->setToolTip(tr("The rate the DS mixer outputs at. Anything higher only costs ROM space."));
	connect(ds, &QPushButton::clicked, this, [this] { rate->setValue(MaxUsefulRate); });
	auto* rateRow = new QHBoxLayout;
	rateRow->addWidget(rate, 1);
	rateRow->addWidget(ds);

	mono = new QCheckBox(tr("Mix down to mono"));

	block = new QSpinBox;
	block->setRange(8, 4096);
	block->setSingleStep(4);
	block->setValue(512);
	block->setSuffix(tr(" bytes per channel"));

	beam = new QSpinBox;
	beam->setRange(1, 256);
	beam->setValue(DefaultBeam);
	beam->setToolTip(tr("How many candidate encodings the IMA-ADPCM search keeps per sample.\n"
						"Higher is cleaner and slower. 1 is a greedy encoder."));

	pathEdit = new QLineEdit(suggestedPath);
	auto* browseButton = new QPushButton(tr("Browse..."));
	connect(browseButton, &QPushButton::clicked, this, &ExportDialog::browse);
	auto* pathRow = new QHBoxLayout;
	pathRow->addWidget(pathEdit, 1);
	pathRow->addWidget(browseButton);

	keep = new QCheckBox(tr("Keep the audio as it is (no re-encoding)"));
	keep->setToolTip(tr("Only the loop and events change. Nothing is decoded or encoded again, so IMA-ADPCM\n"
						"keeps its quality."));
	keep->setEnabled(canKeep);
	keep->setChecked(canKeep && keepByDefault);
	if (!canKeep)
		keep->setText(tr("Keep the audio as it is (not possible for this file)"));

	trim = new QCheckBox(tr("Cut the audio after the loop end"));
	trim->setToolTip(tr("Nothing after the loop end is ever played, so cutting it saves ROM space.\n"
						"Untick it to keep the whole recording."));
	trim->setChecked(true);

	auto* form = new QFormLayout;
	form->addRow(keep);
	form->addRow(trim);
	form->addRow(tr("Format"), format);
	form->addRow(tr("Sample rate"), rateRow);
	form->addRow(QString(), mono);
	form->addRow(tr("ADPCM block"), block);
	form->addRow(tr("ADPCM search width"), beam);
	form->addRow(tr("Save as"), pathRow);

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	buttons->button(QDialogButtonBox::Ok)->setText(tr("Export"));
	connect(buttons, &QDialogButtonBox::accepted, this, &ExportDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &ExportDialog::reject);

	auto* layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addWidget(buttons);

	connect(format, &QComboBox::currentIndexChanged, this, &ExportDialog::updateEnabled);
	connect(keep, &QCheckBox::toggled, this, &ExportDialog::updateEnabled);
	updateEnabled();
}

ConvertOptions ExportDialog::options() const
{
	ConvertOptions o;
	o.format = Format(format->currentData().toInt());
	o.rate = u32(rate->value());
	o.mono = mono->isChecked();
	o.block = u32(block->value() / 4 * 4);
	o.beam = u32(beam->value());
	o.trim = trim->isChecked();
	return o;
}

bool ExportDialog::keepAudio() const
{
	return keep->isChecked();
}

bool ExportDialog::trimAfterLoop() const
{
	return trim->isChecked();
}

QString ExportDialog::path() const
{
	return pathEdit->text();
}

void ExportDialog::accept()
{
	if (pathEdit->text().isEmpty())
	{
		browse();
		if (pathEdit->text().isEmpty())
			return;
	}
	QDialog::accept();
}

void ExportDialog::browse()
{
	QString chosen = QFileDialog::getSaveFileName(this, tr("Export WAV"), pathEdit->text(), tr("WAV files (*.wav)"));
	if (!chosen.isEmpty())
		pathEdit->setText(chosen);
}

void ExportDialog::updateEnabled()
{
	const bool convert = !keep->isChecked();
	const bool ima = Format(format->currentData().toInt()) == Format::Ima;
	format->setEnabled(convert);
	rate->setEnabled(convert);
	mono->setEnabled(convert);
	block->setEnabled(convert && ima);
	beam->setEnabled(convert && ima);
}
