# SPDX-License-Identifier: GPL-2.0-only

"""
Declare targets for a kernel package with sources and prebuilts
"""

load(":kernel_targets.bzl", "KERNEL_TARGETS")

def _get_prebuilt_target(prebuilt_repos, target):
    actual = {}
    for build, repo in prebuilt_repos.items():
        if target in ["kernel_aarch64", "kernel_aarch64_modules"] or target.endswith(".ko"):
            actual[build] = "{}//kernel_aarch64:{}".format(
                repo,
                target.replace("kernel_aarch64", build),
            )

        if target == "kernel_aarch64_gki_artifacts":
            actual[build] = "{}//kernel_aarch64:{}_signed_gki_artifacts".format(repo, build)

        if target == "kernel_aarch64_images":
            native.filegroup(
                name = "prebuilt_{}/{}".format(build, target),
                srcs = [
                    "{}//system_dlkm.modules.blocklist".format(repo),
                    "{}//system_dlkm.modules.load".format(repo),
                    "{}//system_dlkm_staging_archive.tar.gz".format(repo),
                ],
            )
            actual[build] = ":prebuilt_{}/{}".format(build, target)

        if target == "kernel_aarch64_headers":
            native.filegroup(
                name = "prebuilt_{}/{}".format(build, target),
                srcs = [
                    "{}//kernel-headers.tar.gz".format(repo),
                ],
            )
            actual[build] = ":prebuilt_{}/{}".format(build, target)

    if actual:
        native.alias(
            name = "prebuilt/{}".format(target),
            actual = select({
                "//build/kernel/kleaf:page_size_16k": actual["kernel_aarch64_16k"],
                "//conditions:default": actual["kernel_aarch64"],
            }),
        )
        return ":prebuilt/{}".format(target)

    return None

def prebuilt_kernel_package(name, use_prebuilt_kernel_flag, source_package, prebuilt_repos):
    """Declare targets for a kernel package with sources and prebuilts.

    There can only be one prebuilt_kernel_package in a BUILD.bazel.

    Args:
        name: Name.
        use_prebuilt_kernel_flag: The flag to determine whether to use the prebuilt kernel or not.
        source_package: Kernel source packages, e.g. "@//aosp".
        prebuilt_repos: Prebuilt repo defined using kernel_prebuilt_ext.declare_kernel_prebuilts,
            e.g. { "kernel_aarch64": "@gki_prebuilts", "kernel_aarch64_16k": "@gki_prebuilts_16k" }
    """

    native.config_setting(
        name = "use_prebuilts",
        flag_values = {
            use_prebuilt_kernel_flag: "true",
        },
    )

    for target in KERNEL_TARGETS:
        source_target = "{}:{}".format(source_package, target)
        prebuilt_target = _get_prebuilt_target(prebuilt_repos, target)

        native.alias(
            name = target,
            actual = select({
                ":use_prebuilts": prebuilt_target,
                "//conditions:default": source_target,
            }) if prebuilt_target else source_target,
        )
