# SPDX-License-Identifier: GPL-2.0-only

"""
Merges the UAPI headers from merged_kernel_uapi_headers and ddk_uapi_headers.
"""

load("@bazel_skylib//lib:paths.bzl", "paths")
load("//build/kernel/kleaf:hermetic_tools.bzl", "hermetic_toolchain")

def _merged_uapi_headers(ctx):
    hermetic_tools = hermetic_toolchain.get(ctx)
    out_file = (ctx.outputs.out or
                ctx.actions.declare_file("{}/kernel-uapi-headers.tar.gz".format(ctx.attr.name)))
    inputs = []
    outputs = [out_file]

    if not out_file.path.endswith(".tar.gz"):
        fail("out filename must end with \".tar.gz\"")

    for f in reversed(ctx.files.uapi_headers):
        if f.basename.endswith(".tar.gz"):
            inputs.append(f)

    intermediates_dir = paths.join(
        ctx.bin_dir.path,
        paths.dirname(ctx.build_file_path),
        ctx.attr.name + "_intermediates",
    )

    command = hermetic_tools.setup
    command += """
        # Extract all UAPI headers
        mkdir -p {intermediates_dir}/usr

        all_uapi_headers_archives=({all_uapi_headers_archives})

        # Unpack and repack all archives to combine them
        for archive in "${{all_uapi_headers_archives[@]}}"; do
            tar xf ${{archive}} -C {intermediates_dir}
        done

        tar czf {out_name} -C {intermediates_dir} usr
    """.format(
        intermediates_dir = intermediates_dir,
        all_uapi_headers_archives = " ".join([archive.path for archive in inputs]),
        out_name = out_file.path,
    )

    ctx.actions.run_shell(
        mnemonic = "MergedUAPIHeaders",
        inputs = inputs,
        outputs = outputs,
        tools = hermetic_tools.deps,
        progress_message = "Merging UAPI headers",
        command = command,
    )

    return [DefaultInfo(files = depset(outputs))]

merged_uapi_headers = rule(
    doc = """Merges the UAPI headers from merged_kernel_uapi_headers or ddk_uapi_headers.

If there are conflicts of file names in the source tarballs, files higher in
the uapi_headers list have higher priority.
""",
    implementation = _merged_uapi_headers,
    attrs = {
        "uapi_headers": attr.label_list(
            doc = "A list of labels referring to uapi headers, including merged_kernel_uapi_headers() or ddk_uapi_headers() targets.",
            allow_files = True,
            mandatory = True,
        ),
        "out": attr.output(
            doc = "Output file, defaults to {name}/kernel-uapi-headers.tar.gz",
        ),
    },
    toolchains = [hermetic_toolchain.type],
)
