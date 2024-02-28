/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#include <libyang-cpp/Collection.hpp>
#include <libyang-cpp/Set.hpp>
#include "YangSchemaLocations.h"
namespace {
}

namespace rousette::restconf {
libyang::DataNode replaceYangLibraryLocations(const std::optional<std::string>& schemeAndHost, const std::string& urlPrefix, libyang::DataNode& node)
{
    // remove all possible location nodes; unlink invalidates the collection so first copy the nodes into a vector
    std::vector<libyang::DataNode> nodesToDelete;
    for (const auto& xpath : {
             "/ietf-yang-library:yang-library/module-set/module/location",
             "/ietf-yang-library:yang-library/module-set/module/submodule/location",
             "/ietf-yang-library:yang-library/module-set/import-only-module/location",
             "/ietf-yang-library:yang-library/module-set/import-only-module/submodule/location",
             "/ietf-yang-library:modules-state/module/schema",
             "/ietf-yang-library:modules-state/module/submodule/schema"}) {

        for (const auto& n : node.findXPath(xpath)) {
            nodesToDelete.emplace_back(n);
        }
    }
    for (auto node : nodesToDelete) {
        node.unlink();
    }

    // if we were unable to parse scheme and hosts, end
    if (!schemeAndHost) {
        return node;
    }

    for (const auto& xpath : {
             "/ietf-yang-library:yang-library/module-set/module",
             "/ietf-yang-library:yang-library/module-set/module/submodule",
             "/ietf-yang-library:yang-library/module-set/import-only-module",
             "/ietf-yang-library:yang-library/module-set/import-only-module/submodule"}) {
        for (const auto& n : node.findXPath(xpath)) {
            const std::string moduleName = std::string{n.findPath("name")->asTerm().valueStr()};

            std::optional<std::string> revision;
            if (auto revisionNode = n.findPath("revision")) {
                revision = revisionNode->asTerm().valueStr();
            }
            const std::string path = moduleName + (revision ? ("@" + *revision) : "");
            n.newPath("location", *schemeAndHost + urlPrefix + path);
        }
    }

    for (const auto& xpath : {
             "/ietf-yang-library:modules-state/module",
             "/ietf-yang-library:modules-state/module/submodule",
         }) {
        for (const auto& n : node.findXPath(xpath)) {
            const auto moduleName = std::string{n.findPath("name")->asTerm().valueStr()};
            const auto revision = std::string{n.findPath("revision")->asTerm().valueStr()};
            const auto path = moduleName + (!revision.empty() ? ("@" + revision) : "");
            n.newPath("schema", *schemeAndHost + urlPrefix + path);
        }
    }

    return node;
}
}
