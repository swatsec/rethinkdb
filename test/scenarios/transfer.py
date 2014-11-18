#!/usr/bin/env python
# Copyright 2010-2014 RethinkDB, all rights reserved.

import sys, os, time

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), os.path.pardir, 'common')))
import http_admin, driver, workload_runner, scenario_common, rdb_workload_common
from vcoptparse import *

op = OptParser()
scenario_common.prepare_option_parser_mode_flags(op)
op["workload1"] = PositionalArg()
op["workload2"] = PositionalArg()
op["timeout"] = IntFlag("--timeout", 600)
opts = op.parse(sys.argv)

with driver.Metacluster() as metacluster:
    cluster = driver.Cluster(metacluster)
    _, command_prefix, serve_options = scenario_common.parse_mode_flags(opts)

    print "Starting cluster..."
    files1 = driver.Files(metacluster, db_path="db-first", console_output="create-output-first", command_prefix=command_prefix)
    process1 = driver.Process(cluster, files1, console_output="serve-output-first", command_prefix=command_prefix, extra_options=serve_options)
    process1.wait_until_started_up()

    print "Creating table..."
    http1 = http_admin.ClusterAccess([("localhost", process1.http_port)])
    dc = http1.add_datacenter()
    http1.move_server_to_datacenter(files1.server_name, dc)
    ns = scenario_common.prepare_table_for_workload(http1, primary = dc)
    http1.wait_until_blueprint_satisfied(ns)
    rdb_workload_common.wait_for_table(host='localhost', port=process1.driver_port, table=ns.name)

    workload_ports_1 = scenario_common.get_workload_ports(ns, [process1])
    workload_runner.run(opts["workload1"], workload_ports_1, opts["timeout"])

    print "Bringing up new server..."
    files2 = driver.Files(metacluster, db_path="db-second", console_output="create-output-second", command_prefix=command_prefix)
    process2 = driver.Process(cluster, files2, console_output="serve-output-second", command_prefix=command_prefix, extra_options=serve_options)
    process2.wait_until_started_up()
    http1.update_cluster_data(3)
    http1.move_server_to_datacenter(files2.server_name, dc)
    http1.set_table_affinities(ns, {dc: 1})
    http1.check_no_issues()

    print "Waiting for backfill..."
    backfill_start_time = time.time()
    http1.wait_until_blueprint_satisfied(ns, timeout = 3600)
    print "Backfill completed after %d seconds." % (time.time() - backfill_start_time)

    print "Shutting down old server..."
    process1.check_and_stop()
    http2 = http_admin.ClusterAccess([("localhost", process2.http_port)])
    http2.declare_server_dead(files1.server_name)
    http2.set_table_affinities(ns.name, {dc.name: 0})
    http2.check_no_issues()
    http2.wait_until_blueprint_satisfied(ns.name)
    rdb_workload_common.wait_for_table(host='localhost', port=process2.driver_port, table=ns.name)

    workload_ports_2 = scenario_common.get_workload_ports(http2.find_table(ns.name), [process2])
    workload_runner.run(opts["workload2"], workload_ports_2, opts["timeout"])

    cluster.check_and_stop()
