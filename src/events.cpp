/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <memory>
#include <set>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/Session.hpp>
#include <sysrepo.h>

#include <csignal>
#include <unistd.h>

using namespace std::literals;

namespace {
void removeOneAttribute(std::shared_ptr<sysrepo::Session> session, struct lyd_node *elem, struct lyd_attr *&attr)
{
    const auto nextAttr = attr->next;
    lyd_free_attr(session->get_context()->swig_ctx(), elem, attr, 0);
    attr = nextAttr;
}
}

/** @short Listen for changes in the operational datastore */
class AllEvents {
public:
/** @short What YANG-level attributes to keep */
enum class WithAttributes {
    All, ///< Keep all attributes
    RemoveEmptyOperationAndOrigin, ///< Remove sysrepo:operation=none and ietf-origin:unknown
    None, ///< Remove all attributes
};
AllEvents(std::shared_ptr<sysrepo::Session> session, const WithAttributes attrBehavior);

private:
    int onChange(std::shared_ptr<sysrepo::Session> session, const std::string& module);

    std::shared_ptr<sysrepo::Subscribe> sub;
    WithAttributes attrBehavior;
};

AllEvents::AllEvents(std::shared_ptr<sysrepo::Session> session, const WithAttributes attrBehavior)
    : sub(std::make_shared<sysrepo::Subscribe>(session))
    , attrBehavior(attrBehavior)
{
    session->session_switch_ds(SR_DS_OPERATIONAL);
    for (const auto& mod : session->get_context()->get_module_iter()) {
        if (mod->name() == "sysrepo"sv) {
            // this one is magic, subscriptions would cause a SR_ERR_INTERNAL
            continue;
        }
        try {
            sub->module_change_subscribe(mod->name(), [this](const auto sess, const auto name, const auto, const auto, const auto) {
                        return onChange(sess, name);
                    }, nullptr, 0, SR_SUBSCR_CTX_REUSE | SR_SUBSCR_DONE_ONLY | SR_SUBSCR_PASSIVE);
            spdlog::debug("Listening for module {}", mod->name());
        } catch (sysrepo::sysrepo_exception& e) {
            if (e.error_code() == SR_ERR_NOT_FOUND) {
                // nothing to listen for, just ignore this
                continue;
            }
            throw;
        }
    }
}

int AllEvents::onChange(std::shared_ptr<sysrepo::Session> session, const std::string& module)
{
    assert(session->session_get_ds() == SR_DS_OPERATIONAL);
    spdlog::debug("change: {}", module);

    // Unfortunately, the C++ API doesn't give us direct access to the sr_change_iter_t struct,
    // and even if it did, there struct itself it not public, it is only described in the private
    // common.h header. This means that we have to go via the get_change_tree_next() API.
    auto changes = session->get_changes_iter(("/" + module + ":*//.").c_str());

    std::set<lyd_node*> seen;
    while (auto x = session->get_change_tree_next(changes)) {
        auto node = x->node();
        while (node->parent()) {
            node = node->parent();
        }
        if (!seen.insert(node->C_lyd_node()).second) {
            // The get_change_tree_next() iterates over changed items, which means that we should print them
            // starting at their individual roots. There could be many changes below a common root, which is
            // why this cache is needed.
            continue;
        }

        auto copy = node->dup(1);
        struct lyd_node *elem = nullptr, *next = nullptr;
        LY_TREE_DFS_BEGIN(copy->C_lyd_node(), next, elem) {
            if (elem->attr) {
                switch (attrBehavior) {
                case WithAttributes::All:
                    break;
                case WithAttributes::RemoveEmptyOperationAndOrigin:
                    {
                        for (auto attr = elem->attr; attr; /* nothing */) {
                            const auto mod = attr->annotation->module->name;
                            /* spdlog::trace(" XPath {} attr {}:{}: {}", */
                            /*         std::unique_ptr<char, decltype(std::free) *>{lyd_path(elem), std::free}.get(), */
                            /*         mod, attr->name, attr->value_str); */
                            if (mod == "sysrepo"sv && attr->name == "operation"sv && attr->value_str == "none"sv) {
                                removeOneAttribute(session, elem, attr);
                            } else if (mod == "ietf-origin"sv && attr->name == "origin"sv && attr->value_str == "ietf-origin:unknown"sv) {
                                removeOneAttribute(session, elem, attr);
                            } else {
                                attr = attr->next;
                            }
                        }
                    }
                    break;
                case WithAttributes::None:
                    lyd_free_attr(session->get_context()->swig_ctx(), elem, elem->attr, 1);
                    break;
                }
            }
            LY_TREE_DFS_END(copy->C_lyd_node(), next, elem)
        };
        spdlog::info("JSON: {}", copy->print_mem(LYD_JSON, 0));
    }

    return SR_ERR_OK;
}

int main(int argc, char* argv[])
{
    std::ignore = argc;
    std::ignore = argv;

    spdlog::set_level(spdlog::level::trace);

    auto sess = std::make_shared<sysrepo::Session>(std::make_shared<sysrepo::Connection>());
    auto e = AllEvents{
        sess,
        AllEvents::WithAttributes::All,
        /* AllEvents::WithAttributes::RemoveEmptyOperationAndOrigin, */
        /* AllEvents::WithAttributes::None, */
    };

    signal(SIGTERM, [](int) {});
    signal(SIGINT, [](int) {});
    pause();

    return 0;
}
