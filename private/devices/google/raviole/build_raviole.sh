#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

exec tools/bazel run \
    --config=stamp \
    --config=raviole \
    //private/devices/google/raviole:gs101_raviole_dist "$@"
