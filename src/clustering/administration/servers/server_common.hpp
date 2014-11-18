// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef CLUSTERING_ADMINISTRATION_SERVERS_SERVER_COMMON_HPP_
#define CLUSTERING_ADMINISTRATION_SERVERS_SERVER_COMMON_HPP_

#include <string>
#include <vector>

#include "errors.hpp"
#include <boost/shared_ptr.hpp>

#include "clustering/administration/tables/database_metadata.hpp"
#include "clustering/administration/tables/table_metadata.hpp"
#include "clustering/administration/servers/name_client.hpp"
#include "rdb_protocol/artificial_table/backend.hpp"
#include "rdb_protocol/datum.hpp"
#include "rpc/semilattice/view.hpp"

/* This is a base class for the `rethinkdb.server_config` and `rethinkdb.server_status`
pseudo-tables. Subclasses should implement `read_row()` and `write_row()`, in terms of
`lookup()`. */

class common_server_artificial_table_backend_t :
    public artificial_table_backend_t {
public:
    common_server_artificial_table_backend_t(
            boost::shared_ptr< semilattice_readwrite_view_t<
                servers_semilattice_metadata_t> > _servers_sl_view,
            server_name_client_t *_name_client) :
        servers_sl_view(_servers_sl_view),
        name_client(_name_client) {
        servers_sl_view->assert_thread();
        name_client->assert_thread();
    }

    std::string get_primary_key_name();

    bool read_all_rows_as_vector(
            signal_t *interruptor,
            std::vector<ql::datum_t> *rows_out,
            std::string *error_out);

    bool read_row(
            ql::datum_t primary_key,
            signal_t *interruptor,
            ql::datum_t *row_out,
            std::string *error_out);

protected:
    virtual bool format_row(name_string_t const & name,
                            server_id_t const & server_id,
                            server_semilattice_metadata_t const & server,
                            ql::datum_t *row_out,
                            std::string *error_out) = 0;

    /* `lookup()` returns `true` if it finds a row corresponding to the given
    `primary_key` and `false` if it does not find a row. It never produces an error. It
    should only be called on the home thread. */
    bool lookup(
            ql::datum_t primary_key,
            servers_semilattice_metadata_t *servers,
            name_string_t *name_out,
            server_id_t *server_id_out,
            server_semilattice_metadata_t **server_out);

    boost::shared_ptr< semilattice_readwrite_view_t<
        servers_semilattice_metadata_t> > servers_sl_view;
    server_name_client_t *name_client;
};

#endif /* CLUSTERING_ADMINISTRATION_SERVERS_SERVER_COMMON_HPP_ */

