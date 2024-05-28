/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#include <boost/algorithm/string/predicate.hpp>
#include <libyang-cpp/Collection.hpp>
#include <libyang-cpp/Set.hpp>
#include "YangSchemaLocations.h"

namespace {
const auto moduleNodesXPath = "/ietf-yang-library:yang-library/module-set/module |"
                              "/ietf-yang-library:yang-library/module-set/module/submodule |"
                              "/ietf-yang-library:yang-library/module-set/import-only-module |"
                              "/ietf-yang-library:yang-library/module-set/import-only-module/submodule |"
                              "/ietf-yang-library:modules-state/module |"
                              "/ietf-yang-library:modules-state/module/submodule";
}

namespace rousette::restconf {
libyang::DataNode replaceYangLibraryLocations(const std::optional<std::string>& schemeAndHost, const std::string& urlPrefix, libyang::DataNode& node)
{
    std::vector<libyang::DataNode> moduleNodes;
    for (const auto& n : node.findXPath(moduleNodesXPath)) {
        moduleNodes.emplace_back(n);
    }

    /* unlink location leaf-list nodes.
     * In the yang-library container there is a leaf-list location.
     * In the modules-state container there is a leaf called schema
     * The names of these leaves are not used in another context in the other container so we can just remove all without checking what node are we in
     */
    for (const auto& n : moduleNodes) {
        // remove all possible location nodes; unlink invalidates the collection so first copy the nodes into a vector
        std::vector<libyang::DataNode> locationNodes;

        for (const auto& child : n.findXPath("location | schema")) {
            locationNodes.emplace_back(child);
        }

        for (auto& child : locationNodes) {
            child.unlink();
        }
    }

    // if we were unable to parse scheme and hosts, end without providing URLs of the YANG modules
    if (!schemeAndHost) {
        return node;
    }

    for (const auto& n : moduleNodes) {
        const std::string moduleName = std::string{n.findPath("name")->asTerm().valueStr()};

        std::optional<std::string> revision; // in some lists the revision leaf is optional, in some lists it is mandatory but can be empty string
        if (auto revisionNode = n.findPath("revision")) {
            if (auto val = revisionNode->asTerm().valueStr(); !val.empty()) {
                revision = val;
            }
        }

        std::string locationLeafName;

        if (boost::algorithm::starts_with(n.path(), "/ietf-yang-library:modules-state")) {
            locationLeafName = "schema";
        } else {
            locationLeafName = "location";
        }

        const std::string path = moduleName + (revision ? ("@" + *revision) : "");
        n.newPath(locationLeafName, *schemeAndHost + urlPrefix + path);
    }

    return node;
}
}
