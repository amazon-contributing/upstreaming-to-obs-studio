#include "immutable-date-time.hpp"

ImmutableDateTime::ImmutableDateTime(QDateTime date_time)
	: date_time(date_time),
	  date_time_string(
		  date_time.toString(Qt::ISODateWithMs).toUtf8().constData())
{
}

ImmutableDateTime ImmutableDateTime::CurrentTimeUtc()
{
	return ImmutableDateTime(QDateTime::currentDateTimeUtc());
}

quint64 ImmutableDateTime::MSecsElapsed() const
{
	return date_time.msecsTo(QDateTime::currentDateTimeUtc());
}
