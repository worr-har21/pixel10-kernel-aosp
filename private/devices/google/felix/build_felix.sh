#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

exec tools/bazel run \
    --config=stamp \
    --config=felix \
    //private/devices/google/felix:gs201_felix_dist "$@"
