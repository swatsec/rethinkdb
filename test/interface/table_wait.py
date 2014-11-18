#!/usr/bin/env python
# Copyright 2014 RethinkDB, all rights reserved.

"""The `interface.table_wait` test checks that waiting for a table returns when the table is available for writing."""

from __future__ import print_function

import sys, os, time, traceback, multiprocessing

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), os.path.pardir, 'common')))
import driver, scenario_common, utils, vcoptparse

r = utils.import_python_driver()

op = vcoptparse.OptParser()
scenario_common.prepare_option_parser_mode_flags(op)
opts = op.parse(sys.argv)

db = "test"
tables = ["table1", "table2", "table3"]
delete_table = "delete"

def check_table_states(conn, ready):
    statuses = r.db(db).table_status(r.args(tables)).run(conn)
    return all(map(lambda s: (s["status"]['ready_for_writes'] == ready), statuses))

def wait_for_table_states(conn, ready):
    while not check_table_states(conn, ready=ready):
        time.sleep(0.1)

def create_tables(conn):
    r.expr(tables).for_each(r.db(db).table_create(r.row)).run(conn)
    r.db(db).table_create(delete_table).run(conn) # An extra table to be deleted during a wait
    r.db(db).table_list().for_each(r.db(db).table(r.row).insert(r.range(200).map(lambda i: {'id':i}))).run(conn)
    r.db(db).reconfigure(shards=2, replicas=2).run(conn)
    statuses = r.db(db).table_wait().run(conn)
    assert check_table_states(conn, ready=True), \
        "Wait after reconfigure returned before tables were ready, statuses: %s" % str(statuses)

def spawn_table_wait(port, tbls):
    def do_table_wait(port, tbls, done_event):
        conn = r.connect("localhost", port)
        try:
            r.db(db).table_wait(r.args(tbls)).run(conn)
        finally:
            done_event.set()

    def do_post_write(port, tbls, start_event):
        conn = r.connect("localhost", port)
        start_event.wait()
        r.expr(tbls).for_each(r.db(db).table(r.row).insert({})).run(conn)

    sync_event = multiprocessing.Event()
    wait_proc = multiprocessing.Process(target=do_table_wait, args=(port, tbls, sync_event))
    write_proc = multiprocessing.Process(target=do_post_write, args=(port, tbls, sync_event))
    wait_proc.start()
    write_proc.start()
    return write_proc

with driver.Metacluster() as metacluster:
    cluster = driver.Cluster(metacluster)
    _, command_prefix, serve_options = scenario_common.parse_mode_flags(opts)
    
    print("Spinning up two processes...")
    files1 = driver.Files(metacluster, console_output="create-output-1", server_name="a", command_prefix=command_prefix)
    proc1 = driver.Process(cluster, files1, console_output="serve-output-1", command_prefix=command_prefix, extra_options=serve_options)
    files2 = driver.Files(metacluster, console_output = "create-output-2", server_name="b", command_prefix=command_prefix)
    proc2 = driver.Process(cluster, files2, console_output="serve-output-2", command_prefix=command_prefix, extra_options=serve_options)
    proc1.wait_until_started_up()
    proc2.wait_until_started_up()
    cluster.check()

    conn = r.connect("localhost", proc1.driver_port)
    r.db_create(db).run(conn)

    print("Testing simple table (several times)...")
    for i in xrange(5):
        res = r.db(db).table_create("simple").run(conn)
        assert res == {"created": 1}
        r.db(db).table("simple").reconfigure(shards=12, replicas=1).run(conn)
        r.db(db).table_wait("simple").run(conn)
        count = r.db(db).table("simple").count().run(conn)
        assert count == 0
        res = r.db(db).table_drop("simple").run(conn)
        assert res == {"dropped": 1}

    print("Creating %d tables..." % (len(tables) + 1))
    create_tables(conn)

    print("Killing second server...")
    proc2.close()

    wait_for_table_states(conn, ready=False)
    waiter_procs = [ spawn_table_wait(proc1.driver_port, [tables[0]]),            # Wait for one table
                     spawn_table_wait(proc1.driver_port, [tables[1], tables[2]]), # Wait for two tables
                     spawn_table_wait(proc1.driver_port, []) ]                    # Wait for all tables

    def wait_for_deleted_table(port, db, table):
        c = r.connect("localhost", port)
        try:
            r.db(db).table_wait(table).run(c)
            raise RuntimeError("`table_wait` did not error when waiting on a deleted table.")
        except r.RqlRuntimeError as ex:
            assert ex.message == "Table `%s.%s` does not exist." % (db, table), \
                "Unexpected error when waiting for a deleted table: %s" % ex.message

    error_wait_proc = multiprocessing.Process(target=wait_for_deleted_table, args=(proc1.driver_port, db, delete_table))
    error_wait_proc.start()
    r.db(db).table_drop(delete_table).run(conn)
    error_wait_proc.join()

    # Wait some time to make sure the wait doesn't return early
    waiter_procs[0].join(15)
    assert all(map(lambda w: w.is_alive(), waiter_procs)), "Wait returned while a server was still down."

    print("Restarting second server...")
    proc2 = driver.Process(cluster, files2, console_output="serve-output-2", command_prefix=command_prefix, extra_options=serve_options)
    proc2.wait_until_started_up()

    print("Waiting for table readiness...")
    map(lambda w: w.join(), waiter_procs)
    assert check_table_states(conn, ready=True), "`table_wait` returned, but not all tables are ready"

    cluster.check_and_stop()
print("Done.")

