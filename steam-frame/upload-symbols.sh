#!/bin/bash
# Crash reports: uploads the unstripped program's symbols to Sentry, so the stacks of sent crash reports show
# function names. steam-frame/build.sh runs it when SENTRY_AUTH_TOKEN is set; it can be run by hand too:
#
#   SENTRY_AUTH_TOKEN=... SENTRY_ORG=<org> SENTRY_PROJECT=<project> steam-frame/upload-symbols.sh <unstripped lasrecomp>
#
# The token, the organisation and the project come from the environment (or the CLI's own login and defaults); none
# of them is ever written into the repository. The CLI is sentry-cli, or Sentry's newer `sentry` CLI (SENTRY_CLI=path
# picks one). It uploads the ELF file as it is (its symbol table and the debug info of the -g build); the release's
# stripped copy has the same GNU build ID, so Sentry matches them. The game code (lasgame.so) is built on each
# player's Frame from their own files: it has no symbols to upload, and its frames are named f_<guest address> in the
# report itself.
set -e
FILE=${1:?usage: upload-symbols.sh UNSTRIPPED_LASRECOMP}
[ -f "$FILE" ] || { echo "upload-symbols: no file $FILE" >&2; exit 1; }
CLI=${SENTRY_CLI:-}
[ -n "$CLI" ] || CLI=$(command -v sentry-cli || command -v sentry || true)
[ -n "$CLI" ] || { echo "upload-symbols: neither sentry-cli nor sentry is installed (SENTRY_CLI=path)" >&2; exit 1; }
BUILD_ID=$(readelf -n "$FILE" 2> /dev/null | awk '/Build ID/ {print $3; exit}')
[ -n "$BUILD_ID" ] || echo "upload-symbols: warning: $(basename "$FILE") has no GNU build ID: Sentry can't match it" >&2
echo "upload-symbols: $(basename "$FILE"), build id ${BUILD_ID:-none}, with $(basename "$CLI")"
"$CLI" debug-files upload --type elf --wait "$FILE"
