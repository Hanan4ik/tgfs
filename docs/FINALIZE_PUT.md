# Завершение подтверждённого создания файла

Добавлен явный `tgfs intent --finalize`. Режим поддерживает создание и замену содержимого файла. Для create нужны
подтверждённые FILE_COMMIT и ADD; для replace — только FILE_COMMIT.
Он проверяет ссылки частей, FILE_COMMIT → DIRECTORY и при необходимости
атомарно записывает phase=complete в локальный intent. Не загружает новые
CHUNK/FILE_COMMIT, не публикует ADD и не восстанавливает удалённое имя.
С другими изменяющими флагами intent одновременно не используется.

## Условия и удалённые проверки

Staging и все части должны быть verified, без missing/ambiguous refs. Выбираются
канонические commit/ADD по исходным ID; ADD должен следовать после commit.
Проверяется живая metadata **всех** частей, даже корректных в capture; нужные
backlinks исправляются существующим протоколом ремонта. Смысловые поля,
размеры/имена и содержимое документа должны оставаться прежними.
file_id может изменяться; при этом требуется побайтная сверка downloads.

Затем commit и (для create) DIRECTORY перечитываются через служебные пересылки и сверяются
с подтверждёнными metadata/documents. Проверяется закреп SUPERBLOCK. Если
commit.next_message=null, он устанавливается на канонический DIRECTORY ID.
Если ссылка уже правильная, edit не отправляется. Другой ненулевой указатель
блокирует finalize, а не перезаписывается как произвольная навигация.
Ответ edit проверяется по каналу/ID/document/caption. Журнал содержит
commit_navigation_pending/confirmed. Неизвестный исход или ошибка проверки
не приводят к локальному complete; повтор перечитывает фактическое сообщение.

Это не атомарная распределённая транзакция: у Bot API нет caption CAS.
Применяются прежние ограничения ремонта, служебной очистки и локальной flock.
Перед recovery исходный put должен быть остановлен: обычный writer не берёт
эту recovery-lock и может самостоятельно менять intent.

## Локальная фиксация

После удалённого подтверждения intent.json перечитывается и сравнивается с
первоначально загруженным JSON. Изменившееся намерение блокирует запись.
Если phase уже complete, проверяются сохранённые канонические commit/ADD ID;
файл не переписывается. Иначе:

1. В том же state-dir создаётся intent.finalize.tmp с O_EXCL/0600. Существующий
   файл не удаляется и не перезаписывается: нужна диагностика прерванного шага.
2. Сохраняется копия intent с phase=complete, file_commit_id и
   directory_message_id подтверждённых публикаций. Остальные поля сохраняются.
3. Запись завершается fsync временного файла. В recovery log фиксируется
   local_complete_pending до rename.
4. rename атомарно заменяет intent.json; fsync каталога закрепляет изменение.
   В журнал добавляется local_complete_confirmed с fsync.

data.bin, UUID и документы намерения не удаляются. Ошибка после rename может
означать, что локальная замена уже состоялась; повторный запуск перечитывает
intent и проверяет его. Временный файл при ошибке сохраняется для диагностики,
это не разрешение вручную удалять его без проверки. Полное автоматическое
восстановление всех локальных файловых ошибок пока не реализовано.

## Отчёт

- finalized=true — все проверки/фиксация завершились успешно;
- intent_updated — был ли intent.json заменён этим запуском;
- commit_navigation_repaired — был ли подтверждён edit FILE_COMMIT.next_message;
- local_phase=complete, next_step=intent_complete;
- scope=capture_plus_live_verification.

Поля repaired_chunk_ids/repair_skipped_ids относятся к живой проверке частей.
backlink_repairs остаётся диагностическим планом исходного capture.
resumed/can_resume не превращаются в общий флаг full resume. Завершение
исторического создания не доказывает актуальность пути: имя могли позже
удалить. ADD заново не публикуется. Для отражения реального ремонта captions
в journal нужен новый scan; для полностью корректного no-op — нет.

## Живой тест без повреждения данных

Пользователь подтвердил intent создания: commit37, CHUNK36, DIRECTORY38,
already_observed на capture-010. Проверка уже завершённого намерения:

```bash
git pull --ff-only
make
./build/tgfs intent "${tgfs_net[@]}" \
  --state-dir "$HOME/.local/state/tgfs/put-documents-hello-001" \
  --capture ./capture-010 \
  --finalize
```

