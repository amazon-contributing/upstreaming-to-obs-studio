#pragma once

#include "simulcast-output.h"

#include <QWidget>

struct LocalConfig {
	QString goLiveApiUrl;
	QString streamKey;
};

class SimulcastDockWidget : public QWidget {
public:
	SimulcastDockWidget(QWidget *parent = 0);

	void SaveConfig();
	void LoadConfig();

	SimulcastOutput &Output() { return output_; }

private:
	SimulcastOutput output_;
	LocalConfig localConfig_;
};
