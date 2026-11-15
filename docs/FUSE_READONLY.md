# FUSE: монтирование сохранённого журнала в режиме чтения

Реализовано 2026-11-15. `tgfs mount` открывает один неизменяемый capture и
предоставляет каталоги/обычные файлы через libfuse3. Поддержаны getattr,
readdir с offsets, open/read/release, EOF и чтение диапазонов. Изменения
отвергаются с EROFS. Команда остаётся в foreground, с одним потоком FUSE.

## Установка и запуск

Arch/CachyOS: `sudo pacman -S --needed fuse3 pkgconf`.
Debian/Ubuntu: `sudo apt-get install libfuse3-dev fuse3 pkg-config`.
Остальные зависимости (SQLite, libcurl, OpenSSL, Make/C11) сохраняются.
Затем обновить main и выполнить make.

В первом терминале, из корня репозитория:

```bash
mkdir -p "$HOME/mnt/tgfs"

./build/tgfs mount \
  --capture ./capture-006 \
  --mountpoint "$HOME/mnt/tgfs" \
  --token-file "$HOME/.config/tgfs/secrets/test.token" \
  --proxy socks5h://127.0.0.1:10808 \
  --cache-stats
```

Mountpoint должен существовать, быть пустым каталогом текущего пользователя,
без symlink конечного компонента. Требуется доступ к /dev/fuse и fusermount3.
После `OK mount ready (read-only, foreground)` процесс продолжает работать;
отсутствие приглашения shell ожидаемо. Фоновой daemonization пока нет.

Во втором терминале:

```bash
ls -l "$HOME/mnt/tgfs"
cat "$HOME/mnt/tgfs/hello.txt"
cat "$HOME/mnt/tgfs/multipart.txt"
wc -c < "$HOME/mnt/tgfs/empty.txt"
```

Ожидаются hello.txt/multipart.txt/empty.txt, затем `hello tgfs`, `abcdefghij`
с переводами строк и размер пустого файла 0. Это обычные Linux-команды, не
CLI-адаптер `tgfs read`. Для проверки чтения диапазона:

```bash
python3 - <<'PY'
import os
from pathlib import Path
with (Path.home() / 'mnt/tgfs/multipart.txt').open('rb', buffering=0) as f:
    print(os.pread(f.fileno(), 5, 2))   # b'cdefg': через границу частей
    print(os.pread(f.fileno(), 1, 11))  # b'': EOF
PY
```

Проверка режима только чтения:

```bash
touch "$HOME/mnt/tgfs/new.txt"
```

Ожидается `Read-only file system`. Для завершения, из второго терминала:

```bash
fusermount3 -u "$HOME/mnt/tgfs"
```

Первый процесс завершается и печатает итоговые cache stats. Ctrl-C в первом
терминале также завершает foreground mount через обработчик libfuse.

## Offline и локальный пример

Опции cache/proxy/timeout соответствуют read: --offline, --refresh-cache,
--no-cache, --cache-dir, --cache-limit-mib, --cache-stats.
Для offline должны быть заранее получены metadata и нужные CHUNK. Непрочитанный
ранее файл может дать EIO из-за cache miss; в stderr будет причина. По умолчанию
онлайн-режим скачивает недостающие parts через настроенный proxy.

Без Telegram можно смонтировать поставляемый пример:

```bash
./build/tgfs mount \
  --snapshot examples/directory-merge \
  --mountpoint "$HOME/mnt/tgfs"
```

В этом примере файлы находятся внутри documents. Snapshot и capture режимы
взаимоисключающие; snapshot не принимает token/proxy/cache опции.

## Границы сессии

Metadata, слияние каталогов и выбранные версии подготавливаются один раз до
входа в FUSE loop. Рабочий временный metadata-каталог удаляется после загрузки;
дерево и FILE_COMMIT остаются в памяти remote session. Каждый открытый handle
хранит выбранный FILE_COMMIT и refs частей до release. Изменение journal.json
или новая публикация в канале не меняют открытое дерево и дескрипторы.
Для обновления нужен новый scan и remount с новым --capture.

Lookup/getattr/readdir/open используют локальные metadata без скачивания частей
или повторного сканирования. Части запрашиваются только при read и только если
пересекаются с диапазоном. Первая часть определяется бинарным поиском по offsets.
Одна целая часть проверяется по размеру и SHA-256 до копирования байтов приложению.
Последняя проверенная часть хранится в RAM (до 20 MiB) и переиспользуется между
handle одного fs; остальные части доступны в persistent cache по его лимитам.