Ожидаются finalized=true, intent_updated=false,
commit_navigation_repaired=false и local_phase=complete. Служебные сообщения
могут кратковременно появиться и будут очищены. Исторический файл останется
удалённым; новый scan для этого no-op не требуется.

118 тестовых методов; новые сценарии: finalize восстановленного create,
пустой файл, lost commit edit response и повтор без нового edit, конфликт
навигации, изменённая живая CHUNK metadata при правильном capture, сохранение
существующего temp-файла, атомарный phase=complete и повтор после REMOVE без
новых публикаций. Локально 114 passed/4 kernel skips; ASan/UBSan с detect_leaks=0
из-за ptrace. Реальное непустое finalize требует ещё живой проверки на
незавершённом intent. mkdir/unlink completion, недостающие загрузки,
полный recovery/GC и автоматическая очистка service log остаются открытыми.

## Исправление проверки file_id (2026-11-16)

Первый живой тест завершился общим fresh message/document mismatch.
Вероятная причина — слишком строгое сравнение file_id в свежей пересылке.
[Bot API](https://core.telegram.org/bots/api#sending-files) допускает несколько
валидных file_id одного файла даже у одного бота. Проверка источника/caption
сохранена: сравниваются имя и размер, file_unique_id при наличии у обеих сторон,
а при разных file_id скачиваются обе версии и сравниваются байты. Legacy
capture без file_unique_id поддержан через ту же content-проверку.
Несовпадение конкретного поля теперь указывается в ошибке, без секретов
или полных file_id. Фактическая причина пользовательского расхождения ещё
требует повторной живой проверки; старый общий текст её не доказывает.

## Завершение замены (2026-11-16)

--finalize теперь поддерживает put --replace. Публикация новой версии уже
подтверждается FILE_COMMIT; нового ADD нет и финализатор его не создаёт.
Проверяются staging, refs и свежие metadata всех частей/commit; необходимые
backlinks исправляются прежним протоколом. Для replace next_message commit
должен быть null. Другой указатель блокирует завершение, а не стирается.

Локальная фиксация использует тот же atomic temp/fsync/rename/fsync и flock.
Проверяется формат base refs; это не CAS и не запрет устаревшей базы.
Смешение replacement intent с ADD/DIRECTORY fields отвергается.
Сохраняются исходные object_id/version_id/operation_id, base refs и остальные
поля. Записывается canonical file_commit_id и phase=complete;
directory_message_id не создаётся. Для уже complete проверяются сохранённые
refs, файл не переписывается. Ошибка проверки/неизвестный исход backlinks
не приводит к local complete. Новых storage publications нет.

В отчёт добавлен finalize_kind=create/replace. Для replace directory_ids=[],
commit_navigation_repaired=false. Остальные finalized/intent_updated/phase
и scope сохраняют прежний смысл. Историческую замену можно завершить даже
после удаления имени: это не восстановление пути и не выбор текущей версии.

Пользователь подтвердил create finalize с capture-011: commit37/CHUNK36/ADD38,
local complete, intent_updated=false. Успех нового capture не доказывает
повреждение capture-010; сравнение этих журналов не выполнялось.
Новый безопасный тест завершённого replace intent, с прежним tgfs_net:

```bash
git pull --ff-only
make
./build/tgfs intent "${tgfs_net[@]}" \
  --state-dir "$HOME/.local/state/tgfs/replace-documents-hello-001" \
  --capture ./capture-011 \
  --finalize
```

Ожидаются finalized=true, finalize_kind=replace, intent_updated=false,
commit_navigation_repaired=false, directory_ids=[], local_phase=complete.
Имя /documents/hello.txt останется удалённым. Новый scan не нужен; следующий
свободный пользовательский журнал — capture-012.

119 тестовых методов: добавлен сценарий replace completion с подслучаями
обычной/пустой/multipart версии, lost commit response, неудачных backlinks,
недостающего commit/части, конфликта navigation и повторного no-op без edit.
Проверяются отсутствие ADD/новых data/commit, canonical ID в локальном intent
и отсутствие directory_message_id. Локально 115 passed/4 kernel skips;
ASan/UBSan с detect_leaks=0 из-за ptrace. Живое завершение replace ещё ожидает
подтверждения. Загрузка недостающих частей и completion mkdir/unlink открыты.
