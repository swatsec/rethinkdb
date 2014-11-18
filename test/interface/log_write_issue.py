#!/usr/bin/env python
# Copyright 2010-2012 RethinkDB, all rights reserved.
import sys, os, time, pprint, resource
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), os.path.pardir, 'common')))
import driver, utils, scenario_common
from vcoptparse import *
r = utils.import_python_driver()

op = OptParser()
scenario_common.prepare_option_parser_mode_flags(op)
opts = op.parse(sys.argv)

with driver.Metacluster() as metacluster:
    cluster = driver.Cluster(metacluster)
    _, command_prefix, serve_options = scenario_common.parse_mode_flags(opts)

    print "Creating process files..."
    files = driver.Files(metacluster, db_path="db", server_name="the_server",
        console_output="create-output", command_prefix=command_prefix)

    print "Setting resource limit..."
    size_limit = 10 * 1024 * 1024
    resource.setrlimit(resource.RLIMIT_FSIZE, (size_limit, resource.RLIM_INFINITY))

    print "Spinning up server process (which will inherit resource limit)..."
    process = driver.Process(cluster, files, console_output="log",
        extra_options=serve_options + ["--driver-port", "54323"])
    conn = r.connect(process.host, 54323)
    server_uuid = r.db("rethinkdb").table("server_config").nth(0)["id"].run(conn)

    print "Un-setting resource limit..."
    resource.setrlimit(resource.RLIMIT_FSIZE, (resource.RLIM_INFINITY, resource.RLIM_INFINITY))

    print "Filling log file to {0} bytes...".format(size_limit)
    dummy_10k = ("X"*100 + "\n")*100
    with open(os.path.join(files.db_path, "log_file"), "a") as f:
        while f.tell() < size_limit:
            f.write(dummy_10k)

    # When we need to force the server to try to write to its log file, this will do the
    # job; the server logs every name change.
    def make_server_write_to_log():
        res = r.db("rethinkdb").table("server_config").get(server_uuid) \
           .update({"name": "the_server_2"}).run(conn)
        assert res["errors"] == 0 and res["replaced"] == 1, res
        res = r.db("rethinkdb").table("server_config").get(server_uuid) \
           .update({"name": "the_server"}).run(conn)
        assert res["errors"] == 0 and res["replaced"] == 1, res

    print "Making server try to write to log..."
    make_server_write_to_log()

    print "Checking for an issue..."
    issues = list(r.db("rethinkdb").table("issues").run(conn))
    pprint.pprint(issues)
    assert len(issues) == 1
    assert issues[0]["type"] == "log_write_error"
    assert issues[0]["critical"] == False
    assert issues[0]["info"]["servers"] == ["the_server"]
    assert "File too large" in issues[0]["info"]["message"]

    print "Checking issue with identifier_format='uuid'..."
    issues = list(r.db("rethinkdb").table("issues", identifier_format="uuid").run(conn))
    pprint.pprint(issues)
    assert len(issues) == 1
    assert issues[0]["info"]["servers"] == [server_uuid]

    print "Emptying log file..."
    with open(os.path.join(files.db_path, "log_file"), "a") as f:
        f.truncate(0)

    print "Making server try to write to log..."
    make_server_write_to_log()

    print "Checking that issue is gone..."
    issues = list(r.db("rethinkdb").table("issues").run(conn))
    pprint.pprint(issues)
    assert len(issues) == 0

print "Done."
