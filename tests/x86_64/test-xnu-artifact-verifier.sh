#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
python3 "$REPO/tests/x86_64/test-xnu-artifact-verifier.py" \
  "$REPO/tools/dev/x86_64/verify-xnu-artifact.py"
