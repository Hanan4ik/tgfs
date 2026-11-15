# Проверка намерения перед восстановлением

Реализована команда `tgfs intent` для put (создание/replace), mkdir и unlink.
Она только читает intent/capture и скачивает metadata для сверки. Не отправляет,
не редактирует и не удаляет сообщения; не меняет intent, staging или cache.
Автоматическое resume и исправление backlinks пока не реализованы.

## Запуск

Пользователь подтвердил живой unlink и scan capture-010. Нового scan для
проверки завершённого unlink не требуется. Из корня проекта, с tgfs_net:

```bash
git pull --ff-only
make
./build/tgfs intent "${tgfs_net[@]}" \
  --state-dir "$HOME/.local/state/tgfs/unlink-documents-hello-001" \
  --capture ./capture-010
```

JSON-отчёт содержит kind, local_phase, publication, commit_ids, child_ids,
directory_ids, scope=provided_capture_only и resumed=false. Ожидаются
kind=unlink, local_phase=complete, publication=confirmed и исходный ID
DIRECTORY REMOVE в directory_ids. Числовые ID возвращаются строками.

Аналогично можно проверить put/mkdir по их собственному state-dir и более
новому capture. token/proxy/timeout работают как у read. --help не делает
сетевых запросов. init intents пока не поддержаны. State-dir должен быть
приватным (0700), intent.json — owner-only обычным файлом (0600), без symlink
конечного компонента. Команда не исправляет права автоматически.

## Что именно проверяется

- Capture должен быть завершённым, того же storage channel, с boundary строго
  больше source_boundary_id intent. Старый capture отвергается. Используется
  существующая проверка журнала, канала, metadata и слияния DIRECTORY.
- Для FILE_COMMIT ищется operation_id нужного object_id и сравниваются caption
  (без навигационного next_message) и весь сохранённый commit_document.
- Для ADD/REMOVE скачиваются DIRECTORY documents родителя, ищется UUID операции
  и сверяется весь сохранённый документ и caption. Повторные публикации
  возвращаются списком исходных ID. Тело с тем же UUID, но иным содержимым
  приводит к ошибке, а не подтверждению.
- Для mkdir дополнительно сверяется пустой дочерний DIRECTORY. Позднейшие
  операции этого каталога не считаются повтором пустого документа.
- Создание файла подтверждается парой FILE_COMMIT + ADD, replace — FILE_COMMIT,
  mkdir — дочерним DIRECTORY + ADD, unlink — REMOVE.
- Если документ будущего commit ещё не был сохранён в intent, этот этап не
  подтверждается. Старые локальные intentions без нужных полей не дополняются.

publication=confirmed означает лишь, что соответствующие публикации найдены
и совпали **в данном capture**. Это не подтверждение актуального имени/версии:
позже файл могли заменить или удалить. Само поле publication не является проверкой целостности bytes.
Обновление ниже добавляет отдельную проверку staging/CHUNK и предложений
backlinks для put; восстановительные действия пока не выполняются.

publication=not_observed означает, что требуемое сочетание публикаций не
подтверждено в capture. Capture ограничен маркером и может содержать пропуски;
отсутствие записи не доказывает, что запрос не выполнился. Даже такой отчёт
не разрешает слепо повторять sendDocument. Частично найденные этапы остаются
видны в массивах ID. Успешный отчёт обоих типов имеет код 0; ошибка проверки,
сети/формата — код 1, неверные аргументы — 2.

## Границы первого этапа recovery

Команда не перечисляет автоматически все state-dir и не называет intent
безопасным для повторной отправки. Не исправляет local_phase=complete,
не обновляет refs и не удаляет intent после подтверждения. Секреты в отчёт
не включаются. Persistent cache не используется; metadata могут скачиваться повторно. В обновлении ниже CHUNK для put
также скачиваются для проверки; для mkdir/unlink — нет.

Следующий этап: сверить staging/CHUNK и сохранённые refs, сформировать точный
план недостающих действий, затем реализовать ограниченное идемпотентное
продолжение с теми же UUID. Классификация ошибок и правила неизвестного исхода
остаются частью #10/#11; эта команда их целиком не закрывает.

