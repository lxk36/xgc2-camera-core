#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"

package_name="libxgc2-camera-dev"
package_distribution="${PACKAGE_DISTRIBUTION:-}"
build_dir="${XGC2_CAMERA_BUILD_DIR:-${repo_root}/.ci/build}"
stage_dir="${XGC2_CAMERA_STAGE_DIR:-${repo_root}/.ci/stage}"
output_dir="${XGC2_CAMERA_DEB_OUTPUT_DIR:-${repo_root}/debs}"
pkg_root="${XGC2_CAMERA_PACKAGE_ROOT:-${repo_root}/.ci/pkg/${package_name}}"
arch="$(dpkg --print-architecture)"
multiarch="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"

product_version() {
  sed -n 's/^version:[[:space:]]*//p' \
    "${repo_root}/.xgc2/product.yml" | head -n 1
}

package_base_version="${PACKAGE_BASE_VERSION:-$(product_version)}"
if [[ -z "${package_base_version}" ]]; then
  echo "package version is missing" >&2
  exit 1
fi

if [[ -z "${package_distribution}" && -r /etc/os-release ]]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  package_distribution="${VERSION_CODENAME:-${UBUNTU_CODENAME:-}}"
fi
if [[ -z "${package_distribution}" ]]; then
  echo "PACKAGE_DISTRIBUTION or VERSION_CODENAME is required" >&2
  exit 1
fi

version="${PACKAGE_VERSION:-${package_base_version}~${package_distribution}}"
if [[ "${ALLOW_UNSCOPED_BINARY_DEB_VERSION:-0}" != "1" ]]; then
  case "${version}" in
    *"~${package_distribution}"*|*"+${package_distribution}"*) ;;
    *)
      echo "binary package version ${version} must identify ${package_distribution}" >&2
      exit 1
      ;;
  esac
fi

rm -rf "${build_dir}" "${stage_dir}" "${output_dir}" "${pkg_root}"
mkdir -p "${build_dir}" "${output_dir}"

cmake -S "${repo_root}" -B "${build_dir}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG" \
  -DXGC2_CAMERA_BUILD_TESTING=OFF \
  -DXGC2_CAMERA_BUILD_TOOLS=ON
cmake --build "${build_dir}" -- -j"$(nproc)"
DESTDIR="${stage_dir}" cmake --build "${build_dir}" --target install

mkdir -p "${pkg_root}/DEBIAN" \
  "${pkg_root}/usr/share/doc/${package_name}"
cp -a "${stage_dir}/usr" "${pkg_root}/"

cat >"${pkg_root}/DEBIAN/control" <<EOF
Package: ${package_name}
Version: ${version}
Section: libdevel
Priority: optional
Architecture: ${arch}
Maintainer: XGC2 <apt@example.com>
Depends: libc6, libgcc-s1, libstdc++6
Description: ROS-independent XGC2 V4L2 camera development library
 C++14 headers, shared library, CMake metadata, and inspection utility for
 generic single-plane and multi-plane Linux V4L2 capture.
EOF

cat >"${pkg_root}/DEBIAN/postinst" <<'SH'
#!/bin/sh
set -e
if command -v ldconfig >/dev/null 2>&1; then
  ldconfig
fi
SH
cat >"${pkg_root}/DEBIAN/postrm" <<'SH'
#!/bin/sh
set -e
if command -v ldconfig >/dev/null 2>&1; then
  ldconfig
fi
SH
chmod 0755 "${pkg_root}/DEBIAN/postinst" "${pkg_root}/DEBIAN/postrm"

test -f "${pkg_root}/usr/include/xgc2/camera/camera.hpp"
test -e "${pkg_root}/usr/lib/${multiarch}/libxgc2_camera.so"
test -f "${pkg_root}/usr/lib/${multiarch}/cmake/xgc2_camera/xgc2_cameraConfig.cmake"
test -f "${pkg_root}/usr/lib/${multiarch}/pkgconfig/xgc2-camera.pc"
test -x "${pkg_root}/usr/bin/xgc2-camera-inspect"

find "${pkg_root}" -type d -exec chmod 0755 {} +
find "${pkg_root}" -type f -exec chmod 0644 {} +
chmod 0755 "${pkg_root}/DEBIAN" \
  "${pkg_root}/DEBIAN/postinst" \
  "${pkg_root}/DEBIAN/postrm" \
  "${pkg_root}/usr/bin/xgc2-camera-inspect"
find "${pkg_root}/usr/lib/${multiarch}" -maxdepth 1 -type f \
  -name 'libxgc2_camera.so*' -exec chmod 0755 {} +
find "${pkg_root}/usr/lib/${multiarch}" -maxdepth 1 -type f \
  -name 'libxgc2_camera.so*' -exec strip --strip-unneeded {} + \
  2>/dev/null || true
strip --strip-unneeded "${pkg_root}/usr/bin/xgc2-camera-inspect" \
  2>/dev/null || true

fakeroot dpkg-deb --build "${pkg_root}" \
  "${output_dir}/${package_name}_${version}_${arch}.deb" >/dev/null
dpkg-deb -I "${output_dir}/${package_name}_${version}_${arch}.deb"
echo "Debian artifacts written to ${output_dir}"
