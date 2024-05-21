/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <chrono>

namespace libyang {
class Leaf;
class DataNode;
}

namespace rousette::restconf {

template <typename Clock, typename Precision>
std::string yangDateTime(const std::chrono::time_point<Clock>& timePoint);

std::string escapeListKey(const std::string& str);
std::string listKeyPredicate(const std::vector<libyang::Leaf>& listKeyLeafs, const std::vector<std::string>& keyValues);
bool isUserOrderedList(const libyang::DataNode& node);
}