Проверки: завершённые put/mkdir/unlink, потерянные ответы commit/REMOVE,
неопубликованный CHUNK и 429/pin reject, повреждённый intent document,
устаревший capture и права. Проверяется неизменность intent и отсутствие
изменяющих API методов. Всего 113 тестов; локально 109 passed и 4 kernel skips
(нет /dev/fuse), ASan/UBSan с detect_leaks=0 из-за ptrace.

## Проверка staging/CHUNK и диагностический план (2026-11-15)

Пользователь подтвердил первый intent report: unlink complete, REMOVE source ID43,
publication=confirmed. Теперь для put команда дополнительно проверяет data.bin:
owner-only обычный файл, размер и SHA-256 относительно intent. Commit caption,
число частей и локальные refs должны согласовываться с этим staging.

В capture ищутся все CHUNK нужного object_id/version_id. Для каждого проверяются
parent_id, номер/имя, offset/размер/число частей, SHA части и файла. Документ
скачивается и сравнивается побайтно с соответствующим диапазоном staging.
Повреждение staging, bytes, metadata или конфликтующий ненулевой backlink дают
ошибку (код1), а не разрешение продолжить. Кэш не используется.

Дополнительные поля JSON:

| Поле | Значение |
| --- | --- |
| staging | verified для put; not_applicable для mkdir/unlink |
| verified_chunk_ids | ID частей, проверенных по bytes и staging |
| unobserved_part_indexes | Номера частей, не найденных в capture |
| ambiguous_part_indexes | Номера, для которых найдено несколько сообщений |
| unobserved_known_ids | Сохранённые refs/refs подтверждённого commit, не найденные среди проверенных частей |
| backlink_repairs | Предлагаемые message_id/file_commit_id/next_message для частей подтверждённого канонического commit |
| can_resume | Всегда false: автоматическое продолжение не реализовано |
| next_step | Диагностическая категория, не команда записи |

next_step принимает resolve_duplicate_parts, capture_or_upload_outcome_review,
commit_publication_review, backlink_repair_review, directory_publication_review
или verified_in_capture. Для mkdir/unlink — metadata_review_only: этот этап
не проверяет их навигацию. Массивы пусты, если соответствующих наблюдений нет.

Ненайденная часть не объявляется неотправленной: запрос мог выполниться после
маркера либо сообщение могло оказаться в пропуске. Наличие всех частей также
не разрешает повторить FILE_COMMIT без сверки исхода его отправки.
backlink_repairs — только evidence для будущего ограниченного ремонта. Ни один
editMessageCaption сейчас не выполняется. Полная навигация FILE_COMMIT/DIRECTORY,
проверка всех crash-phases и безопасный resume остаются следующими задачами.

Для живой проверки, с прежним tgfs_net:

```bash
./build/tgfs intent "${tgfs_net[@]}" \
  --state-dir "$HOME/.local/state/tgfs/replace-documents-hello-001" \
  --capture ./capture-010
```

Ожидаются staging=verified, проверенные ID CHUNK, пустые массивы проблем и
next_step=verified_in_capture. publication=confirmed относится к прежней
публикации замены: имя уже удалено, но её сообщения всё ещё существуют.
Для mkdir/unlink не требуется data.bin. Ничего заново отправлять не нужно.

Тесты дополнены проверкой повреждённого staging и скачанных bytes, частично
отправленной версии, потерянного commit, незаполненных backlinks и двух
сообщений одной части. Количество тестовых методов прежнее — 113; расширены
сквозные сценарии. Это не проверка реального Telegram: живой результат ещё
ожидается от пользователя.

## Явный режим ремонта

Добавлен [--repair-backlinks](BACKLINK_REPAIR.md). Без этого флага intent
остаётся read-only. С флагом перечитываются сообщения через служебный канал,
берётся локальная блокировка, дописывается лог и редактируются только
проверенные ссылки существующих частей. Полное resume ещё не реализовано.
