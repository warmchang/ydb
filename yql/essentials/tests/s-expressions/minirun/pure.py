import codecs
import os
import pytest
import re
import yql_utils

import yatest.common
from yql_utils import execute, get_tables, get_files, get_http_files, \
    KSV_ATTR, yql_binary_path, is_xfail, is_canonize_peephole, is_peephole_use_blocks, is_canonize_lineage, \
    is_skip_forceblocks, get_param, normalize_source_code_path, replace_vals, get_gateway_cfg_suffix, \
    do_custom_query_check, stable_result_file, stable_table_file, is_with_final_result_issues, \
    normalize_result, get_langver
from yqlrun import YQLRun

from test_utils import get_config, get_parameters_json
from test_file_common import run_file, run_file_no_cache, get_gateways_config, get_sql_query

DEFAULT_LANG_VER = '2025.01'
ASTDIFF_PATH = yql_binary_path('yql/essentials/tools/astdiff/astdiff')
MINIRUN_PATH = yql_binary_path('yql/essentials/tools/minirun/minirun')
DATA_PATH = yatest.common.source_path('yql/essentials/tests/s-expressions/suites')


def run_test(suite, case, cfg, tmpdir, what, yql_http_file_server):
    if get_gateway_cfg_suffix() != '' and what not in ('Results','LLVM'):
        pytest.skip('non-trivial gateways.conf')

    config = get_config(suite, case, cfg, data_path=DATA_PATH)
    langver = get_langver(config)
    if langver is None:
        langver = DEFAULT_LANG_VER

    xfail = is_xfail(config)
    if xfail and what != 'Results':
        pytest.skip('xfail is not supported in this mode')

    program_sql = os.path.join(DATA_PATH, suite, '%s.yqls' % case)
    with codecs.open(program_sql, encoding='utf-8') as program_file_descr:
        sql_query = program_file_descr.read()

    extra_final_args = []
    if is_with_final_result_issues(config):
        extra_final_args += ['--with-final-issues']
    (res, tables_res) = run_file('pure', suite, case, cfg, config, yql_http_file_server, MINIRUN_PATH,
                                 extra_args=extra_final_args, allow_llvm=False, data_path=DATA_PATH,
                                 run_sql=False, langver=langver)

    to_canonize = []
    assert not tables_res

    if what == 'Results':
        if xfail:
            return None

        if do_custom_query_check(res, sql_query):
            return None

        if os.path.exists(res.results_file):
            stable_result_file(res)
            to_canonize.append(yatest.common.canonical_file(res.results_file))
        if res.std_err:
            to_canonize.append(normalize_source_code_path(res.std_err))

    if what == 'Debug':
        to_canonize = [yatest.common.canonical_file(res.opt_file, diff_tool=ASTDIFF_PATH)]

    if what == 'RunOnOpt' or what == 'LLVM':
        is_llvm = (what == 'LLVM')
        files = get_files(suite, config, DATA_PATH)
        http_files = get_http_files(suite, config, DATA_PATH)
        http_files_urls = yql_http_file_server.register_files({}, http_files)
        parameters = get_parameters_json(suite, config, DATA_PATH)
        query_sql = get_sql_query('pure', suite, case, config, DATA_PATH, template='.yqls') if is_llvm else None

        yqlrun = YQLRun(
            prov='pure',
            keep_temp=False,
            gateway_config=get_gateways_config(http_files, yql_http_file_server, allow_llvm=is_llvm),
            udfs_dir=yql_binary_path('yql/essentials/tests/common/test_framework/udfs_deps'),
            binary=MINIRUN_PATH,
            langver=langver
        )

        opt_res, opt_tables_res = execute(
            yqlrun,
            program=res.opt if not is_llvm else query_sql,
            run_sql=False,
            files=files,
            urls=http_files_urls,
            check_error=True,
            verbose=True,
            parameters=parameters)

        assert os.path.exists(opt_res.results_file) == os.path.exists(res.results_file)
        assert not opt_tables_res

        if os.path.exists(res.results_file):
            base_res_yson = normalize_result(stable_result_file(res), False)
            opt_res_yson = normalize_result(stable_result_file(opt_res), False)

            # Compare results
            assert opt_res_yson == base_res_yson, 'RESULTS_DIFFER\n' \
                'Result:\n %(opt_res_yson)s\n\n' \
                'Base result:\n %(base_res_yson)s\n' % locals()

        return None

    return to_canonize
