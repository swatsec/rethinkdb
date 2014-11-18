#!/usr/bin/env python
# Copyright 2010-2012 RethinkDB, all rights reserved.
import sys, os, time
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), os.path.pardir, 'common')))
import http_admin, driver, workload_runner, scenario_common
from vcoptparse import *

op = OptParser()
workload_runner.prepare_option_parser_for_split_or_continuous_workload(op)
scenario_common.prepare_option_parser_mode_flags(op)
opts = op.parse(sys.argv)

with driver.Metacluster() as metacluster:
    print "Starting cluster..."
    cluster = driver.Cluster(metacluster)
    _, command_prefix, serve_options = scenario_common.parse_mode_flags(opts)
    primary_files = driver.Files(metacluster, db_path="db-1", console_output="create-output-1", command_prefix=command_prefix)
    primary = driver.Process(cluster, primary_files, console_output="serve-output-1", command_prefix=command_prefix, extra_options=serve_options)
    secondary_files = driver.Files(metacluster, db_path="db-2", console_output="create-output-2", command_prefix=command_prefix)
    secondary = driver.Process(cluster, secondary_files, console_output="serve-output-2", command_prefix=command_prefix, extra_options=serve_options)
    primary.wait_until_started_up()
    secondary.wait_until_started_up()

    print "Creating table..."
    http = http_admin.ClusterAccess([("localhost", secondary.http_port)])
    primary_dc = http.add_datacenter()
    http.move_server_to_datacenter(primary.files.server_name, primary_dc)
    secondary_dc = http.add_datacenter()
    http.move_server_to_datacenter(secondary.files.server_name, secondary_dc)
    ns = scenario_common.prepare_table_for_workload(http, primary=primary_dc, affinities={primary_dc: 0, secondary_dc: 1})
    http.set_table_ack_expectations(ns, {secondary_dc: 1})
    http.wait_until_blueprint_satisfied(ns)
    cluster.check()
    http.check_no_issues()

    workload_ports = scenario_common.get_workload_ports(ns, [secondary])
    with workload_runner.SplitOrContinuousWorkload(opts, workload_ports) as workload:
        workload.run_before()
        cluster.check()
        http.check_no_issues()
        print "Killing the primary..."
        primary.close()
        http.declare_server_dead(primary.files.server_name)
        http.move_table_to_datacenter(ns, secondary_dc)
        http.set_table_affinities(ns, {secondary_dc: 0})
        http.wait_until_blueprint_satisfied(ns)
        cluster.check()
        http.check_no_issues()
        workload.run_after()

    http.check_no_issues()
    cluster.check_and_stop()
