
#pragma once

#include <QObject>
#include <QDateTime>
#include <QTimer>

#ifdef _WIN32
#include "wmi-data-provider.hpp"
#endif

#include <util/platform.h>

#include <memory>
#include <optional>
#include <vector>

class BerryessaSubmitter;

struct OSCPUUsageInfoDeleter {
	void operator()(os_cpu_usage_info *info) { os_cpu_usage_info_destroy(info); }
};

struct OBSFrameCounters {
	uint32_t output, skipped;
	uint32_t rendered, lagged;
};

struct OBSEncoderFrameCounters {
	OBSWeakEncoderAutoRelease weak_output;
	uint32_t output, skipped;
};

class BerryessaEveryMinute : public QObject {
	Q_OBJECT
public:
	BerryessaEveryMinute(QObject *parent, BerryessaSubmitter *berryessa, const std::vector<OBSEncoder> &outputs);
	virtual ~BerryessaEveryMinute();

private slots:
	void fire();

private:
	BerryessaSubmitter *berryessa_;
	QTimer timer_;
	QDateTime startTime_;

	struct UsageInfoCounters {
		std::unique_ptr<os_cpu_usage_info, OSCPUUsageInfoDeleter> obs_cpu_usage_info_;

		OBSFrameCounters frame_counters_;
		std::vector<OBSEncoderFrameCounters> encoder_counters_;

#ifdef _WIN32
		std::optional<WMIQueries> wmi_queries_;
#endif
	};

	std::shared_ptr<UsageInfoCounters> shared_counters_;
};
