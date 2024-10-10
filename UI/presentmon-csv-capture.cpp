#include "presentmon-csv-capture.hpp"

#include <QProcess>
#include <QMutex>
#include <QMutexLocker>

#include <unordered_map>
#include <cinttypes>

#include "platform.hpp"

#define PRESENTMON_NAME "PresentMon-1.8.0-x64.exe"

#define DISCARD_SAMPLES_BEYOND 144 * 60 * 2 // 144fps, one minute, times two for safety

#define PRESENTMON_SESSION_NAME "PresentMon_OBS_IVS_MultitrackVideo_Tech_Preview"

void PresentMonCapture_accumulator::frame(const ParsedCsvRow &row)
{
	QMutexLocker locked(&mutex);

	auto &app_rows = per_app_rows_[row.Application];

	// don't do this every time, it'll be slow
	// this is just a safety check so we don't allocate memory forever
	if (app_rows.size() > 3 * DISCARD_SAMPLES_BEYOND)
		trimRows(app_rows, locked);

	app_rows.push_back(row);
}

void PresentMonCapture_accumulator::summarizeAndReset(obs_data_t *dest)
{
	std::multimap<float, std::string, std::greater<float>> apps_fps_order;

	{
		QMutexLocker locked(&mutex);

		for (auto &app_rows : per_app_rows_) {
			if (!(app_rows.second.size() >= 2 &&
			      app_rows.second.rbegin()->TimeInSeconds > app_rows.second[0].TimeInSeconds))
				continue;

			// before calculating, throw away extraneous rows
			trimRows(app_rows.second, locked);

			const size_t n = app_rows.second.size();
			double allButFirstBetweenPresents = -app_rows.second[0].msBetweenPresents;
			for (const auto &p : app_rows.second)
				allButFirstBetweenPresents += p.msBetweenPresents;
			allButFirstBetweenPresents /= 1000.0;

			blog(LOG_INFO, "frame timing, all but first msBetweenPresents: %f", allButFirstBetweenPresents);
			blog(LOG_INFO, "frame timing, time from first to last: %f",
			     app_rows.second[n - 1].TimeInSeconds - app_rows.second[0].TimeInSeconds);
			auto fps = (n - 1) / (app_rows.second[n - 1].TimeInSeconds - app_rows.second[0].TimeInSeconds);

			// after calculating, throw away all but the last row
			if (n > 1) {
				app_rows.second.erase(app_rows.second.begin(), app_rows.second.begin() + n - 1);
			}

			apps_fps_order.insert(std::make_pair(fps, app_rows.first));
		}
	}

	// all games get serialized into this array
	OBSDataAutoRelease data = obs_data_create();
	OBSDataArrayAutoRelease array = obs_data_array_create();
	obs_data_set_array(data, "games", array);
	for (auto &p : apps_fps_order) {
		OBSDataAutoRelease game = obs_data_create();
		obs_data_set_string(game, "game", p.second.c_str());
		obs_data_set_double(game, "fps", p.first);
		obs_data_array_push_back(array, game);
	}
	obs_data_set_string(dest, "games", obs_data_get_json(data));

	// we report up to two highest-fps games separately
	auto p = apps_fps_order.cbegin();
	if (p != apps_fps_order.cend()) {
		obs_data_set_string(dest, "game0_name", p->second.c_str());
		obs_data_set_double(dest, "game0_fps", p->first);
		++p;
	}
	if (p != apps_fps_order.cend()) {
		obs_data_set_string(dest, "game1_name", p->second.c_str());
		obs_data_set_double(dest, "game1_fps", p->first);
		++p;
	}
}

// You need to hold the mutex before calling this
void PresentMonCapture_accumulator::trimRows(std::vector<ParsedCsvRow> &app_rows, const QMutexLocker<QMutex> &)
{
	if (app_rows.size() > DISCARD_SAMPLES_BEYOND) {
		app_rows.erase(app_rows.begin(), app_rows.begin() + (app_rows.size() - DISCARD_SAMPLES_BEYOND));
	}
}

