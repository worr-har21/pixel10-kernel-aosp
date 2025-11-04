#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

import argparse
import os
import re
import subprocess
import xml.etree.ElementTree

WORKSPACE = os.getcwd()
MANIFEST_BRANCH = "android15-gs-pixel-6.6"

ALLOWED_MODULES = ["powervr", "trusty", "soc", "perf", "iif", "power/mitigation"]
ALLOWED_DEVICES = ["common", "lga", "muzel"]

def acquire_manifest():
    subprocess.check_call([
        "repo",
        "init",
        "-u",
        "{}://partner-android.googlesource.com/kernel-pixel/manifest".format(PROTOCOL),
        "-b",
        MANIFEST_BRANCH
    ], cwd=WORKSPACE)

def fix_manifest():
    tree = xml.etree.ElementTree.parse(os.path.join(WORKSPACE, ".repo", "manifests", "default.xml"))
    root = tree.getroot()
    assert(root.tag == "manifest")

    def apply_allow_list(pattern, allow_list):
        for project in root.findall('project'):
            name = project.attrib['name']
            if pattern in name:
                found = False
                for am in allow_list:
                    if am in name:
                        found = True
                        break
                if not found:
                    root.remove(project)

    apply_allow_list("google-modules", ALLOWED_MODULES)
    apply_allow_list("devices", ALLOWED_DEVICES)

    tree.write(os.path.join(WORKSPACE, ".repo", "manifests", "default.xml"))

def sync_manifest():
    subprocess.check_call([
        "repo",
        "init",
        "-m",
        "default.xml"
    ], cwd=WORKSPACE)
    subprocess.check_call([
        "repo",
        "sync"
    ], cwd=WORKSPACE)

def patch_path(path, patcher):
    with open(path, "r") as f:
        lines = f.readlines()
    with open(path, "w") as f:
        for line in lines:
            patched_line = patcher(line)
            if patched_line is not None:
                f.write(patched_line)

def patch_kernel_module_bzl():
    def patcher(line):
        if "if not" in line and "exclude_kernel_build_module_srcs" in line:
            return None
        if "module_hdrs" in line:
            return None
        return line
    patch_path(os.path.join(
        WORKSPACE,
        "kernel",
        "kleaf",
        "impl",
        "kernel_module.bzl"
    ), patcher)

def patch_unneeded_dependencies():
    paths_to_patch = [
        os.path.join(WORKSPACE, "private", "devices", "google", "lga", "BUILD.bazel"),
        os.path.join(WORKSPACE, "private", "devices", "google", "lga", "Kconfig.ext.lga"),
        os.path.join(WORKSPACE, "private", "devices", "google", "lga", "device.bazelrc"),
        os.path.join(WORKSPACE, "private", "devices", "google", "muzel", "device.bazelrc"),
        os.path.join(WORKSPACE, "private", "devices", "google", "muzel", "BUILD.bazel"),
        os.path.join(WORKSPACE, "private", "devices", "google", "muzel", "Kconfig.ext.muzel"),
        os.path.join(WORKSPACE, "private", "google-modules", "soc", "rdo", "BUILD.bazel"),
        os.path.join(WORKSPACE, "private", "google-modules", "soc", "gs", "BUILD.bazel"),
    ]
    module_re = re.compile(r"private/google-modules/[a-z]")
    def patcher(line):
        if module_re.search(line):
            for m in ALLOWED_MODULES:
                if m in line:
                    return line
            return None
        return line
    for p in paths_to_patch:
        patch_path(p, patcher)

