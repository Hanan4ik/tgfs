# Удаление имени файла через DIRECTORY REMOVE

Реализовано 2026-11-15: `tgfs unlink --path /documents/hello.txt`.
Команда удаляет имя обычного файла из выбранного каталога, публикуя один
DIRECTORY document с операцией REMOVE. CHUNK/FILE_COMMIT и старые DIRECTORY
не редактируются и не удаляются. Это логическое удаление имени; освобождение
сообщений/ёмкости канала относится к будущему GC. FUSE остаётся read-only.

## Какие ADD удаляются

Writer разрешает родительский путь и файл в выбранном capture. Он собирает
все действующие ADD этого имени/объекта в этом каталоге: дедуплицирует повторы
operation_id и исключает ADD, уже погашенные прежними REMOVE. Для извлечения
refs используется тот же алгоритм слияния, что строит дерево при чтении.

В новый DIRECTORY родителя отправляется, например:

```json
{
  "operations": [
    {
      "operation_id": "7cd5ae13-08e7-4b25-bf72-a44ccabbb874",
      "action": "REMOVE",
      "name": "hello.txt",
      "observed_adds": ["3e09af06-3bf9-443c-b564-f1f9b8785292"]
    }
  ]
}
```

observed_adds содержит **UUID операций ADD**, не Telegram ID и не object_id.
В REMOVE нет object_id/version_id/FILE_COMMIT refs. Caption содержит object_id
родителя, его parent_id (null только у корня) и обязательный next_message=null.
Новые типы записей и новые правила слияния не вводятся.

Если несколько живых ADD связывают одно имя с тем же object_id, REMOVE включает
каждый из них. Повтор старого ADD после REMOVE имя не восстанавливает.
Новый независимый ADD, которого не было в capture writer, не удаляется.
Поэтому устаревший unlink не затрагивает заново созданный файл того же имени,
если тот получил новый object_id/ADD. Одновременные разные object_id под одним
именем по-прежнему конфликтуют, пока наблюдённый REMOVE их не разрешил;
writer не выбирает произвольную сторону уже конфликтного capture.

Замена содержимого через put --replace сохраняет ADD/object_id. Поэтому
unlink по capture до замены удалит это же имя и после новой версии. Это
удаление имени по наблюдённой ADD, не условное удаление определённой версии.

## Локальное состояние и публикация

До отправки создаётся новый приватный state-dir (0700) с intent.json (0600).
Он содержит kind=unlink, path, storage_chat_id/source_boundary_id, parent_id,
целевой object_id, remove_operation_id, observed_adds и готовые caption/document.
data.bin не создаётся; содержимое CHUNK не скачивается. SUPERBLOCK pin
проверяется до подготовки и ещё раз перед удалённой публикацией.

| Phase | Смысл |
| --- | --- |
| prepared | Намерение записано, удалённой публикации ещё нет |
| directory_send_pending | Намерение перед sendDocument сохранено |
| directory_response_uncertain | Ответ потерян/отклонён/не прошёл проверку; смотреть причину и новый scan |
| complete | Ответ DIRECTORY проверен, ID сохранён, локальный intent завершён |

Имя исчезает после принятия REMOVE сервером и включения записи в новый capture.
Потерянный ответ или ошибка сохранения complete не означают, что имя осталось:
публикация могла состояться. Если исходный ID известен, он сохраняется в intent;
если ответ потерян полностью, ID может быть неизвестен. Для диагностики нужны
новый scan и remove_operation_id. Автоматического retry/resume нет.
Существующий state-dir блокирует повтор с тем же намерением.

Прежние capture/mount продолжают видеть файл; открытый mount не перестраивается.
Объект не уничтожается глобально и другие имена/новые ADD не удаляются.

## Проверка на пользовательском канале

Пользователь подтвердил put --replace и чтение capture-009. Следующий
свободный журнал — capture-010. Из корня проекта, с прежним tgfs_net:

```bash
git pull --ff-only
make

./build/tgfs unlink "${tgfs_net[@]}" \
  --capture ./capture-009 \
  --path /documents/hello.txt \
  --state-dir "$HOME/.local/state/tgfs/unlink-documents-hello-001"

./build/tgfs scan "${tgfs_net[@]}" \
  --storage-chat-id -1003916921678 \
  --service-chat-id -1004400939172 \
  --output ./capture-010

./build/tgfs read "${tgfs_net[@]}" --capture ./capture-010 ls /documents
./build/tgfs read "${tgfs_net[@]}" --capture ./capture-010 cat /documents/hello.txt
./build/tgfs read "${tgfs_net[@]}" --capture ./capture-009 cat /documents/hello.txt
```

Первый ls пустой; новый cat завершается ошибкой отсутствующего пути (код 1),
прежний cat печатает `updated in documents`. Корневой /hello.txt не затрагивается.
Новый mount capture-010 также не видит удалённое имя. Данные остаются в канале.

## Границы и проверки

- Только абсолютный путь до 4096 байт, конечное имя до 255 байт, без trailing
  slash/CR/LF/dot/dotdot. Каталог отвергается; rmdir/рекурсивного удаления нет.
- Родитель и обычный файл должны быть известны capture. Некорректный/конфликтный
  журнал отвергается до изменения канала. Повреждённая/недоступная metadata
  цели не трактуется автоматически как подтверждённый обычный файл.
- Старые DIRECTORY entries без operation_id читать по-прежнему можно, но unlink
  для них отвергается: observed_adds вывести нельзя. Автомиграции нет.
- Upload/таймаут/proxy/pacing и проверка Message соответствуют put/mkdir.
  Явный API reject и неизвестный исход сохраняются для диагностики; детальная
  классификация ошибок/retry остаётся задачей #10.
- Сквозные тесты проверяют живые/погашенные/повторные ADD, вложенный и корневой
  unlink, пустой файл, новое создание и устаревшее удаление, независимый ADD,
  старый capture, legacy entries, неверные пути/каталог, lost/mismatched ответ,
  429, смену pin и блокирование повторного state-dir. Условный kernel test
  монтирует remote capture после REMOVE, проверяет ENOENT/readdir и EROFS.
- Всего 110 тестов; локально 106 passed/4 kernel skips (нет /dev/fuse),
  ASan/UBSan с detect_leaks=0 из-за ptrace. Живой Telegram unlink пока ожидает
  подтверждения пользователя; HTTP double не заменяет живую проверку.

## Продолжение после явного отказа

После нового scan запустите `tgfs intent --resume-unlink` с теми же
`--state-dir`, `--token-file` и новым `--capture`. Уже наблюдаемый REMOVE
возвращает `directory_resume=already_observed`. Для отправки требуются
сохранённая пара pending/rejected с UUID операции, истёкший cooldown,
тот же родитель, объект и набор исходных observed_adds. Изменение цели или
неизвестный исход блокирует отправку. Команда возвращает `published` после
подтверждения. Затем выполните новый scan и `intent --finalize`.
