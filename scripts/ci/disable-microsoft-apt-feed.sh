#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Disable the hosted runner's transient third-party Microsoft APT feed for one ephemeral CI job.
# The needed Ubuntu packages come from Ubuntu's own indexes; this does not change persistent hosts.
set -eu
for source in /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources; do
  [ -e "$source" ] || continue
  if grep -q 'packages\.microsoft\.com' "$source"; then
    sudo mv "$source" "$source.xnulinux-disabled"
  fi
done
