#!/bin/sh
# 版本号优先级：git tag / commit > 源码树里的 VERSION 文件 > unknown
# 之所以加 VERSION 文件：工程解压或不在 git 仓库里时也能编出带版本号的固件。
here=$(cd "$(dirname "$0")" && pwd)

VERSION="unknown"
if [ -f "$here/VERSION" ]; then
    VERSION=$(tr -d ' \t\r\n' < "$here/VERSION")
fi

if command -v git >/dev/null 2>&1 && git rev-parse --git-dir >/dev/null 2>&1; then
    V=$(git describe --exact-match --tags 2>/dev/null || git rev-parse --short HEAD 2>/dev/null)
    if [ -n "$V" ]; then
        VERSION="$V"
    fi
fi

if [ ! -e generated/probe ]; then
    mkdir -p generated/probe
fi

cat > generated/probe/version.h << EOF2
#ifndef _PROBE_VERSION_H
#define _PROBE_VERSION_H

#define PROBE_VERSION "${VERSION}"

#endif
EOF2
