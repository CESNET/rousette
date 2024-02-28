/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#include <functional>
#include <sysrepo-cpp/Connection.hpp>
#include "YangSchemaLocations.h"

namespace {
enum class ListEntryType {
    YangLibraryModule,
    YangLibraryImportOnlyModule,
    ModulesState,
};

sysrepo::ErrorCode yangLibraryLocationCb(sysrepo::Session& session, std::optional<libyang::DataNode>& output, const std::string& urlPathPrefix, ListEntryType type)
{
    for (const auto& mod : session.getContext().modules()) {
        std::string prop;

        switch(type) {
            case ListEntryType::YangLibraryModule:
                if (!mod.implemented()) {
                    continue;
                }
                prop = "/ietf-yang-library:yang-library/module-set[name='complete']/module[name='" + std::string{mod.name()} + "']/location[1]";
                break;
            case ListEntryType::YangLibraryImportOnlyModule:
                if (mod.implemented() || !mod.revision()) {
                    continue;
                }
                prop = "/ietf-yang-library:yang-library/module-set[name='complete']/import-only-module[name='" + std::string{mod.name()} + "'][revision='" + std::string{*mod.revision()} + "']/location[1]";
                break;
            case ListEntryType::ModulesState:
                prop = "/ietf-yang-library:modules-state/module[name='" + std::string{mod.name()} + "']";
                if (mod.revision()) {
                    prop += "[revision='" + std::string{*mod.revision()} + "']";
                } else {
                    prop += "[revision='']";
                }
                prop += "/schema";
                break;
        }

        /* FIXME: The prefix does not contain scheme and host, it is only a path prefix.
         * We are behind a reverse proxy, we don't know the scheme and the host :( It can't be set statically.
         * Also, we set the prefix during construction of this class so we can't just take it from the request headers.
         */
        std::string val = urlPathPrefix + std::string{mod.name()};
        if (mod.revision()) {
            val += "@" + std::string{*mod.revision()};
        }

        if (!output) {
            output = session.getContext().newPath(prop, val, libyang::CreationOptions::Output);
        } else {
            output->newPath(prop, val, libyang::CreationOptions::Output);
        }
    }
    return sysrepo::ErrorCode::Ok;
}
}

namespace rousette::restconf {

YangSchemaLocations::YangSchemaLocations(sysrepo::Connection conn, const std::string& urlPathPrefix)
    : m_urlPathPrefix(urlPathPrefix)
    , m_session(conn.sessionStart(sysrepo::Datastore::Operational))
{
    m_sub = m_session.onOperGet(
        "ietf-yang-library", [this](auto session, auto, auto, auto, auto, auto, std::optional<libyang::DataNode>& output) {
            return yangLibraryLocationCb(session, output, m_urlPathPrefix, ListEntryType::YangLibraryModule);
        },
        "/ietf-yang-library:yang-library/module-set/module/location");

    m_sub->onOperGet(
        "ietf-yang-library", [this](auto session, auto, auto, auto, auto, auto, std::optional<libyang::DataNode>& output) {
            return yangLibraryLocationCb(session, output, m_urlPathPrefix, ListEntryType::YangLibraryImportOnlyModule);
        },
        "/ietf-yang-library:yang-library/module-set/import-only-module/location");

    m_sub->onOperGet(
        "ietf-yang-library", [this](auto session, auto, auto, auto, auto, auto, std::optional<libyang::DataNode>& output) {
            return yangLibraryLocationCb(session, output, m_urlPathPrefix, ListEntryType::ModulesState);
        },
        "/ietf-yang-library:modules-state/module/schema");
}
}
