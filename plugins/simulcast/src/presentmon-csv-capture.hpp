
#pragma once

#include "presentmon-csv-parser.hpp"

#include <obs.hpp> // logging and obs_data_t

#include <QProcess>

#include <memory>

class PresentMonCapture_accumulator;
class PresentMonCapture_state;

class PresentMonCapture : public QObject {
	Q_OBJECT
public:
	PresentMonCapture(QObject *parent);

	// Calling this will:
	//   - calculate summary statistics about data received so far,
	//     which will be written to the given obs_data_t.
	//   - discard the datapoints
	void summarizeAndReset(obs_data_t *dest);

private slots:
	void readProcessOutput_();

private:
	std::unique_ptr<QProcess> process_;
	std::unique_ptr<PresentMonCapture_state> state_;
	std::unique_ptr<PresentMonCapture_accumulator> accumulator_;
};
