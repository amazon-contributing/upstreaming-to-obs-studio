
#pragma once

#include "presentmon-csv-parser.hpp"

#include <obs.hpp> // logging and obs_data_t

#include <QMutex>
#include <QMutexLocker>
#include <QProcess>

#include <memory>
#include <unordered_map>

class PresentMonCapture_state {
public:
	bool alreadyErrored_ = false;
	uint64_t lineNumber_ = 0;
	std::vector<const char *> v_;
	ParsedCsvRow row_;
	CsvRowParser parser_;
};

class PresentMonCapture_accumulator {
public:
	QMutex mutex; // XXX I have not thought out the concurrency here
	std::unordered_map<std::string, std::vector<ParsedCsvRow>> per_app_rows_;

	void frame(const ParsedCsvRow &row);
	void summarizeAndReset(obs_data_t *dest);

private:
	// You need to hold the mutex before calling this
	void trimRows(std::vector<ParsedCsvRow> &rows, const QMutexLocker<QMutex> &ensure_lock);
};

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
	std::unique_ptr<QProcess> process_;
	std::unique_ptr<PresentMonCapture_state> state_;
	std::unique_ptr<PresentMonCapture_accumulator> accumulator_;
};
