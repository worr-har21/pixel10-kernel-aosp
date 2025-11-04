#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

exec tools/bazel run \
  --config=stamp \
  --config=rango \
  //private/devices/google/rango:lga_rango_dist "$@"
