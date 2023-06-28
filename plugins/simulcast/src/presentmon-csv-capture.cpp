#include "presentmon-csv-capture.hpp"

#include <QProcess>
#include <QMutex>

#define DISCARD_SAMPLES_BEYOND \
	144 * 60 * 2 // 144fps, one minute, times two for safety

class PresentMonCapture_state {
public:
	PresentMonCapture_state() : alreadyErrored_(false), lineNumber_(0) {}

	bool alreadyErrored_;
	uint64_t lineNumber_;
	std::vector<const char *> v_;
	ParsedCsvRow row_;
	CsvRowParser parser_;
};

class PresentMonCapture_accumulator {
public:
	QMutex mutex; // XXX I have not thought out the concurrency here
	std::vector<ParsedCsvRow> rows_;

	void frame(const ParsedCsvRow& row) {
		mutex.lock();

		// don't do this every time, it'll be slow
		// this is just a safety check so we don't allocate memory forever
		if (rows_.size() > 3 * DISCARD_SAMPLES_BEYOND)
			trimRows();

		rows_.push_back(row);
		mutex.unlock();
	}

	void summarizeAndReset(obs_data_t* dest) {
		mutex.lock();
		trimRows();

		double fps = 10.0; // XXX actually calculate

		if (rows_.size() > 0) {
			// delete all but the most recently received data point
			// XXX is this just a very convoluated rows_.erase(rows_.begin(), rows_.end()-1) ?
			*rows_.begin() = *rows_.rbegin();
			rows_.erase(rows_.begin() + 1, rows_.end());
		}
		mutex.unlock();

		obs_data_set_double(dest, "fps", fps);
	}

private:
	// You need to hold the mutex before calling this
	void trimRows()
	{
		if (rows_.size() > DISCARD_SAMPLES_BEYOND) {
			rows_.erase(rows_.begin(),
				    rows_.begin() + (rows_.size() - DISCARD_SAMPLES_BEYOND));
		}
	}
};


PresentMonCapture::PresentMonCapture(QObject* parent) : QObject(parent)
{
	testCsvParser();

	process_ = new QProcess(this);
	state_ = new PresentMonCapture_state;
	accumulator_ = new PresentMonCapture_accumulator;

	// Log a bunch of QProcess signals
	QObject::connect(process_, &QProcess::started, []() {
		blog(LOG_INFO, "QProcess::started received");
	});
	QObject::connect(
		process_, &QProcess::errorOccurred,
		[](QProcess::ProcessError error) {
			blog(LOG_INFO,
			     QString("QProcess::errorOccurred(error=%1) received")
				     .arg(error)
				     .toUtf8());
		});
	QObject::connect(
		process_, &QProcess::stateChanged,
		[](QProcess::ProcessState newState) {
			blog(LOG_INFO,
			     QString("QProcess::stateChanged(newState=%1) received")
				     .arg(newState)
				     .toUtf8());
		});
	QObject::connect(
		process_, &QProcess::finished,
		[](int exitCode, QProcess::ExitStatus exitStatus) {
			blog(LOG_INFO,
			     QString("QProcess::finished(exitCode=%1, exitStatus=%2) received")
				     .arg(exitCode)
				     .arg(exitStatus)
				     .toUtf8());
		});

	QObject::connect(
		process_, &QProcess::readyReadStandardError, [this]() {
			QByteArray data;
			while ((data = this->process_->readAllStandardError()).size()) {
				blog(LOG_INFO, "StdErr: %s", data.constData());
			}
		});

	// Process the CSV as it appears on stdout
	// This will be better as a class member than a closure, because we have
	// state, and autoformat at 80 columns with size-8 tabs is really yucking
	// things up!

	QObject::connect(process_, &QProcess::readyReadStandardOutput, this,
			 &PresentMonCapture::readProcessOutput_);

	// Start the process
	QStringList args({"-output_stdout", "-stop_existing_session"});
	process_->start(
		"c:\\obsdev\\PresentMon\\build\\Release\\PresentMon-dev-x64.exe",
		args, QIODeviceBase::ReadWrite);
}

PresentMonCapture::~PresentMonCapture()
{
	delete accumulator_;
	delete state_;
}

void PresentMonCapture::summarizeAndReset(obs_data_t* dest) {
	accumulator_->summarizeAndReset(dest);
}

void PresentMonCapture::readProcessOutput_()
{
	char buf[1024];
	if (state_->alreadyErrored_) {
		qint64 n = process_->readLine(buf, sizeof(buf));
		blog(LOG_INFO, "POST-ERROR line %d: %s", state_->lineNumber_,
		     buf);
		state_->lineNumber_++;
	}

	while (!state_->alreadyErrored_ && process_->canReadLine()) {
		qint64 n = process_->readLine(buf, sizeof(buf));

		if (n < 1 || n >= sizeof(buf) - 1) {
			// XXX emit error
			state_->alreadyErrored_ = true;
		} else {
			if (buf[n - 1] == '\n') {
				//blog(LOG_INFO,
				//"REPLACING NEWLINE WITH NEW NULL");
				buf[n - 1] = '\0';
			}

			if (state_->lineNumber_ < 10) {
				blog(LOG_INFO, "Got line %d: %s",
				     state_->lineNumber_, buf);
			}

			state_->v_.clear();
			SplitCsvRow(state_->v_, buf);

			if (state_->lineNumber_ == 0) {
				state_->alreadyErrored_ =
					!state_->parser_.headerRow(state_->v_);
			} else {
				state_->alreadyErrored_ =
					!state_->parser_.dataRow(state_->v_,
								 &state_->row_);

				if (!state_->alreadyErrored_ &&
				    state_->lineNumber_ < 10) {
					accumulator_->frame(state_->row_);
					blog(LOG_INFO,
					     QString("afTest: csv line %1 Application=%3, ProcessID=%4, TimeInSeconds=%5, msBetweenPresents=%6")
					     .arg(state_->lineNumber_)
					     .arg(state_->row_
							  .Application)
					     .arg(state_->row_.ProcessID)
					     .arg(state_->row_
							  .TimeInSeconds)
					     .arg(state_->row_
							  .msBetweenPresents)
					     .toUtf8());
				}
			}
			state_->lineNumber_++;
		}
	}
}
