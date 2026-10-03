#!/usr/bin/env bash
# Аудит проиндексированных (staged) файлов перед коммитом: личные пути, бинарники ELF, крупные файлы. Выход != 0 при находках.
# Использование:  git add -A && tools/audit_staged.sh && git commit -m "…"
cd "$(dirname "$0")/.." || exit 2
bad=0
if git grep --cached -n -I -e "young-developer" -e "/tmp/claude-1000" -- . ':!docs/blender/accuracy.md' ':!docs/blender/status.md' ':!tools/audit_staged.sh' | head -5 | grep .; then
  echo "АУДИТ: личные пути в файлах (см. выше)"; bad=1
fi
while IFS= read -r f; do
  [ -f "$f" ] || continue
  if head -c4 "$f" 2>/dev/null | grep -q $'^\x7fELF'; then echo "АУДИТ: бинарник ELF: $f"; bad=1; fi
  sz=$(stat -c %s "$f" 2>/dev/null || echo 0)
  if [ "$sz" -gt 6000000 ]; then echo "АУДИТ: файл > 6 МБ: $f ($sz)"; bad=1; fi
done < <(git diff --cached --name-only --diff-filter=AM)
[ $bad -eq 0 ] && echo "АУДИТ: чисто"
exit $bad
