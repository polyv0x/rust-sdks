#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

# A standalone pnpm project — its @livekit/uniffi dependency is a plain `link:`,
# not a pnpm workspace member — so it needs its own copy of the shared
# supply-chain policy rather than inheriting one from a parent workspace.
cp ../pnpm-workspace.common.yaml pnpm-workspace.yaml

pnpm install
pnpm exec tsx index.ts
