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

std::string escapeListKey(const std::string& str);
std::string listKeyPredicate(const std::vector<libyang::Leaf>& listKeyLeafs, const std::vector<std::string>& keyValues);
bool isUserOrderedList(const libyang::DataNode& node);
bool isKeyNode(const libyang::DataNode& maybeList, const libyang::DataNode& node);
}
