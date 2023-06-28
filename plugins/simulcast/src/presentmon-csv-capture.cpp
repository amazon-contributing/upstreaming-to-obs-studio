#include "presentmon-csv-capture.hpp"

#include <obs.hpp>   // logging

#include <QProcess>

PresentMonCapture::PresentMonCapture(QObject* parent) : QObject(parent)
{
	testCsvParser();

	process_ = new QProcess(this);
	state_ = new state_t;

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
{}

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
