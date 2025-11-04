#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

exec tools/bazel run \
    --config=stamp \
    --config=tegu \
    //private/devices/google/tegu:zumapro_tegu_dist "$@"
