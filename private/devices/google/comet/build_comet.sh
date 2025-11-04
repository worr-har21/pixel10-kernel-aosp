#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

exec tools/bazel run \
    --config=stamp \
    --config=comet \
    //private/devices/google/comet:zumapro_comet_dist "$@"
