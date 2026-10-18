#!/usr/bin/env bash
# Fetch pinned upstream sources and configure the repo for local work.
# Safe to re-run; skips anything already present.
set -euo pipefail
cd "$(dirname "$0")/.."

git config commit.template .gitmessage
echo "commit template -> .gitmessage"

while read -r name url sha _; do
  [[ -z "${name:-}" || "$name" == \#* ]] && continue
  if [[ -d "$name/.git" ]]; then
    echo "$name: present, skipping"
    continue
  fi
  echo "$name: cloning $url @ ${sha:0:7}"
  git clone --filter=blob:none --no-checkout "$url" "$name"
  git -C "$name" checkout --detach "$sha"
done < <(grep -v '^[[:space:]]*#' UPSTREAM.lock | grep -v '^[[:space:]]*$')

echo
echo "Bootstrap complete. The disc image is not fetched by this script --"
echo "place your own dump at skylanders-rom/ (gitignored)."
