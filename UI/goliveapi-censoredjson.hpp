#pragma once

#include <QString>
#include <nlohmann/json_fwd.hpp>

/**
 * Returns the input serialized to JSON, but any non-empty "authorization"
 * properties have their values replaced by "CENSORED".
 */
QString censoredJson(nlohmann::json data, bool pretty = false);