При последовательном чтении handle от 0 до EOF дополнительно проверяется
SHA-256 целого файла. Случайные/непоследовательные чтения гарантируют проверку
пересекающихся частей; общий SHA требует полного последовательного чтения
(либо существующей команды tgfs read ... cat). Чтение от offset 0 начинает
последовательную проверку заново. Если общий SHA не совпал, последний read
возвращает EIO; ранее возвращённый префикс нельзя считать подтверждением всего
файла. Пустой файл проверяет свой общий SHA при open и возвращает EOF без CHUNK.

FUSE direct_io включён, чтобы чтения проходили через проверяемые handlers.
Single-threaded loop сериализует доступ к curl/SQLite/RAM и concurrent requests:
следующий запрос того же chunk ожидает окончания первого, затем использует RAM
или SQLite. Долгий сетевой запрос может задержать и другие запросы mount;
ограничение --timeout-seconds остаётся действующим. Параллельный worker pool
и мгновенная отмена сетевых запросов ещё не реализованы.

## Метаданные и ошибки

- Права синтетические: каталоги 0555, файлы 0444; uid/gid текущего пользователя.
  Времена пока epoch 0, не время Telegram-поста. Спецбиты/удалённые UID/GID,
  symlink и полный hardlink-контракт остаются за пределами этого MVP.
- root inode=1, остальные inode назначаются локально без коллизий внутри
  snapshot; object_id остаётся удалённой идентичностью. Директории получают
  nlink=2+число непосредственных подкаталогов; файлы пока nlink=1.
- Mount options фиксированы: ro, default_permissions, nodev, nosuid. allow_other
  и произвольные FUSE -o параметры пока не предоставляются. Секреты не передаются
  в mount options/argv libfuse; токен читается клиентом из owner-only файла.
- ENOENT — неизвестный путь, ENOTDIR — файл вместо компонента-каталога,
  EISDIR — открытие каталога как файла, EBADF — неверный handle. Запись/создание/
  удаление/rename/chmod/chown/xattr/truncate/fallocate возвращают EROFS.
- Ошибки формата, недоступные refs, offline cache miss, SHA и backend/network
  пока дают EIO; backend reason выводится в stderr. Детальная errno/retry
  классификация задачи #10 этим не закрывается. Нули вместо отсутствующих
  байтов не возвращаются. Failed read не отдаёт приложению частичный буфер.
- statfs сообщает нулевую неизвестную ёмкость и read-only состояние. Это не
  оценка предела канала и не обещание свободного места.
- На выходе закрываются даже handles, для которых release не был вызван.
  SQLite/CURL и metadata живут до конца сессии. Не удаляйте файлы базы кэша
  вручную во время mount: сначала завершите использующие её процессы.

CLI put остаётся отдельной операцией публикации. Запись через смонтированный
каталог не поддерживается. Интеграция mount -t tgfs/fstab и профилей ещё открыта.
Следующий свободный пользовательский scan — capture-007; для первого mount
подходит уже существующий capture-006.

## Проверки

Добавлены 13 тестов с подслучаями. 12 проверяют настоящую таблицу FUSE callbacks
через C driver: stat/errno, offsets readdir при малом буфере, все EROFS handlers,
чтение диапазонов/EOF, RAM cache между handles, SHA частей/целого файла, missing
refs, пустой файл и срок жизни фиксированной remote session (curl/SQLite/offline).
Test-only подстановка контекста не компилируется в production binary.

Отдельный kernel test действительно монтирует snapshot при наличии /dev/fuse
и fusermount3, проверяет обычные stat/read/pread/EROFS и unmount. В локальной
среде /dev/fuse отсутствует: этот тест пропущен с явной причиной; успешный живой
mount здесь не заявляется. Остальные тесты и ASan/UBSan прошли с detect_leaks=0
из-за ptrace. Всего 101 тест (локально 100 успешных, 1 kernel skip).
CI Ubuntu 24.04 выполнил все 101 тест без пропусков, включая настоящий
kernel mount/unmount, в обычной сборке и ASan/UBSan.

Первый живой mount Telegram должен подтвердить пользователь на своём Linux.
Основание API: [libfuse operations](https://libfuse.github.io/doxygen/structfuse__operations.html).
