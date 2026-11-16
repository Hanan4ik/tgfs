# TGFS — Telegram File System

Файловая система для Linux, в которой Telegram-канал хранит документы с данными и служебный каталог.

**Стадия: начата реализация ядра чтения на C.** Есть CLI для локального снимка сообщений; Добавлены Bot API транспорт и команда живой проверки probe; добавлен сбор журнала канала командой scan; добавлено удалённое чтение ls/cat по capture; добавлена публикация нового файла в каталог через put --parent; добавлен read-only FUSE mount сохранённого журнала. Выбран Bot API для первой версии; остальные технические решения остаются предложениями до согласования. Лицензия репозитория сохранена без изменений.

## Быстрый запуск прототипа

```sh
make
./build/tgfs --help
./build/tgfs --snapshot examples/hello ls /documents
./build/tgfs --snapshot examples/hello cat /documents/hello.txt
make test
```

Зависимости: компилятор C11, Make, OpenSSL libcrypto, libcurl >= 7.85, SQLite >= 3.31, libfuse3 и pkg-config; для тестов Python 3.
[Описание реализации и границ прототипа](docs/IMPLEMENTATION.md).

[Транспорт и запуск проверки на тестовых каналах](docs/BOT_API_TRANSPORT.md).

`tgfs --help` (также `-h`) описывает все команды и параметры;
`tgfs probe --help` показывает справку живой проверки. Запуск без аргументов
выводит общую справку в stderr с кодом 2. Запрос справки возвращает 0 и не обращается к сети.

[Сканирование канала и индекс записей](docs/CHANNEL_SCAN.md).

[Инициализация тестового тома](docs/INITIALIZATION.md): `tgfs init --help`.

[Слияние изменений каталогов](docs/DIRECTORY_OPERATIONS.md) — ADD/REMOVE и локальный пример.

[Чтение файлов из Telegram](docs/REMOTE_READ.md): `tgfs read --help`.

[Публикация нового файла и живая проверка](docs/PUT.md): `tgfs put --help`.

[Создание каталогов и загрузка по пути](docs/DIRECTORY_WRITE.md): `tgfs mkdir --help`, `tgfs put --parent /documents`.

[Замена содержимого файла](docs/FILE_REPLACE.md): `tgfs put --replace`.

[Удаление имени файла](docs/UNLINK.md): `tgfs unlink --help`.

[Проверка сохранённых намерений](docs/INTENT_CHECK.md): `tgfs intent --help`.

[Ограниченный ремонт backlinks](docs/BACKLINK_REPAIR.md): `tgfs intent --repair-backlinks`.

[Восстановление публикации имени](docs/RESUME_DIRECTORY.md): `tgfs intent --resume-directory`.

[Завершение подтверждённого put](docs/FINALIZE_PUT.md): `tgfs intent --finalize`.

[Завершение mkdir/unlink](docs/FINALIZE_METADATA.md): `tgfs intent --finalize`.

[Постоянный индекс, кэш и offline](docs/PERSISTENT_CACHE.md) — включены по умолчанию в read.

[Монтирование через FUSE](docs/FUSE_READONLY.md): `tgfs mount --help`.

## Навигация

- **[Структуры CHUNK, DIRECTORY и FILE_COMMIT](docs/README.md)** — caption, содержимое document и поля каждой записи.

- [Состояние и следующий вопрос](docs/STATE.md) — короткая точка входа для следующих сессий.
- [Исходные требования и оценка](docs/REQUIREMENTS.md).
- [Словарь терминов с примерами](docs/GLOSSARY.md).
- [Архитектура и контракт FUSE](docs/ARCHITECTURE.md).
- [Создание файла: поля, сообщения и сбои](docs/CREATE_FILE_SCENARIO.md).
- [Формат хранения и восстановление](docs/PROTOCOL.md).
- [Открытые сценарии сбоев и варианты решений](docs/UNRESOLVED_FAILURE_SCENARIOS.md).
- [Конфигурация и монтирование](docs/CONFIGURATION.md).
- [Проверенные ограничения Telegram](docs/TELEGRAM.md).
- [Канбан](docs/KANBAN.md) и [GitHub Issues](https://github.com/Hanan4ik/tgfs/issues).
- Решения: [ADR-0001: транспорт — accepted](docs/decisions/0001-transport.md), [ADR-0002: каталог](docs/decisions/0002-catalog.md).

## Рабочая гипотеза

FUSE предоставляет обычные файлы Linux. Процесс TGFS переводит файловые операции в работу с локальным индексом и Telegram backend. Части содержимого отправляются как document; удалённый каталог задаёт дерево имён, inode и версии. Закреплённое сообщение указывает на последний завершённый каталог.

Предлагаемый интерфейс с одним типом файловой системы и несколькими профилями:

```sh
mount -t tgfs default /media/channel1
mount -t tgfs channel2 /media/channel2
```

Это удалённое хранилище поверх FUSE, с опытом использования, похожим на sshfs. Совместимость с протоколом NFS не предполагается.

## Как развиваем проект

Обсуждаем один вопрос в одной Issue; фиксируем вывод в нужном документе и ADR; обновляем STATE и одну карточку канбана. Не генерируем весь план заново. Эксперименты и программирование начинаются отдельным этапом после согласования соответствующих решений.
