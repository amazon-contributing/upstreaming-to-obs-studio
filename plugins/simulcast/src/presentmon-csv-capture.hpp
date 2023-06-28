
#pragma once

#include "presentmon-csv-parser.hpp"

#include <QProcess>

struct state_t {
	state_t() : alreadyErrored_(false), lineNumber_(0) {}

	bool alreadyErrored_;
	uint64_t lineNumber_;
	std::vector<const char *> v_;
	ParsedCsvRow row_;
	CsvRowParser parser_;
};

class PresentMonCapture : public QObject {
	Q_OBJECT
public:
	PresentMonCapture(QObject *parent);
	~PresentMonCapture();

private slots:
	void readProcessOutput_();

private:
	QProcess* process_;
	state_t* state_;
};

