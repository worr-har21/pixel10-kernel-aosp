#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

exec tools/bazel run \
    --config=stamp \
    --config=pantah \
    //private/devices/google/pantah:gs201_pantah_dist "$@"
