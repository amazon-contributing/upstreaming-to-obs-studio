#include "presentmon-csv-parser.hpp"

#include <obs.hpp>   // logging

void SplitCsvRow(std::vector<const char *> &columns, char *csvRow)
{
	while (*csvRow != '\0') {
		columns.push_back(csvRow);
		do {
			csvRow++;
		} while (*csvRow != '\0' && *csvRow != ',');
		if (*csvRow != '\0') {
			*csvRow = '\0';
			csvRow++;
		}
	}
}

CsvRowParser::CsvRowParser()
	: colApplication_(-1),
	  colProcessID_(-1),
	  colTimeInSeconds_(-1),
	  colMsBetweenPresents_(-1)
{
}

QString CsvRowParser::lastError() const
{
	return lastError_;
}

void CsvRowParser::setError(QString s)
{
	lastError_ = s;
	blog(LOG_ERROR, "CsvRowParser::setError: %s", s.toUtf8().constData());
}

bool CsvRowParser::headerRow(const std::vector<const char *> &columns)
{
	for (int i = 0; i < columns.size(); i++) {
		if (0 == strcmp("Application", columns[i])) {
			if (colApplication_ >= 0)
				setError("Duplicate column Application");
			colApplication_ = i;
		}
		if (0 == strcmp("ProcessID", columns[i])) {
			if (colProcessID_ >= 0)
				setError("Duplicate column ProcessID");
			colProcessID_ = i;
		}
		if (0 == strcmp("TimeInSeconds", columns[i])) {
			if (colTimeInSeconds_ >= 0)
				setError("Duplicate column TimeInSeconds");
			colTimeInSeconds_ = i;
		}
		if (0 == strcmp("msBetweenPresents", columns[i])) {
			if (colMsBetweenPresents_ >= 0)
				setError("Duplicate column msBetweenPresents");
			colMsBetweenPresents_ = i;
		}
	}

	if (colApplication_ == -1)
		setError("Header missing column Application");
	if (colProcessID_ == -1)
		setError("Header missing column ProcessID");
	if (colTimeInSeconds_ == -1)
		setError("Header missing column TimeInSeconds");
	if (colMsBetweenPresents_ == -1)
		setError("Header missing column msBetweenPresents");

	return lastError_.isEmpty();
}

bool CsvRowParser::dataRow(const std::vector<const char *> &columns,
			   ParsedCsvRow *dest)
{
	if (colApplication_ >= columns.size())
		setError("Data row missing column Application");
	if (colProcessID_ >= columns.size())
		setError("Data row missing column ProcessID");
	if (colTimeInSeconds_ >= columns.size())
		setError("Data row missing column TimeInSeconds");
	if (colMsBetweenPresents_ >= columns.size())
		setError("Data row missing column msBetweenPresents");

	char *endptr;

	// We truncate application name if necessary for it to fit
	strncpy(dest->Application, columns[colApplication_],
	       sizeof(dest->Application) - 1);
	dest->Application[sizeof(dest->Application) - 1] = '\0';

	dest->ProcessID = strtol(columns[colProcessID_], &endptr, 10);
	if (*endptr != '\0')
		setError("Data row ProcessID not an int");

	dest->TimeInSeconds = strtod(columns[colTimeInSeconds_], &endptr);
	if (*endptr != '\0')
		setError("Data row TimeInSeconds not a float");

	dest->msBetweenPresents =
		strtod(columns[colMsBetweenPresents_], &endptr);
	if (*endptr != '\0')
		setError("Data row msBetweenPresents not a float");

	return lastError_.isEmpty();
}


void testCsvParser()
{
	const char *csvLines[] = {
		"Application,ProcessID,SwapChainAddress,Runtime,SyncInterval,PresentFlags,Dropped,TimeInSeconds,msInPresentAPI,msBetweenPresents,AllowsTearing,PresentMode,msUntilRenderComplete,msUntilDisplayed,msBetweenDisplayChange",
		"MassEffect3.exe,18580,0x00000194515FA9C0,DXGI,1,0,0,32.34646610000000,2.79980000000000,33.59020000000000,0,Composed: Flip,68.39570000000001,117.88900000000000,16.61390000000000",
		"MassEffect3.exe,18580,0x00000194515FA9C0,DXGI,1,0,0,32.36793490000000,47.12030000000000,21.46880000000000,0,Composed: Flip,109.20750000000000,146.40840000000000,49.98820000000001",
		"MassEffect3.exe,18580,0x00000194515FA9C0,DXGI,1,0,0,32.42390420000000,53.39470000000000,55.96930000000000,0,Composed: Flip,105.70290000000000,140.43780000000001,49.99870000000000",
		nullptr};

	std::vector<const char *> v;
	ParsedCsvRow row;
	CsvRowParser parser;
	for (int i = 0; csvLines[i]; i++) {
		char *s = strdup(csvLines[i]);

		v.clear();
		SplitCsvRow(v, s);

		if (i == 0) {
			bool ok = parser.headerRow(v);
			blog(LOG_INFO, QString("afTest: csv line %1 ok = %2")
					       .arg(i)
					       .arg(ok)
					       .toUtf8());
		} else {
			bool ok = parser.dataRow(v, &row);
			blog(LOG_INFO,
			     QString("afTest: csv line %1 ok = %2, Application=%3, ProcessID=%4, TimeInSeconds=%5, msBetweenPresents=%6")
				     .arg(i)
				     .arg(ok)
				     .arg(row.Application)
				     .arg(row.ProcessID)
				     .arg(row.TimeInSeconds)
				     .arg(row.msBetweenPresents)
				     .toUtf8());
		}

		free(s);
	}
}
