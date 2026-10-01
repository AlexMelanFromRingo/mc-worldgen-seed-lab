#!/usr/bin/env bash
# run_dump.sh <version 26.1|26.2|26.3> <dim overworld|large_biomes|amplified|nether> <seeds-file> <points-per-seed> <out>
set -e
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SP="${SP:-${TMPDIR:-/tmp}/cb_scratch}"; mkdir -p "$SP"
V=$1; DIM=$2; SEEDS=$3; N=$4; OUT=$5
CP=$("$ROOT/tools/classpath.sh" $V)
java -Xss8m --sun-misc-unsafe-memory-access=allow -Dlog4j2.configurationFile=$ROOT/oracle/log4j2.xml \
  -cp "$SP/jbuild/$V:$CP" cbx.ClimateDump "$OUT" "$DIM" "@$SEEDS" "$N"
