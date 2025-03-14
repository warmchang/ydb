# -*- coding: utf-8 -*-

from ydb.tests.library.harness.kikimr_runner import KiKiMR
from ydb.tests.oss.canonical import set_canondata_root
from ydb.tests.oss.ydb_sdk_import import ydb

import os
import logging
import pytest

import yatest

logger = logging.getLogger(__name__)


def ydb_bin():
    if os.getenv("YDB_CLI_BINARY"):
        return yatest.common.binary_path(os.getenv("YDB_CLI_BINARY"))
    raise RuntimeError("YDB_CLI_BINARY enviroment variable is not specified")


def upsert_simple(session, full_path):
    path, table = os.path.split(full_path)
    session.transaction().execute(
        """
        PRAGMA TablePathPrefix("{0}");
        UPSERT INTO {1} (`key`, `id`, `value`) VALUES (1, 1111, "one");
        UPSERT INTO {1} (`key`, `id`, `value`) VALUES (2, 2222, "two");
        UPSERT INTO {1} (`key`, `id`, `value`) VALUES (3, 3333, "three");
        UPSERT INTO {1} (`key`, `id`, `value`) VALUES (5, 5555, "five");
        UPSERT INTO {1} (`key`, `id`, `value`) VALUES (7, 7777, "seven");
        """.format(path, table),
        commit_tx=True,
    )


def create_table_with_data(session, path):
    session.create_table(
        path,
        ydb.TableDescription()
        .with_column(ydb.Column("key", ydb.OptionalType(ydb.PrimitiveType.Uint32)))
        .with_column(ydb.Column("id", ydb.OptionalType(ydb.PrimitiveType.Uint64)))
        .with_column(ydb.Column("value", ydb.OptionalType(ydb.PrimitiveType.String)))
        .with_primary_keys("key")
    )

    upsert_simple(session, path)


class BaseTestTableService(object):
    @classmethod
    def setup_class(cls):
        set_canondata_root('ydb/tests/functional/ydb_cli/canondata')

        cls.cluster = KiKiMR()
        cls.cluster.start()
        cls.root_dir = "/Root"
        driver_config = ydb.DriverConfig(
            database="/Root",
            endpoint="%s:%s" % (cls.cluster.nodes[1].host, cls.cluster.nodes[1].port))
        cls.driver = ydb.Driver(driver_config)
        cls.driver.wait(timeout=4)

    @classmethod
    def teardown_class(cls):
        if hasattr(cls, 'cluster'):
            cls.cluster.stop()

    @classmethod
    def execute_ydb_cli_command(cls, args, stdin=None):
        execution = yatest.common.execute(
            [
                ydb_bin(),
                "--endpoint", "grpc://localhost:%d" % cls.cluster.nodes[1].grpc_port,
                "--database", cls.root_dir
            ] +
            args, stdin=stdin
        )

        result = execution.std_out
        logger.debug("std_out:\n" + result.decode('utf-8'))
        return result

    @staticmethod
    def canonical_result(output_result, tmp_path):
        with (tmp_path / "result.output").open("w") as f:
            f.write(output_result.decode('utf-8'))
        return yatest.common.canonical_file(str(tmp_path / "result.output"), local=True, universal_lines=True)


