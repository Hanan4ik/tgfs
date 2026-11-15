# Восстановление публикации имени после FILE_COMMIT

Реализован явный `tgfs intent --resume-directory` для создания нового файла.
Это продолжение одного этапа put, не полное resume. Режим не отправляет новые
CHUNK/FILE_COMMIT, не заменяет файл и не восстанавливает удалённые имена.
С --repair-backlinks одновременно использовать нельзя: необходимые backlinks
режим проверяет/исправляет сам.

## Условия и запреты

Требуются intent kind=put, replace=false, подтверждённый canonical FILE_COMMIT,
verified staging/CHUNK/refs и отсутствие ненаблюдённых/неоднозначных частей.
Сохраняются UUID объекта, версии, FILE_COMMIT и ADD из первоначального intent.

Если ADD уже найден в capture, возвращается directory_resume=already_observed
без новой публикации. Это исторический факт: после ADD мог быть REMOVE,
поэтому режим не пытается восстановить имя, отсутствующее в текущем дереве.

Если ADD не найден, отправка допускается только из фаз backlinks_pending,
commit_send_pending или commit_response_uncertain. По порядку оригинального
writer эти фазы предшествуют **первой попытке ADD**. Для directory_send_pending,
directory_response_uncertain, published_navigation_pending и complete отсутствие
ADD не разрешает retry: исход мог быть неизвестен или имя уже удалили.
Не редактируйте phase вручную для обхода проверки.

Дополнительно проверяется backlink-repair.log. Любая прежняя запись
с event=directory_send_pending блокирует повтор, пока новый capture не
подтвердит ADD. Повреждённый/слишком большой лог также блокирует действие.
Даже явный API reject после записи pending пока требует диагностики: лог
не очищается автоматически. Ненайденный ADD сам по себе не доказывает, что
отправка не состоялась.

## Порядок выполнения

1. Берётся та же локальная flock, что у ремонта backlinks. Создание log-файла
   закрепляется fsync каталога. Намерение и staging не переписываются.
2. Проверяются существующий родительский путь, его UUID и свободное имя в
   выбранном capture. Сохранённые DIRECTORY caption/ADD сверяются с этими
   UUID, именем и исходным add_operation_id. Публикуется только этот документ.
3. Исправляются необходимые backlinks существующих CHUNK по прежнему протоколу.
   Перед записью используется свежая проверка сообщений через служебный канал.
4. FILE_COMMIT перечитывается; неизменяемые поля/document сверяются с capture.
   Перед ADD повторно проверяется закреп SUPERBLOCK.
5. До sendDocument журнал получает directory_send_pending с UUID операции ADD;
   запись завершается fsync. После этого отправляется один directory.json.
6. Проверяются возвращённые канал/ID/caption/filename/size. Подтверждённый ID
   записывается в log как directory_publication_confirmed с fsync.

При потере ответа pending остаётся: повтор со старым capture блокируется.
После нового scan совпавший ADD распознаётся как already_observed. Если ADD
не найден, слепого повторения не будет. Новый capture может иметь пропуски или
заканчиваться раньше выполнения запроса.

Проверка свободного имени относится к выбранному capture, а не даёт глобальную
блокировку канала. Параллельное независимое добавление после его маркера всё
ещё может привести к принятому конфликту имён. flock локальная; две копии
intent на разных машинах не координируются. Повторы одной ADD с тем же UUID
дедуплицируются читателем, но не устраняют лишние физические сообщения.

## Отчёт и оставшиеся этапы

При уже наблюдённой ADD: directory_resume=already_observed, resumed=false.
При новой подтверждённой отправке: directory_resume=published, resumed=true,
scope=capture_plus_recovery_response, publication=confirmed, новый ID в
directory_ids и next_step=capture_after_directory_publication.

local_phase остаётся исходной. Режим не объявляет весь intent завершённым,
не ремонтирует FILE_COMMIT.next_message на новый DIRECTORY и не изменяет
intent.json. Для подтверждения нового дерева/навигации нужен следующий scan.
can_resume остаётся false — это не универсальный сигнал готовности загрузки.
Ошибка после принятой публикации/сохранения ответа не означает откат ADD.

## Безопасный живой тест

Пользователь подтвердил предыдущий no-op ремонта commit41/CHUNK40. Для этого
режима нужен **intent создания**, а не replace:

```bash
git pull --ff-only
make
./build/tgfs intent "${tgfs_net[@]}" \
  --state-dir "$HOME/.local/state/tgfs/put-documents-hello-001" \
  --capture ./capture-010 \
  --resume-directory
```

Ожидается directory_resume=already_observed и resumed=false. ADD первоначального
создания есть в журнале, хотя имя позже удалили. Файл не должен появиться снова:
ls /documents на capture-010 по-прежнему пустой. Новый scan не нужен.
Не меняйте phase и не создавайте искусственный сбой для непустого resume.

116 тестовых методов; новые сценарии проверяют продолжение после неуспешных
backlinks, пустой файл, lost ADD response, явный reject и барьер повторной
отправки, конфликт имени и неизвестную фазу, повтор по новому capture и отсутствие
воскрешения после REMOVE. Проверяется сохранность intent и отсутствие новых
CHUNK/FILE_COMMIT. Локально 112 passed/4 kernel skips; ASan/UBSan с detect_leaks=0
из-за ptrace. Живое непустое продолжение ещё ожидает реального незавершённого
намерения. init/mkdir/unlink resume, загрузка недостающих частей, полный ремонт
навигации и автоматический cleanup log остаются открытыми.
