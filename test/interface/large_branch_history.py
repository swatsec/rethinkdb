#!/usr/bin/env python
# Copyright 2010-2014 RethinkDB, all rights reserved.

from __future__ import print_function

import sys, os

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), os.path.pardir, 'common')))
import http_admin, driver, scenario_common, vcoptparse

op = vcoptparse.OptParser()
scenario_common.prepare_option_parser_mode_flags(op)
op["num-changes"] = IntFlag("--num-changes", 50)
opts = op.parse(sys.argv)

with driver.Metacluster() as metacluster:
    cluster = driver.Cluster(metacluster)
    _, command_prefix, serve_options = scenario_common.parse_mode_flags(opts)
    print("Starting cluster...")
    processes = [driver.Process(
            cluster,
            driver.Files(metacluster, db_path="db-%d" % i, console_output="create-output-%d" % i, command_prefix=command_prefix),
            console_output="serve-output-%d" % i,
            command_prefix=command_prefix, extra_options=serve_options)
        for i in xrange(2)]
    for process in processes:
        process.wait_until_started_up()

    print("Creating table...")
    http = http_admin.ClusterAccess([("localhost", p.http_port) for p in processes])
    dc1 = http.add_datacenter()
    dc2 = http.add_datacenter()
    http.move_server_to_datacenter(processes[0].files.server_name, dc1)
    http.move_server_to_datacenter(processes[1].files.server_name, dc2)

    primary_dc, secondary_dc = dc1, dc2
    ns = scenario_common.prepare_table_for_workload(http, primary=primary_dc, affinities={primary_dc: 0, secondary_dc: 1})
    http.wait_until_blueprint_satisfied(ns)

    for i in xrange(opts["num-changes"]):
        print("Swap %d..." % i)
        primary_dc, secondary_dc = secondary_dc, primary_dc
        http.move_table_to_datacenter(ns, primary_dc)
        http.set_table_affinities(ns, {primary_dc:0, secondary_dc:1})
        http.wait_until_blueprint_satisfied(ns)
        cluster.check()
        http.check_no_issues()

    cluster.check_and_stop()