PresentMonCapture::PresentMonCapture(QObject *parent) : QObject(parent)
{
	testCsvParser();

	process_.reset(new QProcess(this));
	state_.reset(new PresentMonCapture_state);
	accumulator_.reset(new PresentMonCapture_accumulator);

	// Log a bunch of QProcess signals
	QObject::connect(process_.get(), &QProcess::started, []() { blog(LOG_INFO, "QProcess::started received"); });
	QObject::connect(process_.get(), &QProcess::errorOccurred, [](QProcess::ProcessError error) {
		blog(LOG_INFO, "%s", // FIXME: skip Qt formatting?
		     QString("QProcess::errorOccurred(error=%1) received").arg(error).toUtf8().constData());
	});
	QObject::connect(process_.get(), &QProcess::stateChanged, [](QProcess::ProcessState newState) {
		blog(LOG_INFO, "%s",
		     QString("QProcess::stateChanged(newState=%1) received").arg(newState).toUtf8().constData());
	});
	QObject::connect(process_.get(), &QProcess::finished, [](int exitCode, QProcess::ExitStatus exitStatus) {
		blog(LOG_INFO, "%s",
		     QString("QProcess::finished(exitCode=%1, exitStatus=%2) received")
			     .arg(exitCode)
			     .arg(exitStatus)
			     .toUtf8()
			     .constData());
	});

	QObject::connect(process_.get(), &QProcess::readyReadStandardError, [this]() {
		QByteArray data;
		while ((data = this->process_->readAllStandardError()).size()) {
			blog(LOG_INFO, "StdErr: %s", data.constData());
		}
	});

	// Process the CSV as it appears on stdout
	// This will be better as a class member than a closure, because we have
	// state, and autoformat at 80 columns with size-8 tabs is really yucking
	// things up!

	QObject::connect(process_.get(), &QProcess::readyReadStandardOutput, this,
			 &PresentMonCapture::readProcessOutput_);

	// Start the proces
	QStringList args({"-output_stdout", "-stop_existing_session", "-session_name", PRESENTMON_SESSION_NAME});
	std::string path;
	if (GetDataFilePath(PRESENTMON_NAME, path)) {
		process_->start(path.c_str(), args, QIODeviceBase::ReadWrite);
	}
}

PresentMonCapture::~PresentMonCapture()
{
	if (!process_)
		return;

	// Maybe inline functionality from
	// https://github.com/GameTechDev/PresentMon/blob/6320933cfaa373cb1702126c0449cf28b9c3431f/PresentData/TraceSession.cpp#L597-L603 instead?
	QStringList args({"-terminate_existing", "-session_name", PRESENTMON_SESSION_NAME});
	auto exit_process = QProcess(this);
	std::string path;
	if (!GetDataFilePath(PRESENTMON_NAME, path))
		return;

	exit_process.start(path.c_str(), args);

	exit_process.waitForFinished();
	process_->waitForFinished();
}

void PresentMonCapture::summarizeAndReset(obs_data_t *dest)
{
	accumulator_->summarizeAndReset(dest);
}

void PresentMonCapture::readProcessOutput_()
{
	char buf[1024];
	char bufCsvCopy[1024];
#if 0
	if (state_->alreadyErrored_) {
		qint64 n = process_->readLine(buf, sizeof(buf));
		blog(LOG_INFO, "POST-ERROR line %d: %s", state_->lineNumber_,
		     buf);
		state_->lineNumber_++;
	}
#endif

	while (!state_->alreadyErrored_ && process_->canReadLine()) {
		qint64 n = process_->readLine(buf, sizeof(buf));
		state_->lineNumber_++; // start on line number 1

		if (n < 1 || n >= static_cast<qint64>(sizeof(buf) - 1)) {
			// XXX emit error
			state_->alreadyErrored_ = true;
		} else {
			if (buf[n - 1] == '\n') {
				//blog(LOG_INFO,
				//"REPLACING NEWLINE WITH NEW NULL");
				buf[n - 1] = '\0';
			}

			if (state_->lineNumber_ < 10) {
				blog(LOG_INFO, "Got line %" PRIu64 ": %s", state_->lineNumber_, buf);
			}

			state_->v_.clear();
			memcpy(bufCsvCopy, buf, sizeof(bufCsvCopy));
			SplitCsvRow(state_->v_, bufCsvCopy);

			if (state_->lineNumber_ == 1) {
				state_->alreadyErrored_ = !state_->parser_.headerRow(state_->v_);
			} else {
				state_->alreadyErrored_ = !state_->parser_.dataRow(state_->v_, &state_->row_);
				if (!state_->alreadyErrored_) {
					accumulator_->frame(state_->row_);
#if 0
					blog(LOG_INFO,
					     QString("real line received: csv line %1 Application=%3, ProcessID=%4, TimeInSeconds=%5, msBetweenPresents=%6")
						     .arg(state_->lineNumber_)
						     .arg(state_->row_
								  .Application)
						     .arg(state_->row_.ProcessID)
						     .arg(state_->row_
								  .TimeInSeconds)
						     .arg(state_->row_
								  .msBetweenPresents)
						     .toUtf8());
#endif
				}
			}
			if (state_->alreadyErrored_) {
				blog(LOG_INFO, "PresentMon CSV parser failed on line %" PRIu64 ": %s",
				     state_->lineNumber_, buf);
			}
		}
	}
}
