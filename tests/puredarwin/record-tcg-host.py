# SPDX-License-Identifier: GPL-2.0-only
"""Write TCG host facts before cross-host input downloads and setup steps."""

import importlib.util
import json
import os
from pathlib import Path
import sys


def load_host_facts():
    source = Path(__file__).with_name("run-tcg-e2e.py")
    spec = importlib.util.spec_from_file_location("run_tcg_e2e", source)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load host facts from {source}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module.host_facts()


def main() -> int:
    runner_temp = Path(os.environ["RUNNER_TEMP"])
    runner_temp.mkdir(parents=True, exist_ok=True)
    label = os.environ.get("XNULINUX_RUNNER_LABEL", "local")
    host = load_host_facts()
    report = {
        "runner_label": label,
        "phase": "preflight_before_downloads",
        "host": host,
        "result": {"status": "not_started", "passed": None},
    }
    (runner_temp / "puredarwin-tcg-boot-report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    memory = (
        f"runner_label={label}\n"
        f"host={host['system']} {host['release']} ({host['architecture']})\n"
        f"processor={host['processor']}\n"
        f"logical_cpus={host['logical_cpus']}\n"
        f"memory_total_bytes={host['memory_total_bytes']}\n"
        f"memory_available_bytes={host['memory_available_bytes']}\n"
    )
    (runner_temp / "puredarwin-tcg-host-memory.txt").write_text(memory, encoding="utf-8")

    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with Path(summary_path).open("a", encoding="utf-8") as summary:
            summary.write(
                "## Host preflight before artifact downloads\n\n"
                f"- Runner: {label}\n"
                f"- Host: {host['system']} {host['release']} ({host['architecture']})\n"
                f"- CPU: {host['processor']} ({host['logical_cpus']} logical CPUs)\n"
                f"- RAM: {host['memory_total_bytes']} total bytes; "
                f"{host['memory_available_bytes']} available bytes\n"
            )
    print(memory, end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
