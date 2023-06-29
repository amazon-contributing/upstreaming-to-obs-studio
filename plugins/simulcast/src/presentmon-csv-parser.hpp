#pragma once

#include <vector>
#include <QString>

/**
 * Mutates `csvRow`, replacing ',' separators with null terminators,
 * and appends a pointer to the beginning of every column to `columns`.
 */
void SplitCsvRow(std::vector<const char *> &columns, char *csvRow);

#define PRESENTMON_APPNAME_LEN 56

struct ParsedCsvRow {
	char Application[PRESENTMON_APPNAME_LEN];
	uint64_t ProcessID;
	float TimeInSeconds;
	float msBetweenPresents;
};

class CsvRowParser {
public:
	CsvRowParser();

	/**
	 * Call this first with the CSV's header row. On error this returns false,
	 * and will change the result of lastError().
	 * Behavior undefined if called twice.
	 *
	 * If a CSV file is missing a header row, or it is missing an expected column, this
	 * will be detected as an error.
	 */
	bool headerRow(const std::vector<const char *> &columns);

	/**
	 * Call this with CSV data rows. On error this returns false, and
	 * will change the result of lastError().
	 * Behavior undefined if headerRow() was not called exactly once, or returned an error.
	 *
	 * If a valid CSV file is missing required columns in any row, or they fail to
	 * parse as ints/floats, this will be detected as an error.
	 */
	bool dataRow(const std::vector<const char *> &columns,
		     ParsedCsvRow *dest);

	QString lastError() const;

private:
	// Lots of repetition in the code; given a couple more columns
	// it will be worth using a slightly more complicated generic data structure
	// we can loop over. Something like
	//   struct ColumnFloatTarget { const char* name; int index; float ParsedCsvRow::*destMemberPtr; }
	//   floatTargets.push_back(ColumnFloatTarget("timeInSeconds", n, &ParsedCsvRow::timeInSeconds));
	//   floatTargets.push_back(ColumnFloatTarget("msBetweenPresents", n, &ParsedCsvRow::msBetweenPresents));
	int colApplication_, colProcessID_, colTimeInSeconds_,
		colMsBetweenPresents_;

	QString lastError_;

	void setError(QString s);
};


/** XXX turn this into a unit test which can fail */
void testCsvParser();
