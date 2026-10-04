#!/usr/bin/env bash
# Сборка отладочного агента: dbg.jar (Agent) + dbgboot.jar (Dbg, в пути загрузчика начальной загрузки). Нужен JDK 24+ (java.lang.classfile).
set -e
cd "$(dirname "$0")"
rm -rf out && mkdir out
javac -d out Agent.java Dbg.java
printf 'Premain-Class: Agent\nCan-Retransform-Classes: false\n' > out/MANIFEST.MF
(cd out && jar cfm ../dbg.jar MANIFEST.MF Agent*.class)
jar cf dbgboot.jar -C out Dbg.class
ls out | tr '\n' ' '; echo; echo built $(pwd)/dbg.jar + dbgboot.jar
