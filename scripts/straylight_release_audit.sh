#!/usr/bin/env bash
set -euo pipefail

root="${1:-.}"
cd "$root"

fail=0
check() {
  local name="$1"
  local pattern="$2"
  if grep -RInE --exclude=.gitignore --exclude=straylight_release_audit.sh --exclude-dir=.git --exclude-dir=node_modules --exclude-dir=build --exclude-dir=output --exclude-dir=out --exclude-dir=.tmp "$pattern" . >/tmp/straylight-audit-match.txt; then
    echo "[FAIL] $name" >&2
    cat /tmp/straylight-audit-match.txt >&2
    fail=1
  else
    echo "[OK] $name"
  fi
}

slash="/"
personal_path_pattern="${slash}Users${slash}|${slash}home${slash}[A-Za-z0-9._-]+${slash}"

check "personal filesystem paths" "$personal_path_pattern"
check "local IPv4 addresses" '(^|[^0-9])(10\.[0-9]{1,3}\.|192\.168\.|172\.(1[6-9]|2[0-9]|3[0-1])\.)'
check "MAC addresses" '[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}'
check "likely secrets" 'BEGIN (RSA|OPENSSH|EC|DSA) PRIVATE KEY|AKIA[0-9A-Z]{16}|ghp_[A-Za-z0-9_]{20,}|github_pat_|xox[baprs]-[A-Za-z0-9-]{10,}|sk-[A-Za-z0-9]{20,}'

if find . \( -path './.git' -o -path './node_modules' -o -path './build' -o -path './output' -o -path './out' -o -path './.tmp' \) -prune -o \
  \( -name 'authorized_keys' -o -name 'known_hosts' -o -name 'id_rsa' -o -name 'id_rsa.pub' -o -name 'id_ed25519' -o -name 'id_ed25519.pub' \) -print | grep -q .; then
  echo "[FAIL] private SSH material files present" >&2
  find . \( -path './.git' -o -path './node_modules' -o -path './build' -o -path './output' -o -path './out' -o -path './.tmp' \) -prune -o \
    \( -name 'authorized_keys' -o -name 'known_hosts' -o -name 'id_rsa' -o -name 'id_rsa.pub' -o -name 'id_ed25519' -o -name 'id_ed25519.pub' \) -print >&2
  fail=1
else
  echo "[OK] private SSH material files absent"
fi

if find . \( -path './.git' -o -path './node_modules' \) -prune -o \
  \( -name '*.iso' -o -name '*.img' -o -name '*.qcow2' -o -name '*.raw' -o -name '*.deb' -o -name '*.ko' \) -print | grep -q .; then
  echo "[FAIL] generated binary/package artifacts present" >&2
  find . \( -path './.git' -o -path './node_modules' \) -prune -o \
    \( -name '*.iso' -o -name '*.img' -o -name '*.qcow2' -o -name '*.raw' -o -name '*.deb' -o -name '*.ko' \) -print >&2
  fail=1
else
  echo "[OK] generated binary/package artifacts absent"
fi

if [ "$fail" -ne 0 ]; then
  echo "release audit failed" >&2
  exit 1
fi

echo "release audit passed"
