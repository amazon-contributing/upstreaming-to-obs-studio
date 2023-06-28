
#pragma once

#include "presentmon-csv-parser.hpp"

#include <obs.hpp> // logging and obs_data_t

#include <QProcess>

class PresentMonCapture_accumulator;
class PresentMonCapture_state;

class PresentMonCapture : public QObject {
	Q_OBJECT
public:
	PresentMonCapture(QObject *parent);
	~PresentMonCapture();

	// Calling this will:
	//   - calculate summary statistics about data received so far,
	//     which will be written to the given obs_data_t.
	//   - discard the datapoints
	void summarizeAndReset(obs_data_t *dest);

private slots:
	void readProcessOutput_();

private:
	QProcess* process_;
	PresentMonCapture_state *state_;
	PresentMonCapture_accumulator *accumulator_;
};