class TestExecuteQueryWithParams(BaseTestTableService):

    @classmethod
    def setup_class(cls):
        BaseTestTableService.setup_class()
        cls.session = cls.driver.table_client.session().create()

    @pytest.fixture(autouse=True, scope='function')
    def init_test(self, tmp_path):
        self.tmp_path = tmp_path
        self.table_path = self.root_dir + "/" + self.tmp_path.name
        create_table_with_data(self.session, self.table_path)

    def test_uint32(self):
        query = "DECLARE $par1 AS Uint32; SELECT * FROM `{}` WHERE key = $par1;".format(self.table_path)
        output = self.execute_ydb_cli_command(["table", "query", "execute", "-q", query, "--param", "$par1=1"])
        return self.canonical_result(output, self.tmp_path)

    def test_uint64_and_string(self):
        query = "DECLARE $id AS Uint64; "\
                "DECLARE $value AS String; "\
                "SELECT * FROM `{}` WHERE id = $id OR value = $value;".format(self.table_path)
        output = self.execute_ydb_cli_command(["table", "query", "execute", "-q", query, "--param", "$id=2222",
                                               "--param", "$value=\"seven\""])
        return self.canonical_result(output, self.tmp_path)

    def test_list(self):
        query = "DECLARE $values AS List<Uint64?>; SELECT $values AS values;"
        output = self.execute_ydb_cli_command(["table", "query", "execute", "-q", query, "--param", "$values=[1,2,3]"])
        return self.canonical_result(output, self.tmp_path)

    def test_struct(self):
        query = "DECLARE $values AS List<Struct<key:Uint64, value:Utf8>>; "\
                "SELECT "\
                "Table.key AS key, "\
                "Table.value AS value "\
                "FROM (SELECT $values AS lst) FLATTEN BY lst AS Table;"
        output = self.execute_ydb_cli_command(["table", "query", "execute", "-q", query, "--param",
                                               "$values=[{\"key\":1,\"value\":\"one\"},{\"key\":2,\"value\":\"two\"}]"])
        return self.canonical_result(output, self.tmp_path)

    def test_scan_query_with_parameters(self):
        query = "DECLARE $id AS Uint64; "\
                "DECLARE $value AS String; "\
                "SELECT * FROM `{}` WHERE id = $id OR value = $value;".format(self.table_path)
        output = self.execute_ydb_cli_command(["table", "query", "execute", "-t", "scan", "-q", query,
                                               "--param", "$id=2222",
                                               "--param", "$value=\"seven\""])
        return self.canonical_result(output, self.tmp_path)


class TestExecuteQueryWithFormats(BaseTestTableService):

    @classmethod
    def setup_class(cls):
        BaseTestTableService.setup_class()
        cls.session = cls.driver.table_client.session().create()

    @pytest.fixture(autouse=True, scope='function')
    def init_test(self, tmp_path):
        self.tmp_path = tmp_path
        self.table_path = self.root_dir + "/" + self.tmp_path.name
        create_table_with_data(self.session, self.table_path)

    def execute_data_query(self, format):
        query = "SELECT * FROM `{}` WHERE key < 4;".format(self.table_path)
        output = self.execute_ydb_cli_command(["table", "query", "execute", "-t", "data", "-q", query,
                                               "--format", format])
        return self.canonical_result(output, self.tmp_path)

    def execute_scan_query(self, format):
        query = "SELECT * FROM `{}` WHERE key < 4;".format(self.table_path)
        output = self.execute_ydb_cli_command(["table", "query", "execute", "-t", "scan", "-q", query,
                                               "--format", format])
        return self.canonical_result(output, self.tmp_path)

    def execute_read_table(self, format):
        output = self.execute_ydb_cli_command(["table", "readtable", self.table_path, "--format", format])
        return self.canonical_result(output, self.tmp_path)

    # DataQuery

    def test_data_query_pretty(self):
        return self.execute_data_query('pretty')

    def test_data_query_json_base64(self):
        return self.execute_data_query('json-base64')

    def test_data_query_json_base64_array(self):
        return self.execute_data_query('json-base64-array')

    def test_data_query_json_unicode(self):
        return self.execute_data_query('json-unicode')

    def test_data_query_json_unicode_array(self):
        return self.execute_data_query('json-unicode-array')

    def test_data_query_csv(self):
        return self.execute_data_query('csv')

    def test_data_query_tsv(self):
        return self.execute_data_query('tsv')

    # ScanQuery

    def test_scan_query_pretty(self):
        return self.execute_scan_query('pretty')

    def test_scan_query_json_base64(self):
        return self.execute_scan_query('json-base64')

    def test_scan_query_json_base64_array(self):
        return self.execute_scan_query('json-base64-array')

    def test_scan_query_json_unicode(self):
        return self.execute_scan_query('json-unicode')

    def test_scan_query_json_unicode_array(self):
        return self.execute_scan_query('json-unicode-array')

    def test_scan_query_csv(self):
        return self.execute_scan_query('csv')

    def test_scan_query_tsv(self):
        return self.execute_scan_query('tsv')

    # ReadTable

    def test_read_table_pretty(self):
        return self.execute_read_table('pretty')

    def test_read_table_json_base64(self):
        return self.execute_read_table('json-base64')

    def test_read_table_json_base64_array(self):
        return self.execute_read_table('json-base64-array')

    def test_read_table_json_unicode(self):
        return self.execute_read_table('json-unicode')

    def test_read_table_json_unicode_array(self):
        return self.execute_read_table('json-unicode-array')

    def test_read_table_csv(self):
        return self.execute_read_table('csv')

    def test_read_table_tsv(self):
        return self.execute_read_table('tsv')


