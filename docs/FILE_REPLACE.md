# Замена содержимого существующего файла

Реализовано 2026-11-15: `tgfs put --replace`. Без --replace имя по-прежнему
должно быть новым. С --replace имя должно уже обозначать обычный файл в
выбранном capture; каталог и отсутствующее имя отвергаются до отправки данных.
Родитель задаётся --parent (по умолчанию /), как при создании файла.

## Идентичность и момент публикации

| Поле | При замене |
| --- | --- |
| object_id | Сохраняется: это тот же логический файл |
| parent_id / имя | Сохраняются из выбранного каталога |
| version_id | Новый UUIDv4 для содержимого |
| operation_id | Новый UUIDv4 для операции FILE_COMMIT |
| Message.message_id | Новые ID у новых CHUNK и FILE_COMMIT |
| DIRECTORY ADD | Не отправляется: имя уже ссылается на object_id |

Writer разрешает путь по capture, выбирает его канонический FILE_COMMIT и
проверяет parent_id. В новый приватный state-dir до сетевых изменений
сохраняются data.bin и intent: replace=true, прежние base_commit_id и
base_version_id, UUID новой версии/операции, хеши и подготовленный commit.
Это диагностическая база, не compare-and-swap и не запрет устаревшего capture.

Затем отправляются новые CHUNK и FILE_COMMIT с полным parts document.
Перед FILE_COMMIT повторно проверяется закреп SUPERBLOCK. Публикация новой
версии происходит при принятии FILE_COMMIT: до него прежняя версия остаётся
текущей, после него выбирается новая. Затем заполняются file_commit_id и
next_message собственных частей. У FILE_COMMIT next_message=null, поскольку
новая DIRECTORY не отправляется. Обязательное поле навигации сохраняется.

Это существующее правило FILE_COMMIT v1: null backlink CHUNK допустим, если
часть действительно включена в parts; заданный backlink обязан совпадать с
каноническим commit. Его заполнение не является дополнительным commit-сигналом.
Правила чтения/канонизации и формат записей не меняются.

Если после принятого FILE_COMMIT произошла ошибка edit/backlink, команда
возвращает ошибку, но новое содержимое уже может читаться. intent остаётся
в published_backlinks_pending. При потере ответа commit phase будет
commit_response_uncertain, а сам commit мог появиться; нужно проверить новый
scan по сохранённым operation_id/object_id/version_id. Слепого retry/resume нет.
Существующий state-dir блокирует повторную отправку.

Две машины могут опубликовать разные версии на основании одного старого
capture. По принятому правилу побеждает последняя каноническая публикация
FILE_COMMIT по исходному message_id канала. Время компьютера, возраст capture
и base_version_id не делают изменение конфликтным. Повтор того же operation_id
приоритет не повышает. CLI не выполняет автоматический retry после ошибки.

## Старые версии и чтение

Старые CHUNK/FILE_COMMIT и DIRECTORY writer не редактирует и не удаляет.
Поэтому read/mount прежнего capture продолжает читать старую версию, даже
после замены. Новая версия появляется в новом scan; уже работающий mount
фиксирует прежний view до remount. Локальный object inode не является
удалённым UUID или Telegram ID.

Безопасный GC старых версий пока не реализован. Немедленное удаление нарушило
бы чтение прежних captures/handles; без координатора нельзя предполагать,
что другие инстансы больше ими не пользуются. Это текущий предел прототипа,
а не бессрочная политика хранения. Также остаются открыты resume/recovery,
rename и write-FUSE. Логическое удаление имени доступно через [unlink](UNLINK.md). Пределы put прежние: до 64 MiB и 4096 частей.
Hardlink-контракт не реализован: замена требует совпадения parent_id выбранного
commit с родителем имени.

## Проверка на тестовом канале

Пользователь подтвердил mkdir /documents и put/read /documents/hello.txt на
capture-008. Следующий свободный журнал — capture-009. Из корня проекта:

```bash
git pull --ff-only
make

tgfs_net=(
  --token-file "$HOME/.config/tgfs/secrets/test.token"
  --proxy socks5h://127.0.0.1:10808
)
printf 'updated in documents\n' > "$HOME/.local/state/tgfs/replacement-source.txt"

./build/tgfs put "${tgfs_net[@]}" \
  --capture ./capture-008 \
  --file "$HOME/.local/state/tgfs/replacement-source.txt" \
  --parent /documents --name hello.txt --replace \
  --state-dir "$HOME/.local/state/tgfs/replace-documents-hello-001"

./build/tgfs scan "${tgfs_net[@]}" \
  --storage-chat-id -1003916921678 \
  --service-chat-id -1004400939172 \
  --output ./capture-009

./build/tgfs read "${tgfs_net[@]}" --capture ./capture-009 cat /documents/hello.txt
./build/tgfs read "${tgfs_net[@]}" --capture ./capture-008 cat /documents/hello.txt
```

Первый cat должен дать `updated in documents`, второй — `hello from documents`.
Имя в ls /documents остаётся единственным hello.txt. Успешный put выводит
прежний object_id, новый commit_id, directory_id=0 и replaced=1.
Токен остаётся локальным. Живая замена ещё ожидает подтверждения пользователя.

## Проверки

Добавлены сквозные сценарии: замена multipart → empty → multipart, неизменный
object_id, новые UUID версий/операций, отсутствие ADD, старый и новый capture,
две публикации из устаревшего capture, неизменность старых сообщений,
отсутствующий/каталожный target, потеря ответа CHUNK/FILE_COMMIT, смена pin до
commit и ошибка backlink после публикации. Kernel test монтирует новый remote
capture через настоящий curl на HTTP double и проверяет cat/pread/EROFS.

Всего 107 тестов; локально 104 passed и 3 kernel skips из-за отсутствия
/dev/fuse. ASan/UBSan запускаются с detect_leaks=0 из-за ptrace. Живое обращение
к Telegram этими тестами не заменяется.

Живой тест замены /documents/hello.txt и сравнение capture-008/009 подтверждены
пользователем. Следующий этап — [unlink](UNLINK.md); rename/GC/recovery открыты.
