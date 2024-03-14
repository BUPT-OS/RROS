#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""generate_rust_analyzer - Generates the `rust-project.json` file for `rust-analyzer`.
"""

import argparse
import json
import logging
import os
import pathlib
import sys

def args_crates_cfgs(cfgs):
    crates_cfgs = {}
    for cfg in cfgs:
        crate, vals = cfg.split("=", 1)
        crates_cfgs[crate] = vals.replace("--cfg", "").split()

    return crates_cfgs

def generate_crates(srctree, objtree, sysroot_src, external_src, cfgs):
    # Generate the configuration list.
    cfg = []
    with open(objtree / "include" / "generated" / "rustc_cfg") as fd:
        for line in fd:
            line = line.replace("--cfg=", "")
            line = line.replace("\n", "")
            cfg.append(line)

    # Now fill the crates list -- dependencies need to come first.
    #
    # Avoid O(n^2) iterations by keeping a map of indexes.
    crates = []
    crates_indexes = {}
    crates_cfgs = args_crates_cfgs(cfgs)

    def append_crate(display_name, root_module, deps, cfg=[], is_workspace_member=True, is_proc_macro=False):
        crates_indexes[display_name] = len(crates)
        crates.append({
            "display_name": display_name,
            "root_module": str(root_module),
            "is_workspace_member": is_workspace_member,
            "is_proc_macro": is_proc_macro,
            "deps": [{"crate": crates_indexes[dep], "name": dep} for dep in deps],
            "cfg": cfg,
            "edition": "2021",
            "env": {
                "RUST_MODFILE": "This is only for rust-analyzer"
            }
        })

    # First, the ones in `rust/` since they are a bit special.
    append_crate(
        "core",
        sysroot_src / "core" / "src" / "lib.rs",
        [],
        cfg=crates_cfgs.get("core", []),
        is_workspace_member=False,
    )

    append_crate(
        "compiler_builtins",
        srctree / "rust" / "compiler_builtins.rs",
        [],
    )

    append_crate(
        "alloc",
        srctree / "rust" / "alloc" / "lib.rs",
        ["core", "compiler_builtins"],
        cfg=crates_cfgs.get("alloc", []),
    )

    append_crate(
        "macros",
        srctree / "rust" / "macros" / "lib.rs",
        [],
        is_proc_macro=True,
    )
    crates[-1]["proc_macro_dylib_path"] = f"{objtree}/rust/libmacros.so"

    append_crate(
        "build_error",
        srctree / "rust" / "build_error.rs",
        ["core", "compiler_builtins"],
    )

    append_crate(
        "bindings",
        srctree / "rust"/ "bindings" / "lib.rs",
        ["core"],
        cfg=cfg,
    )
    crates[-1]["env"]["OBJTREE"] = str(objtree.resolve(True))

    append_crate(
        "kernel",
        srctree / "rust" / "kernel" / "lib.rs",
        ["core", "alloc", "macros", "build_error", "bindings"],
        cfg=cfg,
    )
    crates[-1]["source"] = {
        "include_dirs": [
            str(srctree / "rust" / "kernel"),
            str(objtree / "rust")
        ],
        "exclude_dirs": [],
    }

    def is_root_crate(build_file, target):
        try:
            return f"{target}.o" in open(build_file).read()
        except FileNotFoundError:
            return False

    # Then, the rest outside of `rust/`.
    #
    # We explicitly mention the top-level folders we want to cover.
    extra_dirs = map(lambda dir: srctree / dir, ("samples", "drivers"))
    if external_src is not None:
        extra_dirs = [external_src]
    for folder in extra_dirs:
        for path in folder.rglob("*.rs"):
            logging.info("Checking %s", path)
            name = path.name.replace(".rs", "")

            # Skip those that are not crate roots.
            if not is_root_crate(path.parent / "Makefile", name) and \
               not is_root_crate(path.parent / "Kbuild", name):
                continue

            logging.info("Adding %s", name)
            append_crate(
                name,
                path,
                ["core", "alloc", "kernel"],
                cfg=cfg,
            )

    return crates

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--verbose', '-v', action='store_true')
    parser.add_argument('--cfgs', action='append', default=[])
    parser.add_argument("srctree", type=pathlib.Path)
    parser.add_argument("objtree", type=pathlib.Path)
    parser.add_argument("sysroot_src", type=pathlib.Path)
    parser.add_argument("exttree", type=pathlib.Path, nargs="?")
    args = parser.parse_args()

    logging.basicConfig(
        format="[%(asctime)s] [%(levelname)s] %(message)s",
        level=logging.INFO if args.verbose else logging.WARNING
    )

    rust_project = {
        "crates": generate_crates(args.srctree, args.objtree, args.sysroot_src, args.exttree, args.cfgs),
        "sysroot_src": str(args.sysroot_src),
    }

    json.dump(rust_project, sys.stdout, sort_keys=True, indent=4)

if __name__ == "__main__":
    main()
