#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

exec tools/bazel run \
    --config=stamp \
    --config=tangorpro \
    //private/devices/google/tangorpro:gs201_tangorpro_dist "$@"
