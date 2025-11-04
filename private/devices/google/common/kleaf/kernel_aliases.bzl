# SPDX-License-Identifier: GPL-2.0-only

"""
Defines helper functions for creating kernel aliases.
"""

load(":kernel_targets.bzl", "KERNEL_TARGETS")

def kernel_aliases(name, flag, packages):
    """Define aliases for kernel targets.

    Args:
        name: Name.
        flag: The flag used to select kernel package.
        packages: Selectable kernel packages.
    """
    for idx, pkg in enumerate(packages):
        native.config_setting(
            name = "{}_{}".format(name, idx),
            flag_values = {
                flag: pkg,
            },
        )

    for target in KERNEL_TARGETS:
        native.alias(
            name = "{}".format(target),
            actual = select({
                ":{}_{}".format(name, idx): "{}:{}".format(pkg, target)
                for idx, pkg in enumerate(packages)
            }),
        )