def patch_defconfig():
    defconfig_paths = [
        os.path.join(WORKSPACE, "private", "devices", "google", "lga", "lga_defconfig"),
        os.path.join(WORKSPACE, "private", "devices", "google", "muzel", "muzel_defconfig"),
    ]
    suppressed_configs = [
        "CONFIG_AOC_USB_VENDOR_HOOKS",
        "CONFIG_CODEC3P",
        "CONFIG_DRM_DW_MIPI_CDPHY",
        "CONFIG_DRM_DW_MIPI_DSI2H",
        "CONFIG_DRM_G2D",
        "CONFIG_DRM_VERISILICON",
        "CONFIG_GS_DRM_PANEL_UNIFIED",
        "CONFIG_GS_PANEL_SIMPLE",
        "CONFIG_LWIS",
        "CONFIG_VERISILICON_CHIP_9x00",
        "CONFIG_VERISILICON_DC9400_0x32a",
        "CONFIG_VERISILICON_MIPI_DSI2H",
        "CONFIG_ARM_SMMU_V3_PIXEL",
        "CONFIG_ARM_SMMU_V3_PKVM_PIXEL",
        "CONFIG_AOC_DRIVER",
        "CONFIG_AOC_ALSA_DP_AUDIO",
        "CONFIG_AOC_ALSA_INCALL_CAP_3",
        "CONFIG_AOC_ALSA_USB",
        "CONFIG_AOC_USB_AUDIO_OFFLOAD",
        "CONFIG_CH_EXTENSION",
        "CONFIG_COMMON_PANEL_TEST",
        "CONFIG_CPIF_AP_SUSPEND_DURING_VOICE_CALL",
        "CONFIG_CPIF_PAGE_RECYCLING",
        "CONFIG_CPIF_TP_MONITOR",
        "CONFIG_LINK_DEVICE_PCIE_IOMMU",
        "CONFIG_CP_PKTPROC",
        "CONFIG_CP_PKTPROC_UL",
        "CONFIG_CP_PMIC",
        "CONFIG_CP_THERMAL",
        "CONFIG_DEBUG_PANEL_TEST",
        "CONFIG_DWC_DPTX",
        "CONFIG_DWC_DPTX_AUDIO",
        "CONFIG_EXYNOS_MODEM_IF",
        "CONFIG_GOOGLE_H2OMG",
        "CONFIG_GOOGLE_LOGBUFFER",
        "CONFIG_GOOGLE_VOTABLE",
        "CONFIG_GS_PANEL_S6E3HC4",
        "CONFIG_LINK_DEVICE_PCIE",
        "CONFIG_LINK_DEVICE_PCIE_IOCC",
        "CONFIG_LINK_DEVICE_PCIE_SOC_GOOGLE",
        "CONFIG_METRICS_COLLECTION_FRAMEWORK",
        "CONFIG_MODEM_IF_QOS",
        "CONFIG_QCOM_QBT_HANDLER",
        "CONFIG_SEC_MODEM_S5100",
        "CONFIG_SHM_IPC",
        "CONFIG_TOUCHSCREEN_TBN",
        "CONFIG_TOUCHSCREEN_TBN_AOC_CHANNEL_MODE",
        "CONFIG_TOUCHSCREEN_HEATMAP",
        "CONFIG_TOUCHSCREEN_OFFLOAD",
        "CONFIG_GOOG_TOUCH_INTERFACE",
        "CONFIG_VERISILICON_DC9400_0x316",
        # suppress all USB drivers in soc which depend on bms/misc code
        "CONFIG_TYPEC_FUSB307",
        "CONFIG_USB_PSY",
        "CONFIG_TYPEC_MAX77759",
        "CONFIG_TYPEC_MAX77759",
        "CONFIG_TYPEC_MAX77759_CONTAMINANT",
        "CONFIG_TYPEC_MAX77779_CONTAMINANT",
        "CONFIG_TYPEC_MAX777X9_I2C",
        "CONFIG_TYPEC_MAX777X9_SPMI",
        "CONFIG_POGO_TRANSPORT",
        "CONFIG_TYPEC_COOLING_DEV",
        "CONFIG_GOOGLE_USB_ROLE_SW",
        "CONFIG_TCPCI_VENDOR_HOOKS",
        # suppress AOC
        "CONFIG_AOC_LGA",
        # power controller also depends on bms/misc
        "CONFIG_GOOGLE_POWER_CONTROLLER",
        "CONFIG_GOOGLE_ACFW_DEBUG",
        "CONFIG_PIXEL_POWER_REBOOT",
    ]
    def patcher(line):
        for c in suppressed_configs:
            if c in line:
                return None
        return line
    for defconfig_path in defconfig_paths:
        patch_path(defconfig_path, patcher)

def patch_usb_kos():
    def patcher(line):
        if "drivers/usb" in line:
            if not "gadget" in line and not "xhci" in line:
                return None
        return line
    patch_path(os.path.join(
        WORKSPACE,
        "private",
        "devices",
        "google",
        "lga",
        "BUILD.bazel"), patcher)

def patch_drm_ko():
    def patcher(line):
        if "drivers/gpu" in line:
            return None
        return line
    for path in [
        os.path.join(WORKSPACE, "private", "devices", "google", "muzel", "BUILD.bazel"),
        os.path.join(WORKSPACE, "private", "devices", "google", "lga", "constants.bzl"),
    ]:
        patch_path(path, patcher)

def patch_serial_8250():
    def patcher(line):
        if "tty/serial/8250" in line:
            return None
        return line
    patch_path(os.path.join(
        WORKSPACE,
        "private",
        "google-modules",
        "soc",
        "rdo",
        "drivers",
        "Kbuild"), patcher)

def patch_serial_8250_kos():
    def patcher(line):
        if "google_8250" in line:
            return None
        return line
    patch_path(os.path.join(
        WORKSPACE,
        "private",
        "devices",
        "google",
        "lga",
        "BUILD.bazel"), patcher)

def patch_power_controller_ko():
    def patcher(line):
        if "power_controller" in line:
            return None
        return line
    patch_path(os.path.join(
        WORKSPACE,
        "private",
        "devices",
        "google",
        "lga",
        "BUILD.bazel"), patcher)

def check_workspace():
    assert os.path.exists(os.path.join(WORKSPACE, "tools", "bazel"))
    assert os.path.exists(os.path.join(WORKSPACE, "private", "google-modules", "powervr"))
    assert os.path.exists(os.path.join(WORKSPACE, "private", "devices", "google"))

def patch_workspace():
    patch_unneeded_dependencies()
    patch_defconfig()
    patch_usb_kos()
    patch_drm_ko()
    patch_serial_8250()
    patch_serial_8250_kos()
    patch_power_controller_ko()

def build_workspace():
    subprocess.check_call([
        os.path.join("tools", "bazel"),
        "build",
        "--config=stamp",
        "--config=muzel",
        "//private/google-modules/powervr"
    ], cwd=WORKSPACE)

PARSER = argparse.ArgumentParser(
    description = "Prepare and checkout manifests for building powervr"
)
PARSER.add_argument("mode", choices=["acquire", "fix", "sync", "patch", "build"])
PARSER.add_argument("--workspace", action="store", help="Directory to use as repo root")
PARSER.add_argument("--protocol", action="store", default="https", help="Protocol to use when checking out manifest, e.g. {https, sso}")
ARGS = PARSER.parse_args()

if ARGS.workspace is not None:
    WORKSPACE = ARGS.workspace
    if not os.path.isdir(WORKSPACE):
        os.mkdir(WORKSPACE)

PROTOCOL = ARGS.protocol

match ARGS.mode:
    case "acquire":
        acquire_manifest()
    case "fix":
        fix_manifest()
    case "sync":
        sync_manifest()
    case "patch":
        check_workspace()
        patch_workspace()
    case "build":
        check_workspace()
        build_workspace()
