#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

exec tools/bazel run \
    --config=stamp \
    --config=caimito \
    //private/devices/google/caimito:zumapro_caimito_dist "$@"
