// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "clustering/administration/issues/server.hpp"

#include "clustering/administration/servers/name_client.hpp"
#include "clustering/administration/servers/name_server.hpp"
#include "clustering/administration/datum_adapter.hpp"

const datum_string_t server_down_issue_t::server_down_issue_type =
    datum_string_t("server_down");
const uuid_u server_down_issue_t::base_issue_id =
    str_to_uuid("377a1d56-db29-416d-97f7-4ce1efc6e97b");

const datum_string_t server_ghost_issue_t::server_ghost_issue_type =
    datum_string_t("server_ghost");
const uuid_u server_ghost_issue_t::base_issue_id =
    str_to_uuid("193df26a-eac7-4373-bf0a-12bbc0b869ed");

server_down_issue_t::server_down_issue_t() { }

server_down_issue_t::server_down_issue_t(const server_id_t &_down_server_id) :
    local_issue_t(from_hash(base_issue_id, _down_server_id)),
    down_server_id(_down_server_id) { }

bool server_down_issue_t::build_info_and_description(
        UNUSED const metadata_t &metadata,
        server_name_client_t *name_client,
        admin_identifier_format_t identifier_format,
        ql::datum_t *info_out,
        datum_string_t *description_out) const {
    ql::datum_t down_server_name_or_uuid;
    name_string_t down_server_name;
    if (!convert_server_id_to_datum(down_server_id, identifier_format, name_client,
            &down_server_name_or_uuid, &down_server_name)) {
        /* If a disconnected server is deleted from `rethinkdb.server_config`, there is a
        brief window of time before the `server_down_issue_t` is destroyed. During that
        time, if the user reads from `rethinkdb.issues`, we don't want to show them an
        issue saying "__deleted_server__ is still connected". So we return `false` in
        this case. */
        return false;
    }
    ql::datum_array_builder_t affected_servers_builder(
        ql::configured_limits_t::unlimited);
    std::string affected_servers_str;
    size_t num_affected = 0;
    for (const server_id_t &id : affected_server_ids) {
        ql::datum_t name_or_uuid;
        name_string_t name;
        if (!convert_server_id_to_datum(id, identifier_format, name_client,
                &name_or_uuid, &name)) {
            /* Ignore connectivity reports from servers that have been declared dead */
            continue;
        }
        affected_servers_builder.add(name_or_uuid);
        if (!affected_servers_str.empty()) {
            affected_servers_str += ", ";
        }
        affected_servers_str += name.str();
        ++num_affected;
    }
    if (num_affected == 0) {
        /* The servers making the reports have all been declared dead */
        return false;
    }
    ql::datum_object_builder_t info_builder;
    info_builder.overwrite("server", down_server_name_or_uuid);
    info_builder.overwrite("affected_servers",
        std::move(affected_servers_builder).to_datum());
    *info_out = std::move(info_builder).to_datum();
    *description_out = datum_string_t(strprintf(
        "Server `%s` is inaccessible from %s%s.", down_server_name.c_str(),
        (num_affected == 1 ? "" : "these servers: "), affected_servers_str.c_str()));
    return true;
}

server_ghost_issue_t::server_ghost_issue_t() { }

server_ghost_issue_t::server_ghost_issue_t(const server_id_t &_ghost_server_id,
                                           const std::string &_hostname,
                                           int64_t _pid) :
    local_issue_t(from_hash(base_issue_id, _ghost_server_id)),
    ghost_server_id(_ghost_server_id), hostname(_hostname), pid(_pid) { }

bool server_ghost_issue_t::build_info_and_description(
        UNUSED const metadata_t &metadata,
        UNUSED server_name_client_t *name_client,
        UNUSED admin_identifier_format_t identifier_format,
        ql::datum_t *info_out,
        datum_string_t *description_out) const {
    ql::datum_object_builder_t builder;
    builder.overwrite("server_id", convert_uuid_to_datum(ghost_server_id));
    builder.overwrite("hostname", ql::datum_t(datum_string_t(hostname)));
    builder.overwrite("pid", ql::datum_t(static_cast<double>(pid)));
    *info_out = std::move(builder).to_datum();
    *description_out = datum_string_t(strprintf(
        "The server process with hostname `%s` and PID %" PRId64 " was deleted from the "
        "`rethinkdb.server_config` table, but the process is still running. Once a "
        "server has been deleted from `rethinkdb.server_config`, the data files "
        "for that RethinkDB instance cannot be used any more; you must start a new "
        "RethinkDB instance with an empty data directory.",
        hostname.c_str(), 
        pid));
    return true;
}

