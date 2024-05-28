#include "goliveapi-censoredjson.hpp"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

void censorRecurse(json &data)
{
	if (!data.is_structured())
		return;

	auto it = data.find("authentication");
	if (it != data.end() && it->is_string()) {
		*it = "CENSORED";
	}

	for (auto &child : data) {
		censorRecurse(child);
	}
}

QString censoredJson(json data, bool pretty)
{
	censorRecurse(data);

	return QString::fromStdString(data.dump(pretty ? 4 : -1));
}
