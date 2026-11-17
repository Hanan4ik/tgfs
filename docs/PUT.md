# Публикация нового файла в TGFS

Реализовано 2026-11-15: `tgfs put`. Команда создаёт новый объект файла,
записывает CHUNK/FILE_COMMIT и публикует отдельный DIRECTORY ADD в выбранном
каталоге (--parent, по умолчанию /). Создание каталогов и вложенные пути:
[DIRECTORY_WRITE.md](DIRECTORY_WRITE.md). Это CLI writer; запись через FUSE,
rename/unlink и автоматическое recovery ещё не реализованы. Замена
содержимого существующего файла доступна с явным [--replace](FILE_REPLACE.md).

## Первый живой опыт (история, подтверждён пользователем)

Ниже сохранён первоначальный запуск с capture-004. Для новой проверки
используйте актуальные свободные номера и state-dir из DIRECTORY_WRITE.md.

Обновить main, собрать и подготовить стабильный исходный файл:

```bash
git switch main
git pull --ff-only
make
install -d -m 700 "$HOME/.local/state/tgfs"
printf 'hello tgfs\n' > "$HOME/.local/state/tgfs/hello-source.txt"

./build/tgfs put \
  --capture ./capture-004 \
  --file "$HOME/.local/state/tgfs/hello-source.txt" \
  --name hello.txt \
  --state-dir "$HOME/.local/state/tgfs/put-hello-001" \
  --token-file "$HOME/.config/tgfs/secrets/test.token" \
  --proxy socks5h://127.0.0.1:10808
```

Используется проверенный пользователем capture-004 с пустым деревом.
state-dir должен отсутствовать, его родитель — существовать. Имя hello.txt
должно быть новым. Справка: `tgfs put --help`. Успех — OK put с UUID и
исходными ID commit/directory. Код 0 — успех, 1 — операция не завершилась,
2 — неверные аргументы.

При успехе собрать новый журнал (следующий свободный каталог пользователя —
capture-005), затем прочитать файл:

```bash
./build/tgfs scan \
  --token-file "$HOME/.config/tgfs/secrets/test.token" \
  --storage-chat-id -1003916921678 \
  --service-chat-id -1004400939172 \
  --proxy socks5h://127.0.0.1:10808 \
  --output ./capture-005

./build/tgfs read \
  --capture ./capture-005 \
  --token-file "$HOME/.config/tgfs/secrets/test.token" \
  --proxy socks5h://127.0.0.1:10808 \
  ls /

./build/tgfs read \
  --capture ./capture-005 \
  --token-file "$HOME/.config/tgfs/secrets/test.token" \
  --proxy socks5h://127.0.0.1:10808 \
  cat /hello.txt
```

Ожидаются hello.txt в ls и `hello tgfs` с переводом строки в cat.
Для live multipart можно отдельно использовать новый файл/имя/state-dir и
`--chunk-bytes N`. Для 10-байтного hello tgfs достаточно N=4; по умолчанию
используется часть до 20971520 байт. Не запускать повтор с новым state-dir
после неясного исхода предыдущей операции.

## Последовательность

1. Проверить capture и восстановить корень через metadata reader. Если имя
   уже есть в снимке, остановиться до записи. getChat/pinned SUPERBLOCK
   должен совпадать с SUPERBLOCK из capture; next_message не участвует в
   сравнении смысловых полей. Без закрепа writer останавливается.
2. Прочитать обычный входной файл, сформировать независимые UUIDv4 object_id,
   version_id, operation_id FILE_COMMIT и operation_id ADD. Создать новый
   приватный state-dir (0700), сохранить data.bin и intent.json (0600).
   В intent сохраняются размеры, хеш, граница, UUID и документы будущих
   commit/ADD. fsync/rename завершаются до следующего сетевого изменения.
3. Отправить части как documents NAME.0, NAME.1 и т.д. Каждый CHUNK содержит
   UUID файла/версии/родителя, размеры, offset, SHA-256 части и целого файла,
   file_commit_id=null и next_message=null. Сохранить исходные ID частей.
4. Отправить FILE_COMMIT-document с точными refs parts. Пустой файл имеет
   parts=[] и не имеет CHUNK или байтов-заглушек.
5. Изменить captions собственных CHUNK: поставить file_commit_id и
   next_message на следующую свою часть/FILE_COMMIT. Смысловые поля и bytes
   не меняются. Каждое изменение проверяется по возвращённому Message.
6. Ещё раз проверить закреп. Опубликовать DIRECTORY-document целевого каталога
   (parent_id=null только у корня), содержащий одну ADD имени на новый object_id.
7. Поставить next_message собственного FILE_COMMIT на новую DIRECTORY.
   Сохранить phase=complete и вывести результат.

Файл получает имя после DIRECTORY ADD, а не после первой части. FILE_COMMIT
публикует содержимое версии; до ADD новый объект не виден по пути. Это
упорядоченные публикации, не атомарная транзакция нескольких сообщений:
после отправки ADD файл уже может читаться, даже если финальный ремонт
next_message или сохранение complete завершились ошибкой.

