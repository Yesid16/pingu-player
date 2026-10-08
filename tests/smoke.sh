#!/usr/bin/env bash
# Prueba de extremo a extremo: arranca el reproductor con salida nula (sin
# tarjeta de sonido) y le manda comandos mientras "suena" la musica.
set -u
BUILD="${BUILD:-build}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

for n in 1 2 3; do
    "$BUILD/gentone" "$TMP/p$n.wav" "$((200 + n * 80))" 2.0 || exit 1
done

fail() { echo "SMOKE FALLO: $1"; echo "---- salida ----"; cat "$TMP/out.txt"; exit 1; }

(
    echo "add $TMP/p1.wav $TMP/p2.wav $TMP/p3.wav"
    echo "ls"
    echo "play"
    sleep 0.7
    echo "pause"
    sleep 0.4
    echo "status"
    echo "resume"
    echo "next"
    sleep 0.3
    echo "shuffle"
    echo "shuffle on"
    echo "shuffle off"
    echo "mv 3 1"
    echo "rm 2"
    echo "ls"
    echo "prev"
    echo "repeat all"
    sleep 0.3
    echo "penguin"
    echo "stop"
    echo "play 2"
    sleep 0.3
    echo "clear"
    echo "ls"
    echo "repeat off"
    echo "add $TMP/p1.wav"
    echo "play"
    sleep 2.6
    echo "status"
    echo "quit"
) | timeout 40 "$BUILD/pingu" --backend null > "$TMP/out.txt" 2>&1
rc=$?

[ $rc -eq 0 ] || fail "codigo de salida $rc (timeout/crash)"
grep -q "Reproduciendo: p1.wav" "$TMP/out.txt"        || fail "no reprodujo p1"
grep -q "en pausa" "$TMP/out.txt"                     || fail "no entro en pausa"
grep -q "lista mezclada" "$TMP/out.txt"               || fail "shuffle no respondio"
grep -q "la lista esta vacia" "$TMP/out.txt"          || fail "clear no vacio la lista"
grep -q "Fin de la lista" "$TMP/out.txt"              || fail "no detecto el fin de la lista"
grep -q "Hasta luego" "$TMP/out.txt"                  || fail "no salio limpiamente"
grep -qiE "segmentation|sanitizer|runtime error" "$TMP/out.txt" && fail "error de ejecucion"

echo "smoke OK"
