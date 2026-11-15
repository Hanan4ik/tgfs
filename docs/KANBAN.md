# Канбан проектирования

Обновлено: 2026-11-14. Это Markdown-канбан, связанный с реальными GitHub Issues; GitHub Project с перетаскиваемыми карточками не создан. Статусы колонок ведём здесь, критерии — в Issues. Не делать вид, что документированный вариант уже согласован.

| Backlog | Готово к обсуждению | Обсуждаем | Готово |
| --- | --- | --- | --- |
| [#6 Bot API эксперимент](https://github.com/Hanan4ik/tgfs/issues/6) | — | [#3 FUSE и контракт MVP](https://github.com/Hanan4ik/tgfs/issues/3) | [#1 Выбран Bot API](https://github.com/Hanan4ik/tgfs/issues/1) |
| [#7 Сбои записи FUSE](https://github.com/Hanan4ik/tgfs/issues/7) | [#4 Профили и mount](https://github.com/Hanan4ik/tgfs/issues/4) | [#2 Каталог и восстановление](https://github.com/Hanan4ik/tgfs/issues/2) | Начальная документация и задачи |
| — | [#5 Части и целостность](https://github.com/Hanan4ik/tgfs/issues/5) | — | [#8 Read-only MVP: проверен online/offline](https://github.com/Hanan4ik/tgfs/issues/8) |
| [#9 Очистка физических дубликатов](https://github.com/Hanan4ik/tgfs/issues/9) | — | — | — |
| [#10 Классификация ошибок и несостыковок](https://github.com/Hanan4ik/tgfs/issues/10) | — | — | — |

Сводный список открытых сценариев: [UNRESOLVED_FAILURE_SCENARIOS.md](UNRESOLVED_FAILURE_SCENARIOS.md). 51 сценарий с постановкой, статусом решения и критерием проверки; наличие записи не означает выполнения карточки.

## Реализация начата

Разрешение пользователя 2026-11-14: реализован первый локальный срез чтения на C — CLI ls/cat, snapshot adapter, индекс в памяти, выбор FILE_COMMIT и проверка частей. [Сборка и ограничения](IMPLEMENTATION.md). #8 переходит к реализации ядра чтения; Telegram/FUSE и критерии read-only MVP ещё не выполнены. Статусы архитектурных задач и экспериментов не повышаются наличием кода.

## Правила

- Новые ответы по MVP: [MVP_DESIGN_DECISIONS.md](MVP_DESIGN_DECISIONS.md). Каталог-document и перенос parts в document выбраны как направление; точные поля и публикация ещё обсуждаются. #10 — отложенная классификация ошибок формата/API/данных/локального слоя, без детального проектирования сейчас.

- Текущий фокус — #2 совместно с #3: точные поля commit, генерация ID и контракт записи. FILE_COMMIT, refs частей, общий object_id и последняя публикация как текущая версия содержимого согласованы; правило повторов operation_id также принято; UUIDv4 для object_id выбран, SUPERBLOCK собран в [SUPERBLOCK_V1.md](SUPERBLOCK_V1.md); рассмотренный пример частей и встроенного FILE_COMMIT записан в [FILE_COMMIT_V1.md](FILE_COMMIT_V1.md). Следующий вопрос — создание имени с первой версией; большие манифесты, генераторы ID и parser limits ещё открыты. [ADR-0003](decisions/0003-journal-discovery.md) принят: маркер границы и обязательный next_message; #2 остаётся открытой, точный формат и rename/delete ещё обсуждаются. #6: первый живой probe пройден; остальные эксперименты открыты. #7 остаётся Backlog.
- «Готово к обсуждению» означает наличие постановки, не готовность реализации.
- Перемещать в «Готово» после критериев Issue и фиксации решения/результата в документах.
- #8 начата локальным ядром чтения. Полный read-only MVP требует Telegram backend, проверки #6 и контракта FUSE.
- Задача #9 создана по явному требованию пользователя; механизм очистки ещё не реализован. Последующие этапы записи, GC и multiwriter не дробим на десятки карточек, пока не приняты базовые решения.
- При изменении карточки синхронизировать статус в Issue body и STATE; новые задачи искать среди существующих, чтобы не создавать дубликаты.

## Транспорт (2026-11-14)

Реализованы libcurl-клиент и команда [probe](BOT_API_TRANSPORT.md), проверенные локальным HTTP double. Пользователь выполнил живой probe через proxychains: PASS. Добавлена явная настройка прокси. Остальные эксперименты и критерии #6/#8 не завершены.

## Сбор журнала (2026-11-14)

В #8 добавлена команда [scan](CHANNEL_SCAN.md) с маркером, индексом записей и явной неопределённостью недоступных ID. Локальные тесты пройдены. Удалённое чтение/FUSE, живой scan и завершение архитектуры DIRECTORY остаются открытыми.

## Инициализация (2026-11-14)

Добавлена команда [init](INITIALIZATION.md): явное подтверждение нового тома, проверка прав/pin, устойчивый UUIDv4 intent, placeholder/edit/pin/verify. Автоматического resume и защиты от параллельного init нет. 47 локальных тестов проходят; живой init не выполнен. Следующий этап — проверка init/scan пользователем и подключение документов к удалённому чтению; FUSE ещё не реализован.

## Живая проверка init/scan (2026-11-14)

Пользователь подтвердил init: start_id=3, root_object_id=79516260-85d5-4767-8285-c53ccfb868a5. Scan создал маркер 5 и получил HTTP/API 400 при пересылке ID 4. Вероятна служебная запись о закреплении, но описание ошибки прежний клиент скрывал: причина ещё не подтверждена. Добавлены requested_id и ограниченная диагностика API с фильтрацией токена/URL; дополнительные варианты отказа пересылки сохраняются как недоступные ID с неполным покрытием. 48 локальных тестов прошли; нужен повторный живой scan.

Уточнение живого scan: Telegram вернул для ID 4 точное описание `Bad Request: the message can't be forwarded`. Добавлен этот вариант и сквозной тест; обход продолжает работу с unavailable_ids/coverage_verified=false (код 3). Тип содержимого ID 4 этим ответом не установлен; он не объявляется пустым или безопасно пропускаемым.

## Принятое правило пропусков (2026-11-14)

Пользователь гарантирует разрешённую пересылку в TGFS-канале. В основном обходе любой HTTP/API 400 от forwardMessage считается несуществующим/служебным ID, без анализа description. Удалён список строк ошибок. Обход возвращает 0 при завершении; unavailable_ids и физический coverage_verified сохраняются, добавлены scan_completed/gap_policy. 403/429/timeout и ошибки других методов останавливают обход; SUPERBLOCK должен читаться. 48 тестов и локальные ASan/UBSan прошли. Это заменяет прежнюю трактовку пропусков с кодом 3.

## Слияние каталогов (2026-11-15)

Пользователь согласовал независимое объединение добавлений и явные удаления. Реализовано в ядре snapshot: DIRECTORY operations ADD/REMOVE с UUIDv4 operation_id, observed_adds, дедупликацией и сохранением удалений при повторах. Корень — root_object_id/parent_id=null; пустой корень объявляет SUPERBLOCK. Конфликт одинакового имени разных объектов — ошибка, выбор победителя не согласован. Старые entries не объединяются автоматически. [Кодирование и пример](DIRECTORY_OPERATIONS.md). 61 локальный тест и ASan/UBSan прошли; writer/сетевой импорт документов/FUSE ещё открыты.

## Удалённое чтение (2026-11-15)

Реализован [read ls/cat](REMOTE_READ.md) по capture: getChat, скачивание DIRECTORY/FILE_COMMIT documents, восстановление дерева/операций, ленивое скачивание выбранных CHUNK с проверкой размера и SHA-256. Пустые файлы/корень и multipart поддерживаются. 68 локальных тестов и ASan/UBSan прошли. Живого read ещё нет; metadata временные, без постоянного cache/SQLite. Следующий этап — публикация тестовых данных и живое чтение, затем постоянный индекс и FUSE.

## Публикация нового файла (2026-11-15)

Добавлен [put](PUT.md): новый объект в корень, staging/UUID intent, CHUNK → FILE_COMMIT → backlinks → DIRECTORY ADD, навигация собственных записей и проверка ответов. Поддержаны пустой файл и multipart; проверяется имя в capture и закреп. Автоматического retry/resume/overwrite нет; stale capture может дать конфликт одинаковых имён. 72 локальных теста включают полный put→scan→read и независимые ADD; живой put ещё не выполнен. Пользователь подтвердил живое read пустого корня. Следующий этап — живой непустой файл (capture-005), постоянный индекс/cache и FUSE.

## Живой цикл и постоянный кэш (2026-11-15)

Пользователь подтвердил put→scan→read: hello.txt (11 байт), multipart.txt (3 части/11 байт), empty.txt (0 частей/0 байт), объединённый ls и пустой cat с кодом 0. capture-006: B=32, records=17, unavailable=11, cleanup_pending=0. Следующий свободный scan — capture-007.

Добавлен [SQLite-индекс и кэш read](PERSISTENT_CACHE.md): сохраняет adapter/records и проверенные blobs, scopes по bot/channel/root/start и конкретному capture. Warm read переиспользует metadata/части, offline не обращается к API; refresh/no-cache/stats и LRU budget доступны явно. Intents put/init отделены от кэша. 88 тестов; живая проверка кэша ещё требуется. Следующий этап — read-only FUSE; готовый mount пока не заявляется.

## Read-only FUSE (2026-11-15)

Пользователь подтвердил постоянный cache: ls miss/downloads=6 → hit/downloads=0; multipart cat downloads=3 → offline hits=9/downloads=0.

Реализован [mount](FUSE_READONLY.md) локального snapshot либо Telegram capture: фиксированный metadata view, file handles с FILE_COMMIT, stat/readdir/range reads, один RAM chunk и SQLite, typed path errno и EROFS для изменений. 101 тест: локально 100 успешных и 1 kernel mount skip (нет /dev/fuse); callbacks и remote session/offline проверены, ASan/UBSan проходят. Живой Telegram mount ещё не проверен. Следующий свободный scan — capture-007, mount использует capture-006. Полный write-FUSE/профили/детальный recovery остаются открытыми.

## Каталоги и вложенная публикация (2026-11-15)

Пользователь подтвердил живой online/offline FUSE capture-006, ls/cat/stat,
empty и EROFS. Read-only MVP #8 выполнен; общий Linux write-контракт #3 открыт.
Добавлены [mkdir и put --parent](DIRECTORY_WRITE.md): пустой DIRECTORY → ADD
родителя, разрешение UUID по capture, правильные parent_id дочерних и
родительских документов, приватный intent и навигация только своих записей.
Нет coordinator, mkdir -p, overwrite, resume или write-FUSE.
104 теста (локально 102 passed, 2 kernel skip), ASan/UBSan; живые вложенные
mkdir/put ещё не проверены. Следующий шаг пользователя: mkdir /documents на
capture-006 → scan capture-007 → put --parent /documents → scan capture-008.

## Замена файла (2026-11-15)

Пользователь подтвердил живые mkdir /documents и put/read /documents/hello.txt
на capture-008. Добавлен [put --replace](FILE_REPLACE.md): прежний object_id,
новые version_id/operation_id, FILE_COMMIT публикует содержимое без нового ADD.
Проверяется существующий обычный файл и его parent; base refs сохраняются
в intent для диагностики, а не CAS. Конкурентные версии из старого capture
подчиняются принятому latest canonical commit. Старые сообщения остаются
для прежних captures/mount; GC и write-FUSE не реализованы.
107 тестов (локально 104 passed/3 kernel skips), ASan/UBSan. Следующий шаг —
живая замена /documents/hello.txt → scan capture-009 и сравнение cat старого
capture-008/нового capture-009. Назначение независимых captures пояснено
в [CHANNEL_SCAN.md](CHANNEL_SCAN.md).

## Логическое удаление имени файла (2026-11-15)

Пользователь подтвердил живую замену /documents/hello.txt, capture-009 и
сравнение с capture-008. Добавлен [unlink](UNLINK.md): разрешение обычного файла,
сбор всех живых наблюдённых ADD тем же merge, один DIRECTORY REMOVE, приватный
intent и диагностика неизвестного ответа. Старые captures/сообщения сохраняются;
устаревший REMOVE не гасит новый независимый ADD. Legacy entries без ADD UUID
не мигрируются, каталоги отвергаются; rmdir/rename/GC/resume/write-FUSE открыты.
110 тестов (локально 106 passed/4 kernel skips), ASan/UBSan. Следующий шаг —
unlink /documents/hello.txt на capture-009 → scan capture-010 → пустой ls,
ENOENT нового cat и сохранённый cat прежнего capture.

## Диагностика намерений (2026-11-15)

Пользователь подтвердил живой unlink/capture-010. Добавлена read-only команда
[intent](INTENT_CHECK.md): сверка put/mkdir/unlink по UUID и точному содержимому
metadata в более новом capture, отчёт исходных ID/confirmed/not_observed.
Ничего не публикует и не меняет локальное намерение. Это первый этап recovery,
не автоматическое resume: CHUNK/staging/backlinks ещё не проверяются, отсутствие
в capture не разрешает retry. 113 тестов (локально 109 passed/4 kernel skips),
ASan/UBSan. Следующая пользовательская проверка — intent завершённого unlink
с capture-010; новый scan не нужен. Дальше — проверка refs/staging и план
безопасного продолжения с теми же UUID.

## Проверка данных перед recovery (2026-11-15)

Пользователь подтвердил intent unlink: directory_ids=[43], complete/confirmed.
[intent](INTENT_CHECK.md) теперь проверяет staging и CHUNK put по размерам,
SHA/bytes, metadata и refs; сообщает ненаблюдённые/неоднозначные части и
предложения backlinks подтверждённого canonical commit. can_resume=false,
изменяющих API запросов нет. 113 тестов со сценариями повреждений/duplicates.
Следующий живой тест — intent replace-documents-hello-001 на capture-010.
Далее — ограниченный ремонт metadata и безопасное продолжение; полный resume,
GC и write-FUSE пока не реализованы.

## Ограниченный ремонт backlinks (2026-11-15)

Пользователь подтвердил put intent: commit41/CHUNK40, staging verified, проблем
нет. Добавлен явный [--repair-backlinks](BACKLINK_REPAIR.md): свежая проверка
канала/commit/parts через служебные копии, flock/журнал до мутаций, проверяемый
edit только двух навигационных полей. Уже исправленные части пропускаются,
новые публикации не создаются, local_phase не меняется. 114 тестов с lost-edit,
повтором, origin/cleanup/pin/metadata конфликтами и lock. Следующий живой тест
на прежнем state/capture-010 — no-op, искусственно ломать captions не нужно.
Полное resume, ремонт commit/directory и автоматический cleanup лога открыты.