@pytest.mark.parametrize("query_type", ["data", "scan"])
class TestExecuteQueryWithParamsFromJson(BaseTestTableService):
    @classmethod
    def setup_class(cls):
        BaseTestTableService.setup_class()
        cls.session = cls.driver.table_client.session().create()

    @pytest.fixture(autouse=True, scope='function')
    def init_test(self, tmp_path):
        self.tmp_path = tmp_path
        self.table_path = self.root_dir + "/" + self.tmp_path.name
        create_table_with_data(self.session, self.table_path)

    @staticmethod
    def write_data(data, filename):
        with open(filename, "w") as file:
            file.write(data)

    def uint32(self, query_type):
        param_data = '{\n' \
            '   "par1": 1\n' \
            '}'
        query = "DECLARE $par1 AS Uint32; SELECT * FROM `{}` WHERE key = $par1;".format(self.table_path)
        self.write_data(param_data, str(self.tmp_path / "params.json"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "--param-file", str(self.tmp_path / "params.json"), "-q", query]
        )
        return self.canonical_result(output, self.tmp_path)

    def uint64_and_string(self, query_type):
        param_data = '{\n' \
            '   "value": "seven",\n' \
            '   "id": 2222' \
            '}'
        query = "DECLARE $id AS Uint64; "\
                "DECLARE $value AS String; "\
                "SELECT * FROM `{}` WHERE id = $id OR value = $value;".format(self.table_path)
        self.write_data(param_data, str(self.tmp_path / "params.json"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "--param-file", str(self.tmp_path / "params.json"), "-q", query]
        )
        return self.canonical_result(output, self.tmp_path)

    def list(self, query_type):
        param_data = '{\n' \
            '   "values": [1, 2, 3]\n' \
            '}'
        query = "DECLARE $values AS List<Uint64?>; SELECT $values AS values;"
        self.write_data(param_data, str(self.tmp_path / "params.json"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "--param-file", str(self.tmp_path / "params.json"), "-q", query]
        )
        return self.canonical_result(output, self.tmp_path)

    def struct(self, query_type):
        param_data = '{\n' \
            '   "values": [\n' \
            '       {\n'\
            '           "key": 1,\n'\
            '           "value": "one"\n'\
            '       },\n'\
            '       {\n'\
            '           "key": 2,\n'\
            '           "value": "two"\n'\
            '       }\n'\
            '   ]\n'\
            '}'
        query = "DECLARE $values AS List<Struct<key:Uint64, value:Utf8>>; "\
                "SELECT "\
                "Table.key AS key, "\
                "Table.value AS value "\
                "FROM (SELECT $values AS lst) FLATTEN BY lst AS Table;"
        self.write_data(param_data, str(self.tmp_path / "params.json"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "--param-file", str(self.tmp_path / "params.json"), "-q", query]
        )
        return self.canonical_result(output, self.tmp_path)

    def multiple_files(self, query_type):
        param_data1 = '{\n' \
            '   "str": "Строчка"\n' \
            '}'
        param_data2 = '{\n' \
            '   "num": 1542\n' \
            '}'
        param_data3 = '{\n' \
            '   "date": "2011-11-11"\n' \
            '}'
        query = "DECLARE $str AS Utf8; "\
                "DECLARE $num AS Uint64; "\
                "DECLARE $date AS Date; "\
                "SELECT $str AS str, $num as num, $date as date; "
        self.write_data(param_data1, str(self.tmp_path / "param1.json"))
        self.write_data(param_data2, str(self.tmp_path / "param2.json"))
        self.write_data(param_data3, str(self.tmp_path / "param3.json"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "--param-file", str(self.tmp_path / "param1.json"), "--param-file",
             str(self.tmp_path / "param2.json"), "--param-file", str(self.tmp_path / "param3.json"), "-q", query]
        )
        return self.canonical_result(output, self.tmp_path)

    def ignore_excess_parameters(self, query_type):
        param_data = '{\n' \
            '   "a": 12,\n' \
            '   "b": 34' \
            '}'
        query = "DECLARE $a AS Uint64; " \
                "SELECT $a AS a; "
        self.write_data(param_data, str(self.tmp_path / "params.json"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "-q", query, "--param-file", str(self.tmp_path / "params.json")]
        )
        return self.canonical_result(output, self.tmp_path)

    def script_from_file(self, query_type):
        query = "DECLARE $a AS Uint64; " \
                "SELECT $a AS a; "
        self.write_data(query, str(self.tmp_path / "query.yql"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "-f", str(self.tmp_path / "query.yql"), "--param", "$a=3"]
        )
        return self.canonical_result(output, self.tmp_path)

    def test_uint32(self, query_type):
        return self.uint32(query_type)

    def test_uint64_and_string(self, query_type):
        return self.uint64_and_string(query_type)

    def test_list(self, query_type):
        return self.list(query_type)

    def test_struct(self, query_type):
        return self.struct(query_type)

    def test_multiple_files(self, query_type):
        return self.multiple_files(query_type)

    def test_ignore_excess_parameters(self, query_type):
        return self.ignore_excess_parameters(query_type)

    def test_script_from_file(self, query_type):
        return self.script_from_file(query_type)


