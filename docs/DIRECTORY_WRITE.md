# Создание каталогов и публикация файлов по пути

Реализовано 2026-11-15: `tgfs mkdir --path /documents` создаёт один каталог,
`tgfs put --parent /documents --name hello.txt` публикует новый файл в нём.
Без --parent put по-прежнему пишет в корень. Новые wire-типы не вводятся:
используются DIRECTORY operations ADD/REMOVE и прежние CHUNK/FILE_COMMIT.

## Что отправляется в канал

mkdir сначала разрешает родительский путь в **выбранном capture**, проверяет,
что это каталог, имени ещё нет и закреп соответствует SUPERBLOCK. UUIDv4
нового каталога и операции ADD сохраняются в приватном intent до sendDocument.

1. Отправляется directory.json с `{"operations":[]}`. Caption содержит
   type=DIRECTORY, object_id нового каталога, parent_id родителя,
   next_message=null. Пустой каталог имеет настоящий JSON document.
2. После ответа и повторной проверки закрепа отправляется новый DIRECTORY
   **родителя**: его object_id не меняется, parent_id указывает на его родителя
   (null только у корня). Document содержит одну ADD: operation_id, name,
   object_id нового каталога. Предыдущие документы родителя не заменяются.
3. next_message собственного первого DIRECTORY редактируется на исходный ID
   второго сообщения. После этого intent получает phase=complete.

Имя становится видимо после ADD. Если отправлен только дочерний DIRECTORY,
объект остаётся без имени в дереве. Ошибка после принятого ADD может означать,
что каталог уже виден: это несколько публикаций, не атомарная транзакция.
FILE_COMMIT для каталога не нужен. В родительской ADD нет Telegram ID,
version_id или file_commit_id; для поиска используется восстановленный индекс.

put с --parent применяет ту же проверку пути. CHUNK/FILE_COMMIT получают UUID
целевого каталога в parent_id. Последний DIRECTORY ADD адресован именно этому
каталогу и сохраняет его родительский UUID. Исходный Telegram ID DIRECTORY
может меняться при каждом добавлении; ссылки object_id остаются устойчивыми.

Независимые ADD разных имён объединяются принятым алгоритмом и во вложенных
каталогах. Одновременное создание одного имени разными object_id остаётся
конфликтом. Центрального координатора/блокировки нет. Проверка уникальности
касается capture, а не невидимых конкурентных публикаций.

## Проверка на пользовательском канале

Пользователь подтвердил живые online/offline FUSE, ls/cat/stat/empty/EROFS для
capture-006. Следующие свободные журналы — capture-007 и capture-008.
Перед обновлением бинарника завершите текущий mount. Команды из корня проекта:

```bash
git pull --ff-only
make

tgfs_net=(
  --token-file "$HOME/.config/tgfs/secrets/test.token"
  --proxy socks5h://127.0.0.1:10808
)

./build/tgfs mkdir "${tgfs_net[@]}" \
  --capture ./capture-006 \
  --path /documents \
  --state-dir "$HOME/.local/state/tgfs/mkdir-documents-001"

./build/tgfs scan "${tgfs_net[@]}" \
  --storage-chat-id -1003916921678 \
  --service-chat-id -1004400939172 \
  --output ./capture-007

./build/tgfs read "${tgfs_net[@]}" --capture ./capture-007 ls /documents
```

Первый ls нового каталога пустой. В корне прежние файлы сохраняются, добавляется
documents. Старый capture-006 ещё не знает UUID новой директории: использовать
его для put --parent /documents нельзя. Нужен завершённый capture-007.

```bash
printf 'hello from documents\n' > "$HOME/.local/state/tgfs/directory-source.txt"

./build/tgfs put "${tgfs_net[@]}" \
  --capture ./capture-007 \
  --file "$HOME/.local/state/tgfs/directory-source.txt" \
  --parent /documents \
  --name hello.txt \
  --state-dir "$HOME/.local/state/tgfs/put-documents-hello-001"

./build/tgfs scan "${tgfs_net[@]}" \
  --storage-chat-id -1003916921678 \
  --service-chat-id -1004400939172 \
  --output ./capture-008

./build/tgfs read "${tgfs_net[@]}" --capture ./capture-008 ls /documents
./build/tgfs read "${tgfs_net[@]}" --capture ./capture-008 cat /documents/hello.txt
```

Ожидаются hello.txt и `hello from documents`. Можно затем смонтировать
capture-008 [прежней командой FUSE](FUSE_READONLY.md). Уже работающий mount
capture-006 автоматически не обновляется. Корневой /hello.txt — другой объект,
он сохраняет прежнее содержимое. Для /documents/sub применяется mkdir --path
/documents/sub с capture, который уже содержит documents; затем снова scan.

## Локальное состояние и сбои

mkdir state-dir содержит intent.json (0600) в новом каталоге 0700: kind=mkdir,
path/parent_id/object_id/add_operation_id, storage/source_boundary_id,
подготовленные captions/documents. data.bin отсутствует: mkdir не загружает
пользовательские байты. Известные child_message_id и directory_message_id
сохраняются после ответов. put сохраняет data.bin как раньше, parent_path и
parent_id, а также caption операции DIRECTORY для диагностики.

| Phase mkdir | Смысл |
| --- | --- |
| child_send_pending | Намерение сохранено до отправки пустого DIRECTORY |
| child_response_uncertain | Ответ потерян или metadata не совпали; дочерний объект мог появиться |
| child_sent | Исходный ID дочернего DIRECTORY сохранён, имя ещё не опубликовано |
| directory_send_pending | Намерение ADD сохранено перед отправкой родителя |
| directory_response_uncertain | ADD мог быть принят; проверить новый scan по add_operation_id |
| published_navigation_pending | ADD подтверждён, финальная навигация/complete ещё не завершены |
| complete | Подтверждён ADD и навигация собственной записи |

Существующий state-dir блокирует повторную публикацию. Resume, автоматическое
удаление orphan и повторная отправка при неизвестном исходе не реализованы.
Не удаляйте intent после ошибки. UUID/известные ID и phase нужны для диагностики.
Токен и proxy в intent не записываются. Старые записи и предшественники журнала
не редактируются; полный ремонт next_message остаётся отдельной задачей.

## Границы и проверки

- Один каталог за команду; mkdir -p, rename/move/delete пока отсутствуют.
- Только абсолютные TGFS-пути до 4096 байт; конечное имя каталога до 255 байт
  UTF-8, без slash, CR/LF, `.` и `..`. Завершающий slash у mkdir допускается.
  Компоненты родителя разрешаются существующим ядром и не допускают dot/dotdot.
- Существующее имя файла или каталога отвергается, файл вместо родителя —
  ошибка. Проверки завершаются до удалённых изменений.
- Пределы put сохраняются: новый файл, до 64 MiB, 4096 частей, имя до 245 байт.
- Mount остаётся read-only; обычные mkdir/cp в нём получают EROFS.
- Добавлены сквозные проверки вложенного дерева, parent_id, разных ADD из
  одного capture, empty/multipart, stale capture, неверных путей/дубликатов,
  потери ответов, смены pin и повторного запуска. Условный kernel test строит
  дерево через настоящие CLI/curl на HTTP double, сканирует, монтирует remote
  capture и проверяет cat/pread/nlink/EROFS/unmount.
- Локально 104 теста: 102 успешны, 2 kernel skip (нет /dev/fuse); ASan/UBSan
  с detect_leaks=0 из-за ptrace. Живые mkdir/put во вложенный Telegram каталог
  ещё должен подтвердить пользователь; HTTP double не заменяет эту проверку.