## Состояние и неизвестные ответы

| Файл/поле | Назначение |
| --- | --- |
| data.bin | Локальная копия байтов, из которых считались хеши и отправлялись части |
| intent.json | UUID, имя, parent_path/parent_id/storage, размеры/хеш, source_boundary_id, parts/refs, документы commit/ADD |
| phase | prepared, chunk_send_pending/chunk_sent, commit_send_pending, backlinks_pending, directory_send_pending, published_navigation_pending, complete |
| *_response_uncertain | Ответ загрузки потерян или не прошёл проверку; операция могла выполниться |
| file_commit_id / directory_message_id | Известные исходные ID ответов |
| intent.tmp | Незавершённая локальная замена; не удалять при диагностике |

Состояние сохраняется до изменяющего запроса и после подтверждённого ответа.
Токен/пароли в intent не записываются. При ошибке известные последние ID и
phase выводятся в stderr. Существующий state-dir блокирует повторную отправку.
Не удаляйте и не заменяйте его после ошибки: автоматического resume пока нет.
Исходный файл должен быть стабильным во время чтения. data.bin после
публикации остаётся для диагностики, это не одноразовый download-cache.

При неизвестном исходе ADD смотрите новый scan и ищите сохранённый
add_operation_id: файл мог уже появиться. При неизвестном commit/CHUNK
необходима диагностика по сохранённым UUID и refs; слепого retry нет.
Старые и частично отправленные сообщения не удаляются writer автоматически.

## Ограничения прототипа

- Создание нового файла либо явный --replace в существующем каталоге (--parent, по умолчанию /).
  Проверка имени относится к выбранному capture. При устаревшем снимке
  конкурентные создания одинакового имени могут конфликтовать; ядро
  сохраняет обе операции и сообщает об ошибке, а не выбирает победителя.
  Независимые добавления разных имён из одного старого capture объединяются.
- Вход — обычный файл без symlink конечного компонента, до 64 MiB, читается
  в память целиком. Имя нового файла — UTF-8, до 245 байт, без /, CR/LF.
  Максимум 4096 частей; --chunk-bytes принимает 1..20971520. Это локальные
  ограничения writer, не физическая ёмкость канала.
- Интервал между sendDocument/editMessageCaption по умолчанию 1100 ms,
  --send-interval-ms принимает 0..10000. Он снижает частоту запросов, но не
  гарантирует отсутствие 429 при других writer/ботах. 429 и retry_after
  возвращаются пользователю, без автоматической повторной публикации.
- Обновляются только ссылки собственных записей этой публикации. Ближайшие
  предшественники из старой истории не редактируются; при параллельной
  записи ссылки могут перепрыгивать чужие сообщения. Корректность текущего
  полного scan не зависит от next_message. Полный ремонт цепи/checkpoint
  и ускорение инкрементального обхода ещё открыты.
- Применяются методы/настройки существующего Bot API транспорта. Проверяются
  канал, исходные ID (по возрастанию), filename/size/caption ответа.

## Проверки

4 новых сквозных теста с подслучаями используют реальный multipart/curl и
HTTP double: put → scan → read, одна часть/multipart/пустой файл, слияние
независимых ADD из старого capture, отказ от существующего имени, проверка
backlinks/next_message, смена pin, потеря ответа, неверные metadata и 429
при edit без публикации ADD. Проверяется intent на диске до сетевых изменений
и блокирование повторной отправки с тем же state-dir.
Всего 72 теста; ASan/UBSan локально с detect_leaks=0 из-за ptrace.
Живой put/read корневых hello.txt/multipart.txt/empty.txt подтверждён пользователем;
токен остаётся у него. Число 72 относится к первоначальному этапу.

Ссылки: [формат операций](DIRECTORY_OPERATIONS.md), [чтение](REMOTE_READ.md),
[рекомендации Telegram о частоте](https://core.telegram.org/bots/faq#my-bot-is-hitting-limits-how-do-i-avoid-this).

## Каталоги (2026-11-15)

Теперь поддержаны --parent /absolute/path и создание родителя через mkdir.
Родитель должен быть виден в capture; после mkdir нужен scan. Подробный порядок,
проверки и команды: [DIRECTORY_WRITE.md](DIRECTORY_WRITE.md). Живой put/read
в корне и online/offline FUSE подтверждены пользователем; вложенный writer
пока проверен автоматически. Ограничения размера, intent и retry сохраняются.

## Замена содержимого

Добавлен put --replace: object_id сохраняется, версия/operation_id новые,
DIRECTORY ADD не нужен. Без флага существующее имя всё ещё отвергается.
[Публикация, ошибки после commit и живой тест](FILE_REPLACE.md).

Раннее продолжение прерванной загрузки: [--resume-upload](RESUME_UPLOAD.md).
Новые intents сохраняют last_attempted_part перед каждой попыткой CHUNK;
неизвестный ответ не разрешает автоматический повтор.