server_issue_tracker_t::server_issue_tracker_t(
        local_issue_aggregator_t *parent,
        boost::shared_ptr<semilattice_read_view_t<cluster_semilattice_metadata_t> >
            _cluster_sl_view,
        watchable_map_t<peer_id_t, cluster_directory_metadata_t> *_directory_view,
        server_name_client_t *_name_client,
        server_name_server_t *_name_server) :
    down_issues(std::vector<server_down_issue_t>()),
    ghost_issues(std::vector<server_ghost_issue_t>()),
    down_subs(parent, down_issues.get_watchable(),
        &local_issues_t::server_down_issues),
    ghost_subs(parent, ghost_issues.get_watchable(),
        &local_issues_t::server_ghost_issues),
    cluster_sl_view(_cluster_sl_view),
    directory_view(_directory_view),
    name_client(_name_client),
    name_server(_name_server),
    cluster_sl_subs(std::bind(&server_issue_tracker_t::recompute, this),
                    cluster_sl_view),
    directory_subs(directory_view,
                   std::bind(&server_issue_tracker_t::recompute, this),
                   false),
    name_client_subs(std::bind(&server_issue_tracker_t::recompute, this))
{
    watchable_t<std::map<server_id_t, peer_id_t> >::freeze_t freeze(
        name_client->get_server_id_to_peer_id_map());
    name_client_subs.reset(name_client->get_server_id_to_peer_id_map(), &freeze);
    recompute();   
}

server_issue_tracker_t::~server_issue_tracker_t() {
    // Clear any outstanding down/ghost issues
    down_issues.apply_atomic_op(
        [] (std::vector<server_down_issue_t> *issues) -> bool {
            issues->clear();
            return true;
        });
    ghost_issues.apply_atomic_op(
        [] (std::vector<server_ghost_issue_t> *issues) -> bool {
            issues->clear();
            return true;
        });
}

void server_issue_tracker_t::recompute() {
    std::vector<server_down_issue_t> down_list;
    std::vector<server_ghost_issue_t> ghost_list;
    for (auto const &pair : cluster_sl_view->get().servers.servers) {
        boost::optional<peer_id_t> peer_id =
            name_client->get_peer_id_for_server_id(pair.first);
        if (!pair.second.is_deleted() && !static_cast<bool>(peer_id)) {
            if (name_server->get_permanently_removed_signal()->is_pulsed()) {
                /* We are a ghost server. Ghost servers don't make disconnection reports.
                */
                continue;
            }
            down_list.push_back(server_down_issue_t(pair.first));
        } else if (pair.second.is_deleted() && static_cast<bool>(peer_id)) {
            directory_view->read_key(*peer_id,
                [&](const cluster_directory_metadata_t *md) {
                    if (md == nullptr) {
                        /* Race condition; the server appeared in `name_client`'s list of
                        servers but hasn't appeared in the directory yet. */
                        return;
                    }
                    ghost_list.push_back(server_ghost_issue_t(
                        pair.first, md->hostname, md->pid));
                });
        }
    }
    down_issues.set_value(down_list);
    ghost_issues.set_value(ghost_list);
}

void server_issue_tracker_t::combine(
        local_issues_t *local_issues,
        std::vector<scoped_ptr_t<issue_t> > *issues_out) {
    // Combine down issues
    {
        std::map<server_id_t, server_down_issue_t*> combined_down_issues;
        for (auto &down_issue : local_issues->server_down_issues) {
            auto combined_it = combined_down_issues.find(down_issue.down_server_id);
            if (combined_it == combined_down_issues.end()) {
                combined_down_issues.insert(std::make_pair(
                    down_issue.down_server_id,
                    &down_issue));
            } else {
                rassert(down_issue.affected_server_ids.size() == 1);
                combined_it->second->add_server(down_issue.affected_server_ids[0]);
            }
        }

        for (auto const &it : combined_down_issues) {
            issues_out->push_back(scoped_ptr_t<issue_t>(
                new server_down_issue_t(*it.second)));
        }
    }

    // Combine ghost issues
    {
        std::set<server_id_t> ghost_issues_seen;
        for (auto &ghost_issue : local_issues->server_ghost_issues) {
            /* This is trivial, since we don't track affected servers for ghost issues.
            We assume hostname and PID are reported the same by every server, so we just
            show the first report and ignore the others. */
            if (ghost_issues_seen.count(ghost_issue.ghost_server_id) == 0) {
                issues_out->push_back(scoped_ptr_t<issue_t>(
                    new server_ghost_issue_t(ghost_issue)));
                ghost_issues_seen.insert(ghost_issue.ghost_server_id);
            }
        }
    }

}