@pytest.mark.parametrize("query_type", ["data", "scan"])
class TestExecuteQueryWithParamsFromStdin(BaseTestTableService):
    @classmethod
    def setup_class(cls):
        BaseTestTableService.setup_class()
        cls.session = cls.driver.table_client.session().create()

    @pytest.fixture(autouse=True, scope='function')
    def init_test(self, tmp_path):
        self.tmp_path = tmp_path
        self.table_path = self.root_dir + "/" + self.tmp_path.name
        create_table_with_data(self.session, self.table_path)

    @staticmethod
    def write_data(data, filename):
        with open(filename, "w") as file:
            file.write(data)

    @staticmethod
    def get_delim(format):
        if format == "csv":
            return ","
        elif format == "tsv":
            return "\t"
        raise RuntimeError("Unknown format: {}".format(format))

    def get_stdin(self):
        self.stdin = (self.tmp_path / "stdin.txt").open("r")
        return self.stdin

    def close_stdin(self):
        self.stdin.close()

    def simple_json(self, query_type):
        param_data = '{\n' \
            '   "s": "Some_string",\n' \
            '   "val": 32\n' \
            '}'
        query = "DECLARE $s AS Utf8; "\
                "DECLARE $val AS Uint64; "\
                "SELECT $s AS s, $val AS val; "
        self.write_data(param_data, str(self.tmp_path / "stdin.txt"))
        output = self.execute_ydb_cli_command(["table", "query", "execute", "-t", query_type, "-q", query], self.get_stdin())
        self.close_stdin()
        return self.canonical_result(output, self.tmp_path)

    def simple_csv_tsv(self, query_type, format):
        param_data = 's{0}val\n' \
            '\"Some_s{0}tring\"{0}32'
        param_data = param_data.format(self.get_delim(format))
        query = "DECLARE $s AS Utf8; "\
                "DECLARE $val AS Uint64; "\
                "SELECT $s AS s, $val AS val; "
        self.write_data(param_data, str(self.tmp_path / "stdin.txt"))
        output = self.execute_ydb_cli_command(["table", "query", "execute", "-t", query_type,
                                               "-q", query, "--stdin-format", format], self.get_stdin())
        self.close_stdin()
        return self.canonical_result(output, self.tmp_path)

    def stdin_par_raw(self, query_type):
        param_data = 'Line1\n' \
            'Line2\n' \
            'Line3\n'
        query = "DECLARE $s AS Utf8; " \
                "SELECT $s AS s; "
        self.write_data(param_data, str(self.tmp_path / "stdin.txt"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "-q", query, "--stdin-format", "raw", "--stdin-par", "s"],
            self.get_stdin()
        )
        self.close_stdin()
        return self.canonical_result(output, self.tmp_path)

    def stdin_par_json(self, query_type):
        param_data = "[1, 2, 3, 4]"
        query = "DECLARE $arr AS List<Uint64>; "\
                "SELECT $arr AS arr; "
        self.write_data(param_data, str(self.tmp_path / "stdin.txt"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "-q", query, "--stdin-par", "arr"],
            self.get_stdin()
        )
        self.close_stdin()
        return self.canonical_result(output, self.tmp_path)

    def stdin_par_csv_tsv(self, query_type, format):
        param_data = 'id{0}value\n' \
            '1{0}"ab{0}a"'
        param_data = param_data.format(self.get_delim(format))
        query = "DECLARE $s AS Struct<id:UInt64,value:Utf8>; " \
                "SELECT $s AS s; "
        self.write_data(param_data, str(self.tmp_path / "stdin.txt"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "-q", query, "--stdin-format", format, "--stdin-par", "s"],
            self.get_stdin()
        )
        self.close_stdin()
        return self.canonical_result(output, self.tmp_path)

    def mix_json_and_binary(self, query_type):
        param_data1 = 'Строка номер 1\n' \
            'String number 2\n' \
            'Строка номер 3'
        param_data2 = '{\n' \
            '   "date": "2044-08-21",\n' \
            '   "val": 32\n' \
            '}'
        query = "DECLARE $s AS String; " \
                "DECLARE $date AS Date; " \
                "DECLARE $val AS Uint64; " \
                "SELECT $s AS s, $date AS date, $val AS val; "
        self.write_data(param_data1, str(self.tmp_path / "stdin.txt"))
        self.write_data(param_data2, str(self.tmp_path / "params.json"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "-q", query, "--stdin-par", "s",
             "--stdin-format", "raw", "--param-file", str(self.tmp_path / "params.json")],
            self.get_stdin()
        )
        self.close_stdin()
        return self.canonical_result(output, self.tmp_path)

    def different_sources_json(self, query_type):
        param_data1 = '{\n' \
            '   "s": "Строка utf-8"\n' \
            '}'
        param_data2 = '{\n' \
            '   "date": "2000-09-01"\n' \
            '}'
        query = "DECLARE $s AS Utf8; " \
                "DECLARE $date AS Date; " \
                "DECLARE $val AS Uint64; " \
                "SELECT $s AS s, $date AS date, $val AS val; "
        self.write_data(param_data1, str(self.tmp_path / "stdin.txt"))
        self.write_data(param_data2, str(self.tmp_path / "params.json"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "-q", query, "--param-file", str(self.tmp_path / "params.json"), "--param", "$val=100"],
            self.get_stdin()
        )
        self.close_stdin()
        return self.canonical_result(output, self.tmp_path)

    def different_sources_csv_tsv(self, query_type, format):
        param_data1 = 's\n' \
            '\"Some_s{0}tring\"'
        param_data1 = param_data1.format(self.get_delim(format))
        param_data2 = '{\n' \
            '   "date": "2000-09-01"\n' \
            '}'
        query = "DECLARE $s AS Utf8; " \
                "DECLARE $date AS Date; " \
                "DECLARE $val AS Uint64; " \
                "SELECT $s AS s, $date AS date, $val AS val; "
        self.write_data(param_data1, str(self.tmp_path / "stdin.txt"))
        self.write_data(param_data2, str(self.tmp_path / "params.json"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "-q", query, "--stdin-format", format, "--param-file", str(self.tmp_path / "params.json"), "--param", "$val=100"],
            self.get_stdin()
        )
        self.close_stdin()
        return self.canonical_result(output, self.tmp_path)

    def framing_newline_delimited_json(self, query_type):
        param_data = '{"s": "Some text", "num": 1}\n' \
            '{"s": "Строка 1\\nСтрока2", "num": 2}\n' \
            '{"s": "Abacaba", "num": 3}\n'
        query = "DECLARE $s AS Utf8; " \
                "DECLARE $num AS Uint64; " \
                "SELECT $s AS s, $num AS num; "
        self.write_data(param_data, str(self.tmp_path / "stdin.txt"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "-q", query, "--stdin-format", "newline-delimited"],
            self.get_stdin()
        )
        self.close_stdin()
        return self.canonical_result(output, self.tmp_path)

    def framing_newline_delimited_csv_tsv(self, query_type, format):
        param_data = 's{0}num\n' \
            'Some text{0}1\n' \
            '"Строка 1\nСтрока2"{0}2\n' \
            'Abacaba{0}3\n'
        param_data = param_data.format(self.get_delim(format))
        query = "DECLARE $s AS Utf8; " \
                "DECLARE $num AS Uint64; " \
                "SELECT $s AS s, $num AS num; "
        self.write_data(param_data, str(self.tmp_path / "stdin.txt"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "-q", query, "--stdin-format", format, "--stdin-format", "newline-delimited"],
            self.get_stdin()
        )
        self.close_stdin()
        return self.canonical_result(output, self.tmp_path)

    def framing_newline_delimited_raw(self, query_type):
        param_data = 'Line1\n' \
            'Line2\n' \
            'Line3\n'
        query = "DECLARE $s AS Utf8; " \
                "SELECT $s AS s; "
        self.write_data(param_data, str(self.tmp_path / "stdin.txt"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "-q", query, "--stdin-format", "raw",
             "--stdin-par", "s", "--stdin-format", "newline-delimited"],
            self.get_stdin()
        )
        self.close_stdin()
        return self.canonical_result(output, self.tmp_path)

    def batching_full_raw(self, query_type):
        param_data = 'Line1\n' \
            'Line2\n' \
            'Line3\n'
        query = "DECLARE $s AS List<Utf8>; " \
                "SELECT $s AS s; "
        self.write_data(param_data, str(self.tmp_path / "stdin.txt"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "-q", query, "--stdin-format", "raw", "--stdin-par", "s",
             "--stdin-format", "newline-delimited", "--batch", "full"],
            self.get_stdin()
        )
        self.close_stdin()
        return self.canonical_result(output, self.tmp_path)

    def batching_full_json(self, query_type):
        param_data = '{"s": "Line1", "id": 1}\n' \
            '{"s": "Line2", "id": 2}\n' \
            '{"s": "Line3", "id": 3}\n' \
            '{"s": "Line4", "id": 4}\n' \
            '{"s": "Line5", "id": 5}\n' \
            '{"s": "Line6", "id": 6}\n' \
            '{"s": "Line7", "id": 7}\n' \
            '{"s": "Line8", "id": 8}\n' \
            '{"s": "Line9", "id": 9}\n'
        query = "DECLARE $arr as List<Struct<s:Utf8, id:Uint64>>; " \
                "SELECT $arr as arr; "
        self.write_data(param_data, str(self.tmp_path / "stdin.txt"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "-q", query, "--stdin-par", "arr",
             "--stdin-format", "newline-delimited", "--batch", "full"],
            self.get_stdin()
        )
        self.close_stdin()
        return self.canonical_result(output, self.tmp_path)

    def batching_full_csv_tsv(self, query_type, format):
        param_data = 's{0}id\n' \
            'Line1{0}1\n' \
            'Line2{0}2\n' \
            'Line3{0}3\n' \
            'Line4{0}4\n' \
            'Line5{0}5\n' \
            'Line6{0}6\n' \
            'Line7{0}7\n' \
            'Line8{0}8\n' \
            'Line9{0}9'
        param_data = param_data.format(self.get_delim(format))
        query = "DECLARE $arr as List<Struct<s:Utf8, id:Uint64>>; " \
                "SELECT $arr as arr; "
        self.write_data(param_data, str(self.tmp_path / "stdin.txt"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "-q", query, "--stdin-format", format, "--stdin-par", "arr",
             "--stdin-format", "newline-delimited", "--batch", "full"],
            self.get_stdin()
        )
        self.close_stdin()
        return self.canonical_result(output, self.tmp_path)

    def batching_adaptive_raw(self, query_type):
        param_data = 'Line1\n' \
            'Line2\n' \
            'Line3\n' \
            'Line4\n' \
            'Line5\n' \
            'Line6\n' \
            'Line7\n' \
            'Line8\n' \
            'Line9\n'
        query = "DECLARE $s AS List<Utf8>; " \
                "SELECT $s AS s; "
        self.write_data(param_data, str(self.tmp_path / "stdin.txt"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "-q", query, "--stdin-format", "raw", "--stdin-par", "s",
             "--stdin-format", "newline-delimited", "--batch", "adaptive", "--batch-max-delay", "0", "--batch-limit", "3"],
            self.get_stdin()
        )
        self.close_stdin()
        return self.canonical_result(output, self.tmp_path)

    def batching_adaptive_json(self, query_type):
        param_data = '{"s": "Line1", "id": 1}\n' \
            '{"s": "Line2", "id": 2}\n' \
            '{"s": "Line3", "id": 3}\n' \
            '{"s": "Line4", "id": 4}\n' \
            '{"s": "Line5", "id": 5}\n' \
            '{"s": "Line6", "id": 6}\n' \
            '{"s": "Line7", "id": 7}\n' \
            '{"s": "Line8", "id": 8}\n' \
            '{"s": "Line9", "id": 9}\n'
        query = "DECLARE $arr as List<Struct<s:Utf8, id:Uint64>>; " \
                "SELECT $arr as arr; "
        self.write_data(param_data, str(self.tmp_path / "stdin.txt"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "-q", query, "--stdin-par", "arr", "--stdin-format", "newline-delimited",
             "--batch", "adaptive", "--batch-max-delay", "0", "--batch-limit", "3"],
            self.get_stdin()
        )
        self.close_stdin()
        return self.canonical_result(output, self.tmp_path)

    def batching_adaptive_csv_tsv(self, query_type, format):
        param_data = 's{0}id\n' \
            'Line1{0}1\n' \
            'Line2{0}2\n' \
            'Line3{0}3\n' \
            'Line4{0}4\n' \
            'Line5{0}5\n' \
            'Line6{0}6\n' \
            'Line7{0}7\n' \
            'Line8{0}8\n' \
            'Line9{0}9'
        param_data = param_data.format(self.get_delim(format))
        query = "DECLARE $arr as List<Struct<s:Utf8, id:Uint64>>; " \
                "SELECT $arr as arr; "
        self.write_data(param_data, str(self.tmp_path / "stdin.txt"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "-q", query, "--stdin-format", format, "--stdin-par", "arr", "--stdin-format", "newline-delimited",
             "--batch", "adaptive", "--batch-max-delay", "0", "--batch-limit", "3"],
            self.get_stdin()
        )
        self.close_stdin()
        return self.canonical_result(output, self.tmp_path)

    def ignore_excess_parameters_json(self, query_type):
        param_data = '{\n' \
            '   "a": 12,\n' \
            '   "b": 34' \
            '}'
        query = "DECLARE $a AS Uint64; " \
                "SELECT $a AS a; "
        self.write_data(param_data, str(self.tmp_path / "stdin.txt"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "-q", query],
            self.get_stdin()
        )
        self.close_stdin()
        return self.canonical_result(output, self.tmp_path)

    def ignore_excess_parameters_csv_tsv(self, query_type, format):
        param_data = 'a{0}b\n' \
            '12{0}34\n'
        param_data = param_data.format(self.get_delim(format))
        query = "DECLARE $a AS Uint64; " \
                "SELECT $a AS a; "
        self.write_data(param_data, str(self.tmp_path / "stdin.txt"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "-q", query, "--stdin-format", format],
            self.get_stdin()
        )
        self.close_stdin()
        return self.canonical_result(output, self.tmp_path)

    def columns_bad_header(self, query_type, format):
        param_data = 'x{0}y\n' \
            '1{0}1\n' \
            '2{0}2\n' \
            '3{0}3\n'
        param_data = param_data.format(self.get_delim(format))
        query = "DECLARE $a AS Uint64; " \
                "DECLARE $b AS Uint64; " \
                "SELECT $a AS a, $b AS b; "
        self.write_data(param_data, str(self.tmp_path / "stdin.txt"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "-q", query, "--stdin-format", format, "--stdin-format", "newline-delimited",
             "--columns", "a{0}b".format(self.get_delim(format)), "--skip-rows", "1"],
            self.get_stdin()
        )
        self.close_stdin()
        return self.canonical_result(output, self.tmp_path)

    def columns_no_header(self, query_type, format):
        param_data = '1{0}1\n' \
            '2{0}2\n' \
            '3{0}3\n'
        param_data = param_data.format(self.get_delim(format))
        query = "DECLARE $a AS Uint64; " \
                "DECLARE $b AS Uint64; " \
                "SELECT $a AS a, $b AS b; "
        self.write_data(param_data, str(self.tmp_path / "stdin.txt"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "-q", query, "--stdin-format", format, "--stdin-format", "newline-delimited",
             "--columns", "a{0}b".format(self.get_delim(format))],
            self.get_stdin()
        )
        self.close_stdin()
        return self.canonical_result(output, self.tmp_path)

    def skip_rows(self, query_type, format):
        param_data = 'a{0}b\n' \
            'x{0}x\n' \
            'x{0}x\n' \
            'x{0}x\n' \
            '1{0}1\n' \
            '2{0}2\n' \
            '3{0}3\n'
        param_data = param_data.format(self.get_delim(format))
        query = "DECLARE $a AS Uint64; " \
                "DECLARE $b AS Uint64; " \
                "SELECT $a AS a, $b AS b; "
        self.write_data(param_data, str(self.tmp_path / "stdin.txt"))
        output = self.execute_ydb_cli_command(
            ["table", "query", "execute", "-t", query_type, "-q", query, "--stdin-format", format, "--stdin-format", "newline-delimited",
             "--skip-rows", "3"],
            self.get_stdin()
        )
        self.close_stdin()
        return self.canonical_result(output, self.tmp_path)

    def test_simple_json(self, query_type):
        return self.simple_json(query_type)

    def test_simple_csv(self, query_type):
        return self.simple_csv_tsv(query_type, "csv")

    def test_simple_tsv(self, query_type):
        return self.simple_csv_tsv(query_type, "tsv")

    def test_stdin_par_raw(self, query_type):
        return self.stdin_par_raw(query_type)

    def test_stdin_par_json(self, query_type):
        return self.stdin_par_json(query_type)

    def test_stdin_par_csv(self, query_type):
        return self.stdin_par_csv_tsv(query_type, "csv")

    def test_stdin_par_tsv(self, query_type):
        return self.stdin_par_csv_tsv(query_type, "tsv")

    def test_mix_json_and_binary(self, query_type):
        return self.mix_json_and_binary(query_type)

    def test_different_sources_json(self, query_type):
        return self.different_sources_json(query_type)

    def test_different_sources_csv(self, query_type):
        return self.different_sources_csv_tsv(query_type, "csv")

    def test_different_sources_tsv(self, query_type):
        return self.different_sources_csv_tsv(query_type, "tsv")

    def test_framing_newline_delimited_json(self, query_type):
        return self.framing_newline_delimited_json(query_type)

    def test_framing_newline_delimited_csv(self, query_type):
        return self.framing_newline_delimited_csv_tsv(query_type, "csv")

    def test_framing_newline_delimited_tsv(self, query_type):
        return self.framing_newline_delimited_csv_tsv(query_type, "tsv")

    def test_framing_newline_delimited_raw(self, query_type):
        return self.framing_newline_delimited_raw(query_type)

    def test_batching_full_raw(self, query_type):
        return self.batching_full_raw(query_type)

    def test_batching_full_json(self, query_type):
        return self.batching_full_json(query_type)

    def test_batching_full_csv(self, query_type):
        return self.batching_full_csv_tsv(query_type, "csv")

    def test_batching_full_tsv(self, query_type):
        return self.batching_full_csv_tsv(query_type, "tsv")

    def test_batching_adaptive_raw(self, query_type):
        return self.batching_adaptive_raw(query_type)

    def test_batching_adaptive_json(self, query_type):
        return self.batching_adaptive_json(query_type)

    def test_batching_adaptive_csv(self, query_type):
        return self.batching_adaptive_csv_tsv(query_type, "csv")

    def test_batching_adaptive_tsv(self, query_type):
        return self.batching_adaptive_csv_tsv(query_type, "tsv")

    def test_ignore_excess_parameters_json(self, query_type):
        return self.ignore_excess_parameters_json(query_type)

    def test_ignore_excess_parameters_csv(self, query_type):
        return self.ignore_excess_parameters_csv_tsv(query_type, "csv")

    def test_ignore_excess_parameters_tsv(self, query_type):
        return self.ignore_excess_parameters_csv_tsv(query_type, "tsv")

    def test_columns_bad_header_csv(self, query_type):
        return self.columns_bad_header(query_type, "csv")

    def test_columns_bad_header_tsv(self, query_type):
        return self.columns_bad_header(query_type, "tsv")

    def test_columns_no_header_csv(self, query_type):
        return self.columns_no_header(query_type, "csv")

    def test_columns_no_header_tsv(self, query_type):
        return self.columns_no_header(query_type, "tsv")

    def test_skip_rows_csv(self, query_type):
        return self.skip_rows(query_type, "csv")

    def test_skip_rows_tsv(self, query_type):
        return self.skip_rows(query_type, "tsv")
