#pragma once

#include <obs.hpp>
#include <QString>
class QWidget;

obs_data_t *DownloadGoLiveConfig(QWidget *parent, QString url,
				 obs_data_t *postData);
