# Завершение mkdir и unlink

`tgfs intent --finalize` теперь поддерживает mkdir и unlink. Put/create/replace
сохраняют прежний контракт. Режим не публикует новые объекты, ADD или REMOVE.
Он подтверждает сохранённую операцию и завершает локальный intent.

## Проверки

Для mkdir нужны оба документа: исходный пустой дочерний DIRECTORY и ADD
родителя с сохранённым UUID операции. Для unlink — подтверждённый REMOVE.
Сохранённые caption/body сверяются с полями intent и форматом операций.
Отсутствующая публикация не трактуется как разрешение отправить её заново.

Документы перечитываются через служебный канал с проверкой forward_origin,
chat/message identity, caption и document identity/bytes. Alternate file_id
разрешается только прежней content-проверкой; конфликт блокирует завершение.
Перед локальной фиксацией/редактированием проверяется SUPERBLOCK pin.

У mkdir child.next_message должен указывать на canonical ADD: null исправляется,
правильная ссылка пропускается, другая ненулевая блокирует finalize. Ответ edit
проверяется, child_navigation_pending/confirmed записываются в recovery log.
После потерянного ответа локальный intent не получает complete; повтор читает
текущее сообщение и не повторяет уже выполненный edit.

Операционный DIRECTORY родителя (ADD/REMOVE) должен сохранять next_message=null
по контракту этих writer; чужая навигация не перезаписывается. У unlink ничего
в канале не редактируется, кроме создания/очистки служебной копии для проверки.
CHUNK/data.bin не скачиваются, staging=not_applicable.

## Локальное завершение

Применяется общий путь flock → сверка неизменившегося intent → приватный
intent.finalize.tmp с O_EXCL → fsync → rename → fsync каталога. Для mkdir
сохраняются child_message_id и directory_message_id; для unlink только
подтверждённый directory_message_id. file_commit_id не добавляется.
UUID и остальные поля не меняются. Уже complete с верными refs не переписывается.
Временный файл после сбоя не удаляется автоматически, исходный writer должен
быть остановлен. Ограничения CAS/cleanup/локальной flock прежние.

Отчёт: finalize_kind=mkdir/unlink, finalized=true, intent_updated=true/false,
child_navigation_repaired=true/false, local_phase=complete,
next_step=intent_complete и scope=capture_plus_live_verification.
resumed/can_resume остаются false: неизвестные публикации не повторяются.
Завершение исторического mkdir не восстанавливает удалённые имена; завершение
REMOVE не удаляет дополнительные/ненаблюдённые ADD. Полный recovery ранних фаз
и автоматический cleanup всё ещё отдельные задачи.

## Живые проверки

Пользователь подтвердил replace finalize на capture-011: commit41/CHUNK40,
intent_updated=false. Следующий свободный scan — capture-012, но для этих
no-op проверок новый scan не требуется. Из корня проекта с прежним tgfs_net:

```bash
git pull --ff-only
make

./build/tgfs intent "${tgfs_net[@]}" \
  --state-dir "$HOME/.local/state/tgfs/mkdir-documents-001" \
  --capture ./capture-011 --finalize

./build/tgfs intent "${tgfs_net[@]}" \
  --state-dir "$HOME/.local/state/tgfs/unlink-documents-hello-001" \
  --capture ./capture-011 --finalize
```

Ожидаются finalize_kind=mkdir/unlink соответственно, finalized=true,
intent_updated=false и child_navigation_repaired=false. Служебные копии могут
кратковременно появляться, затем очищаются. Существующий /documents остаётся,
удалённый /documents/hello.txt не восстанавливается.

121 тестовый метод: новые mkdir/unlink completion scenarios проверяют complete,
потерянный child/parent/REMOVE response, незавершённую навигацию, lost child edit,
конфликт next_message, повтор без edit/перезаписи и отсутствие публикаций.
Неподтверждённые child/ADD/REMOVE и API/pin failures не дают complete.
Общий atomic local путь также проверен put-регрессиями. Локально 117 passed и
4 kernel skips (нет /dev/fuse); ASan/UBSan с detect_leaks=0 из-за ptrace.
Живое непустое восстановление по-прежнему ожидает реального незавершённого intent.
