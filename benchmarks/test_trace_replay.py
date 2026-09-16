from pathlib import Path

from vllm.benchmarks.lib.endpoint_request_func import RequestFuncOutput

import trace_replay


def test_save_request_results_without_optional_ucm_timestamps(
    tmp_path: Path, monkeypatch
):
    output = RequestFuncOutput(
        success=True,
        latency=0.5,
        output_tokens=3,
        ttft=0.1,
        prompt_len=10,
    )
    written = {}

    def capture_sheet(dataframe, excel_file, sheet_name):
        written["dataframe"] = dataframe
        written["excel_file"] = excel_file
        written["sheet_name"] = sheet_name

    monkeypatch.setattr(trace_replay, "write_excel_sheet", capture_sheet)

    trace_replay.save_req_results_to_file([output], str(tmp_path))

    details = written["dataframe"]
    assert written["excel_file"] == str(tmp_path / "metrics.xlsx")
    assert written["sheet_name"] == "details"
    assert details.loc[0, "input_lens"] == 10
    assert details.loc[0, "output_lens"] == 3
    assert details.loc[0, "ttfts_ms"] == 100
    assert details.loc[0, "tpot_ms"] == 200
    assert bool(details.loc[0, "success"])