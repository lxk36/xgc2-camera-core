#!/usr/bin/env python3
"""Guard the camera-core build manifest contract used by central staging."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import tempfile
from datetime import datetime
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
GENERATOR = SCRIPT_DIR / "xgc2_artifact_manifest.py"
TOP_LEVEL_KEYS = {
    "schema",
    "product",
    "source_sha",
    "version",
    "distribution",
    "architecture",
    "ci",
    "created_at",
    "debs",
}
DEB_KEYS = {"file", "package", "version", "architecture", "sha256", "size"}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def main() -> int:
    spec = importlib.util.spec_from_file_location("camera_manifest", GENERATOR)
    require(spec is not None and spec.loader is not None, "cannot load manifest generator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    with tempfile.TemporaryDirectory(prefix="xgc2-camera-manifest-") as directory:
        root = Path(directory)
        deb_dir = root / "debs"
        output_dir = root / "manifests"
        deb_dir.mkdir()
        deb = deb_dir / "libxgc2-camera-dev_0.1.0-1~focal_amd64.deb"
        deb.write_bytes(b"manifest-contract-fixture")

        fields = {
            "Package": "libxgc2-camera-dev",
            "Version": "0.1.0-1~focal",
            "Architecture": "amd64",
        }
        module.deb_field = lambda _path, field: fields[field]
        destination = module.build_manifest(
            argparse.Namespace(
                deb_dir=str(deb_dir),
                output_dir=str(output_dir),
                product="libxgc2-camera-dev",
                product_version="0.1.0-1",
                distribution="focal",
                architecture="amd64",
                source_sha="a" * 40,
                ci_run_id="12345",
                ci_workflow="ci",
                ci_workflow_ref=(
                    "lxk36/xgc2-camera-core/.github/workflows/ci.yml@refs/heads/main"
                ),
            )
        )
        manifest = json.loads(destination.read_text(encoding="utf-8"))

        require(set(manifest) == TOP_LEVEL_KEYS, "unexpected build manifest fields")
        require(
            manifest["schema"] == "xgc2.build-artifact.v1",
            "build manifest schema regressed",
        )
        require(manifest["version"] == "0.1.0-1", "version field is invalid")
        require(isinstance(manifest["debs"], list) and len(manifest["debs"]) == 1,
                "debs must contain exactly the fixture package")
        entry = manifest["debs"][0]
        require(set(entry) == DEB_KEYS, "unexpected deb manifest fields")
        require(entry["file"] == deb.name, "deb file field is invalid")
        require(entry["package"] == fields["Package"], "deb package field is invalid")
        require(entry["version"] == fields["Version"], "deb version field is invalid")
        require(entry["architecture"] == fields["Architecture"],
                "deb architecture field is invalid")
        require(entry["sha256"] == hashlib.sha256(deb.read_bytes()).hexdigest(),
                "deb SHA256 field is invalid")
        require(entry["size"] == deb.stat().st_size, "deb size field is invalid")
        require(manifest["ci"]["run_id"] == "12345", "CI identity is invalid")
        require(manifest["created_at"].endswith("Z"), "created_at must use UTC")
        datetime.fromisoformat(manifest["created_at"].replace("Z", "+00:00"))

    print("Build artifact manifest contract checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
