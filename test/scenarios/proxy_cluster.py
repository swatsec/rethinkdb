#!/usr/bin/env python
# Copyright 2010-2012 RethinkDB, all rights reserved.
import sys, os, time
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), os.path.pardir, 'common')))
import http_admin, driver, workload_runner, scenario_common
from vcoptparse import *

op = OptParser()
scenario_common.prepare_option_parser_mode_flags(op)
op["workload"] = PositionalArg()
op["timeout"] = IntFlag("--timeout", 600)
opts = op.parse(sys.argv)

with driver.Metacluster() as metacluster:
    cluster = driver.Cluster(metacluster)
    _, command_prefix, serve_options = scenario_common.parse_mode_flags(opts)
    
    print "Starting cluster..."
    serve_files = driver.Files(metacluster, db_path="db", console_output="create-output", command_prefix=command_prefix)
    serve_process = driver.Process(cluster, serve_files, console_output="serve-output", command_prefix=command_prefix, extra_options=serve_options)
    
    # remove --cache-size
    for option in serve_options[:]:
        if option == '--cache-size':
            position = serve_options.index(option)
            serve_options.pop(position)
            if len(serve_options) > position: # we have at least one more option... the cache size
                serve_options.pop(position)
            break # we can only handle one
        elif option.startswith('--cache-size='):
            serve_options.remove(option)
    
    proxy_process = driver.ProxyProcess(cluster, 'proxy-logfile', console_output='proxy-output', command_prefix=command_prefix, extra_options=serve_options)
    processes = [serve_process, proxy_process]
    for process in processes:
        process.wait_until_started_up()

    print "Creating table..."
    http = http_admin.ClusterAccess([("localhost", proxy_process.http_port)])
    dc = http.add_datacenter()
    for server_id in http.servers:
        http.move_server_to_datacenter(server_id, dc)
    ns = scenario_common.prepare_table_for_workload(http, primary=dc)
    http.wait_until_blueprint_satisfied(ns)

    workload_ports = scenario_common.get_workload_ports(ns, [proxy_process])
    workload_runner.run(opts["workload"], workload_ports, opts["timeout"])

    cluster.check_and_stop()
