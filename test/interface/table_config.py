#!/usr/bin/env python
# Copyright 2014 RethinkDB, all rights reserved.

from __future__ import print_function

import os, pprint, sys, time

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), os.path.pardir, 'common')))
import driver, scenario_common, utils, vcoptparse

r = utils.import_python_driver()

"""The `interface.table_config` test checks that the special `rethinkdb.table_config` and `rethinkdb.table_status` tables behave as expected."""

op = vcoptparse.OptParser()
scenario_common.prepare_option_parser_mode_flags(op)
opts = op.parse(sys.argv)

with driver.Metacluster() as metacluster:
    cluster1 = driver.Cluster(metacluster)
    _, command_prefix, serve_options = scenario_common.parse_mode_flags(opts)
    
    print("Spinning up two processes...")
    files1 = driver.Files(metacluster, console_output="create-output-1", server_name="a", command_prefix=command_prefix)
    proc1 = driver.Process(cluster1, files1, console_output="serve-output-1", command_prefix=command_prefix, extra_options=serve_options)
    files2 = driver.Files(metacluster, console_output="create-output-2", server_name="b", command_prefix=command_prefix)
    proc2 = driver.Process(cluster1, files2, console_output="serve-output-2", command_prefix=command_prefix, extra_options=serve_options)
    files3 = driver.Files(metacluster, console_output="create-output-3", server_name="never_used", command_prefix=command_prefix)
    proc3 = driver.Process(cluster1, files3, console_output="serve-output-3", command_prefix=command_prefix, extra_options=serve_options)
    proc1.wait_until_started_up()
    proc2.wait_until_started_up()
    proc3.wait_until_started_up()
    cluster1.check()
    conn = r.connect(proc1.host, proc1.driver_port)

    def check_foo_config_matches(expected):
        config = r.table_config("foo").nth(0).run(conn)
        assert config["name"] == "foo" and config["db"] == "test"
        found = config["shards"]
        if len(expected) != len(found):
            return False
        for (e_shard, f_shard) in zip(expected, found):
            if set(e_shard["replicas"]) != set(f_shard["replicas"]):
                return False
            if e_shard["director"] != f_shard["director"]:
                return False
        return True

    def check_status_matches_config():
        config = list(r.db("rethinkdb").table("table_config").run(conn))
        status = list(r.db("rethinkdb").table("table_status").run(conn))
        uuids = set(row["id"] for row in config)
        if not (len(uuids) == len(config) == len(status)):
            return False
        if uuids != set(row["id"] for row in status):
            return False
        for c_row in config:
            s_row = [row for row in status if row["id"] == c_row["id"]][0]
            if c_row["db"] != s_row["db"]:
                return False
            if c_row["name"] != s_row["name"]:
                return False
            c_shards = c_row["shards"]
            s_shards = s_row["shards"]
            # Make sure that servers that have never been involved with the table will
            # never appear in `table_status`. (See GitHub issue #3101.)
            for s_shard in s_shards:
                for doc in s_shard["replicas"]:
                    assert doc["server"] != "never_used"
            if len(s_shards) != len(c_shards):
                return False
            for (s_shard, c_shard) in zip(s_shards, c_shards):
                if set(doc["server"] for doc in s_shard["replicas"]) != \
                        set(c_shard["replicas"]):
                    return False
                if s_shard["director"] != c_shard["director"]:
                    return False
                if any(doc["state"] != "ready" for doc in s_shard["replicas"]):
                    return False
            if not s_row["status"]["ready_for_outdated_reads"]:
                return False
            if not s_row["status"]["ready_for_reads"]:
                return False
            if not s_row["status"]["ready_for_writes"]:
                return False
            if not s_row["status"]["all_replicas_ready"]:
                return False
        return True

    def check_tables_named(names):
        config = list(r.db("rethinkdb").table("table_config").run(conn))
        if len(config) != len(names):
            return False
        for row in config:
            if (row["db"], row["name"]) not in names:
                return False
        return True

    def wait_until(condition):
        try:
            start_time = time.time()
            while not condition():
                time.sleep(1)
                if time.time() > start_time + 10:
                    raise RuntimeError("Out of time")
        except:
            config = list(r.db("rethinkdb").table("table_config").run(conn))
            status = list(r.db("rethinkdb").table("table_status").run(conn))
            print("Something went wrong.\nconfig =")
            pprint.pprint(config)
            print("status =")
            pprint.pprint(status)
            raise

    # Make sure that `never_used` will never be picked as a default for a table by
    # removing the `default` tag.
    res = r.db("rethinkdb").table("server_config") \
           .filter({"name": "never_used"}) \
           .update({"tags": []}) \
           .run(conn)
    assert res["replaced"] == 1, res

    print("Creating a table...")
    r.db_create("test").run(conn)
    r.table_create("foo").run(conn)
    r.table_create("bar").run(conn)
    r.db_create("test2").run(conn)
    r.db("test2").table_create("bar2").run(conn)
    r.table("foo").insert([{"i": i} for i in xrange(10)]).run(conn)
    assert set(row["i"] for row in r.table("foo").run(conn)) == set(xrange(10))

    print("Testing that table_config and table_status are sane...")
    wait_until(lambda: check_tables_named(
        [("test", "foo"), ("test", "bar"), ("test2", "bar2")]))
    wait_until(check_status_matches_config)

    print("Testing that we can move around data by writing to table_config...")
    def test_shards(shards):
        print("Reconfiguring:", {"shards": shards})
        res = r.table_config("foo").update({"shards": shards}).run(conn)
        assert res["errors"] == 0, repr(res)
        wait_until(lambda: check_foo_config_matches(shards))
        wait_until(check_status_matches_config)
        assert set(row["i"] for row in r.table("foo").run(conn)) == set(xrange(10))
        print("OK")
    test_shards(
        [{"replicas": ["a"], "director": "a"}])
    test_shards(
        [{"replicas": ["b"], "director": "b"}])
    test_shards(
        [{"replicas": ["a", "b"], "director": "a"}])
    test_shards(
        [{"replicas": ["a"], "director": "a"},
         {"replicas": ["b"], "director": "b"}])
    test_shards(
        [{"replicas": ["a", "b"], "director": "a"},
         {"replicas": ["a", "b"], "director": "b"}])
    test_shards(
        [{"replicas": ["a"], "director": "a"}])

    print("Testing that table_config rejects invalid input...")
    def test_invalid(conf):
        print("Reconfiguring:", conf)
        res = r.db("rethinkdb").table("table_config").filter({"name": "foo"}) \
               .replace(conf).run(conn)
        assert res["errors"] == 1
        print("Error, as expected")
    test_invalid(r.row.merge({"shards": []}))
    test_invalid(r.row.merge({"shards": "this is a string"}))
    test_invalid(r.row.merge({"shards":
        [{"replicas": ["a"], "director": "a", "extra_key": "extra_value"}]}))
    test_invalid(r.row.merge({"shards": [{"replicas": [], "director": None}]}))
    test_invalid(r.row.merge({"shards": [{"replicas": ["a"], "director": "b"}]}))
    test_invalid(r.row.merge(
        {"shards": [{"replicas": ["a"], "director": "b"},
                    {"replicas": ["b"], "director": "a"}]}))
    test_invalid(r.row.merge({"primary_key": "new_primary_key"}))
    test_invalid(r.row.merge({"db": "new_db"}))
    test_invalid(r.row.merge({"extra_key": "extra_value"}))
    test_invalid(r.row.without("name"))
    test_invalid(r.row.without("primary_key"))
    test_invalid(r.row.without("db"))
    test_invalid(r.row.without("shards"))

    print("Testing that we can rename tables through table_config...")
    res = r.table_config("bar").update({"name": "bar2"}).run(conn)
    assert res["errors"] == 0
    wait_until(lambda: check_tables_named(
        [("test", "foo"), ("test", "bar2"), ("test2", "bar2")]))

    print("Testing that we can't rename a table so as to cause a name collision...")
    res = r.table_config("bar2").update({"name": "foo"}).run(conn)
    assert res["errors"] == 1

    print("Testing that we can create a table through table_config...")
    def test_create(doc, pkey):
        res = r.db("rethinkdb").table("table_config") \
               .insert(doc, return_changes=True).run(conn)
        assert res["errors"] == 0, repr(res)
        assert res["inserted"] == 1, repr(res)
        assert doc["name"] in r.table_list().run(conn)
        assert res["changes"][0]["new_val"]["primary_key"] == pkey
        assert "shards" in res["changes"][0]["new_val"]
        for i in xrange(10):
            try:
                r.table(doc["name"]).insert({}).run(conn)
            except r.RqlRuntimeError:
                time.sleep(1)
            else:
                break
        else:
            raise ValueError("Table took too long to become available")
        rows = list(r.table(doc["name"]).run(conn))
        assert len(rows) == 1 and list(rows[0].keys()) == [pkey]
    test_create({
        "name": "baz",
        "db": "test",
        "primary_key": "frob",
        "shards": [{"replicas": ["a"], "director": "a"}]
        }, "frob")
    test_create({
        "name": "baz2",
        "db": "test",
        "shards": [{"replicas": ["a"], "director": "a"}]
        }, "id")
    test_create({
        "name": "baz3",
        "db": "test"
        }, "id")

    print("Testing that we can delete a table through table_config...")
    res = r.table_config("baz").delete().run(conn)
    assert res["errors"] == 0, repr(res)
    assert res["deleted"] == 1, repr(res)
    assert "baz" not in r.table_list().run(conn)

    print("Testing that identifier_format works...")
    a_uuid = r.db("rethinkdb").table("server_config") \
              .filter({"name": "a"}).nth(0)["id"].run(conn)
    db_uuid = r.db("rethinkdb").table("db_config") \
               .filter({"name": "test"}).nth(0)["id"].run(conn)
    res = r.db("rethinkdb").table("table_config", identifier_format="uuid") \
           .insert({
               "name": "idf_test",
               "db": db_uuid,
               "shards": [{"replicas": [a_uuid], "director": a_uuid}]
               }) \
           .run(conn)
    assert res["inserted"] == 1, repr(res)
    res = r.db("rethinkdb").table("table_config", identifier_format="uuid") \
           .filter({"name": "idf_test"}).nth(0).run(conn)
    assert res["shards"] == [{"replicas": [a_uuid], "director": a_uuid}], repr(res)
    res = r.db("rethinkdb").table("table_config", identifier_format="name") \
           .filter({"name": "idf_test"}).nth(0).run(conn)
    assert res["shards"] == [{"replicas": ["a"], "director": "a"}], repr(res)
    r.table_wait("idf_test").run(conn)
    res = r.db("rethinkdb").table("table_status", identifier_format="uuid") \
           .filter({"name": "idf_test"}).nth(0).run(conn)
    assert res["shards"] == [{
        "replicas": [{"server": a_uuid, "state": "ready"}],
        "director": a_uuid
        }], repr(res)
    res = r.db("rethinkdb").table("table_status", identifier_format="name") \
           .filter({"name": "idf_test"}).nth(0).run(conn)
    assert res["shards"] == [{
        "replicas": [{"server": "a", "state": "ready"}],
        "director": "a"
        }], repr(res)

    cluster1.check_and_stop()
print("Done.")
