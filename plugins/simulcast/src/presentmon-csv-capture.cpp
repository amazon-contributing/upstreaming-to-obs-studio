#include "presentmon-csv-capture.hpp"
#include "presentmon-csv-parser.hpp"

#include <obs.hpp>   // logging

#include <QProcess>

void StartPresentMon()
{
	testCsvParser();

	// frame stuff
	// XXX where to put this?
	QProcess *process = new QProcess();

	// Log a bunch of QProcess signals
	QObject::connect(process, &QProcess::started, []() {
		blog(LOG_INFO, "QProcess::started received");
	});
	QObject::connect(
		process, &QProcess::errorOccurred,
		[](QProcess::ProcessError error) {
			blog(LOG_INFO,
			     QString("QProcess::errorOccurred(error=%1) received")
				     .arg(error)
				     .toUtf8());
		});
	QObject::connect(
		process, &QProcess::stateChanged,
		[](QProcess::ProcessState newState) {
			blog(LOG_INFO,
			     QString("QProcess::stateChanged(newState=%1) received")
				     .arg(newState)
				     .toUtf8());
		});
	QObject::connect(
		process, &QProcess::finished,
		[](int exitCode, QProcess::ExitStatus exitStatus) {
			blog(LOG_INFO,
			     QString("QProcess::finished(exitCode=%1, exitStatus=%2) received")
				     .arg(exitCode)
				     .arg(exitStatus)
				     .toUtf8());
		});

	QObject::connect(
		process, &QProcess::readyReadStandardError, [process]() {
			QByteArray data;
			while ((data = process->readAllStandardError()).size()) {
				blog(LOG_INFO, "StdErr: %s", data.constData());
			}
		});

	// Process the CSV as it appears on stdout
	// This will be better as a class member than a closure, because we have
	// state, and autoformat at 80 columns with size-8 tabs is really yucking
	// things up!
	struct state_t {
		state_t() : alreadyErrored_(false), lineNumber_(0) {}

		bool alreadyErrored_;
		uint64_t lineNumber_;
		std::vector<const char *> v_;
		ParsedCsvRow row_;
		CsvRowParser parser_;
	};
	state_t *state = new state_t;
	QObject::connect(process, &QProcess::readyReadStandardOutput, [state, process]() {
		char buf[1024];
		if (state->alreadyErrored_) {
			qint64 n = process->readLine(buf, sizeof(buf));
			blog(LOG_INFO, "POST-ERROR line %d: %s",
			     state->lineNumber_, buf);
			state->lineNumber_++;
		}
		while (!state->alreadyErrored_ && process->canReadLine()) {
			qint64 n = process->readLine(buf, sizeof(buf));

			if (n < 1 || n >= sizeof(buf) - 1) {
				// XXX emit error
				state->alreadyErrored_ = true;
			} else {
				if (buf[n - 1] == '\n') {
					//blog(LOG_INFO,
					//"REPLACING NEWLINE WITH NEW NULL");
					buf[n - 1] = '\0';
				}

				if (state->lineNumber_ < 10) {
					blog(LOG_INFO, "Got line %d: %s",
					     state->lineNumber_, buf);
				}

				state->v_.clear();
				SplitCsvRow(state->v_, buf);

				if (state->lineNumber_ == 0) {
					state->alreadyErrored_ =
						!state->parser_.headerRow(
							state->v_);
				} else {
					state->alreadyErrored_ =
						!state->parser_.dataRow(
							state->v_,
							&state->row_);

					if (!state->alreadyErrored_ &&
					    state->lineNumber_ < 10) {
						blog(LOG_INFO,
						     QString("afTest: csv line %1 Application=%3, ProcessID=%4, TimeInSeconds=%5, msBetweenPresents=%6")
							     .arg(state->lineNumber_)
							     .arg(state->row_
									  .Application)
							     .arg(state->row_
									  .ProcessID)
							     .arg(state->row_
									  .TimeInSeconds)
							     .arg(state->row_
									  .msBetweenPresents)
							     .toUtf8());
					}
				}
				state->lineNumber_++;
			}
		}
	});

	// Start the process
	QStringList args({"-output_stdout", "-stop_existing_session"});
	process->start(
		"c:\\obsdev\\PresentMon\\build\\Release\\PresentMon-dev-x64.exe",
		args, QIODeviceBase::ReadWrite);
}
