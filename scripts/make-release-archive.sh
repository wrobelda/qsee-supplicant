#!/bin/sh
set -eu

usage() {
	echo "usage: $0 source OUTPUT | binary LIBC OUTPUT" >&2
	exit 2
}

[ "$#" -ge 2 ] || usage

mode=$1
version=$(cat VERSION)
epoch=${SOURCE_DATE_EPOCH:-$(git log -1 --format=%ct)}

case $mode in
source)
	[ "$#" -eq 2 ] || usage
	output=$2
	git archive --format=tar --prefix="qsee-supplicant-$version/" HEAD |
		gzip -n >"$output"
	;;
binary)
	[ "$#" -eq 3 ] || usage
	libc=$2
	output=$3
	case $libc in
	glibc|musl) ;;
	*) usage ;;
	esac
	stage=$(mktemp -d)
	trap 'rm -rf "$stage"' EXIT HUP INT TERM
	root="$stage/qsee-supplicant-$version-linux-aarch64-$libc"
	make DESTDIR="$root" install
	install -Dm644 LICENSE "$root/usr/share/licenses/qsee-supplicant/LICENSE"
	install -Dm644 LICENSES/BSD-3-Clause-Clear.txt \
		"$root/usr/share/licenses/qsee-supplicant/BSD-3-Clause-Clear.txt"
	tar --sort=name --mtime="@$epoch" --owner=0 --group=0 --numeric-owner \
		-C "$stage" -cf - "$(basename "$root")" | gzip -n >"$output"
	;;
*)
	usage
	;;
esac
