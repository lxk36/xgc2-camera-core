#!/usr/bin/env python3

"""Create a deterministic release-train manifest for locally built debs."""

import argparse
import hashlib
import json
import pathlib
import subprocess


def deb_field(path: pathlib.Path, field: str) -> str:
    return subprocess.check_output(
        ["dpkg-deb", "-f", str(path), field], text=True
    ).strip()


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def build_manifest(arguments: argparse.Namespace) -> None:
    deb_dir = pathlib.Path(arguments.deb_dir)
    output_dir = pathlib.Path(arguments.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    artifacts = []
    for deb in sorted(deb_dir.glob("*.deb")):
        artifacts.append(
            {
                "filename": deb.name,
                "sha256": sha256(deb),
                "size_bytes": deb.stat().st_size,
                "package": deb_field(deb, "Package"),
                "version": deb_field(deb, "Version"),
                "architecture": deb_field(deb, "Architecture"),
            }
        )
    if not artifacts:
        raise SystemExit(f"no .deb artifacts found in {deb_dir}")

    manifest = {
        "schema": "xgc2.artifact-manifest.v1",
        "product": arguments.product,
        "product_version": arguments.product_version,
        "distribution": arguments.distribution,
        "architecture": arguments.architecture,
        "source_sha": arguments.source_sha,
        "ci": {
            "run_id": arguments.ci_run_id,
            "workflow": arguments.ci_workflow,
            "workflow_ref": arguments.ci_workflow_ref,
        },
        "artifacts": artifacts,
    }
    destination = output_dir / (
        f"{arguments.product}-{arguments.distribution}-"
        f"{arguments.architecture}.json"
    )
    destination.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    build = subparsers.add_parser("build")
    build.add_argument("--deb-dir", required=True)
    build.add_argument("--output-dir", required=True)
    build.add_argument("--product", required=True)
    build.add_argument("--product-version", required=True)
    build.add_argument("--distribution", required=True)
    build.add_argument("--architecture", required=True)
    build.add_argument("--source-sha", required=True)
    build.add_argument("--ci-run-id", required=True)
    build.add_argument("--ci-workflow", required=True)
    build.add_argument("--ci-workflow-ref", required=True)
    build.set_defaults(function=build_manifest)
    arguments = parser.parse_args()
    arguments.function(arguments)


if __name__ == "__main__":
    main()
