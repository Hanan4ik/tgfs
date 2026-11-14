# Bot API транспорт и живая проверка

Реализовано 2026-11-14 на C/libcurl (>= 7.85). Это сетевой слой и отдельная
команда probe; ls/cat пока используют локальный snapshot. Сканер канала,
SQLite, FUSE и публикация файлов TGFS ещё не подключены.

## Сборка

Arch Linux: `sudo pacman -S --needed base-devel openssl curl python`.
Debian/Ubuntu: `sudo apt-get install build-essential libssl-dev libcurl4-openssl-dev python3`.
Из корня репозитория: `make`. Проверки: `make test`, `make sanitize`.

## Проверка на тестовых каналах

В Bash сохраните токен локально, не помещая его в командную строку или Git:

```bash
install -d -m 700 "$HOME/.config/tgfs/secrets"
(
  umask 077
  read -r -s -p 'Токен тестового бота: ' tgfs_token
  printf '\n' >&2
  printf '%s\n' "$tgfs_token" > "$HOME/.config/tgfs/secrets/test.token"
  unset tgfs_token
)
chmod 600 "$HOME/.config/tgfs/secrets/test.token"
./build/tgfs probe \
  --token-file "$HOME/.config/tgfs/secrets/test.token" \
  --storage-chat-id -1003916921678 \
  --service-chat-id -1004400939172
```

Здесь использованы полные Bot API ID для указанных пользователем каналов.
getChat проверяет возвращённые ID и тип channel перед отправкой. Бот должен
иметь возможность публикации в обоих каналах; пересылка должна быть разрешена.

Последовательность probe:

1. getMe и getChat обоих каналов.
2. sendDocument: 4096 случайных байт с именем tgfs-probe.bin.0 в хранилище.
3. editMessageCaption: изменение подписи собственного сообщения.
4. forwardMessage: пересылка в служебный канал; сверка исходного канала и
   forward_origin.message_id с ID загрузки. ID копии проверяется отдельно.
5. getFile по file_id пересланного document; скачивание и сверка размера,
   всех байтов и SHA-256.

Успех: строка `PASS transport probe`, код 0. Ошибка операции: код 1;
неверные аргументы: 2. В каналах остаются **два тестовых сообщения**; их ID
печатаются для ручной проверки/удаления. Подпись имеет magic TGFS_PROBE:
это не CHUNK и не инициализация тома. Автоматического удаления нет.
Тест 4096 байт не подтверждает граничный размер 20 MiB.

## Интерфейс и ограничения

include/tgfs/bot_api.h предоставляет синхронный клиент с одним потоком-владельцем.
JSON-вызовы: getMe, getChat, getFile, sendMessage, forwardMessage,
editMessageText, editMessageCaption. Отдельно — multipart sendDocument с
заданным filename и скачивание по относительному file_path. Вызывающий код
освобождает result через cJSON_Delete и скачанные байты через free.

Публичный HTTPS endpoint фиксирован; проверка TLS включена, редиректы
запрещены. Поддерживаются стандартные proxy-переменные libcurl. Токен
читается из обычного файла владельца без доступа группы/остальных, symlink
отвергается. Диагностика не выводит URL, токен или сырые ответы сервера.

Лимиты прототипа: API JSON до 1 MiB; загружаемый/скачиваемый document до
20 971 520 байт; caption до 1024 **байт** (консервативный локальный предел).
Upload/download буферизуются в памяти. Относительный file_path допускает
ASCII буквы, цифры, `_-.` и разделители `/`, без `.`/`..` сегментов.
Незнакомый формат пути отвергается. Пустой document не отправляется.

Таймаут запроса по умолчанию 60 секунд, `--timeout-seconds` принимает 1–300.
Повторов нет: потеря ответа на запись может означать уже выполненную запись.
Ошибка возвращает категорию local/network/http/api/format/limit, HTTP/API
коды, retry_after и outcome_unknown. При outcome_unknown=1 сначала проверьте
канал: повторный запуск probe создаёт новые сообщения. Это базовая диагностика,
а не завершённая классификация и recovery из #10.

## Проверки и состояние эксперимента

Локальный HTTP double проверяет multipart/filename, JSON-запросы, изменение
caption, разные ID источника/копии, скачивание, повреждение байтов, неправильный
forward_origin, небезопасный file_path, дубли ключей JSON, слишком большой
ответ, запрет редиректов, 429/retry_after, потерю ответа без повторной отправки
и права файла токена. Секрет не должен попадать в вывод.

Только build/tgfs-test собирается с TGFS_TESTING и принимает
TGFS_TEST_API_BASE=http://127.0.0.1:PORT. Рабочий build/tgfs игнорирует эту
переменную; тестовый бинарник нельзя использовать с настоящим токеном.

Локально прошли 30 тестов ядра и 7 тестов транспорта (включая подслучаи),
также ASan/UBSan с detect_leaks=0 из-за ptrace среды. Живой Telegram-тест выполнен пользователем 2026-11-14 через proxychains: все шаги PASS, 4096 байт, source_id=2 и copy_id=2 в разных каналах. SHA-256: `5aab18ea9659e7bd091b79d633c092fa67d846bf0d3b3c729dbc1f4cc7b29366`. Подтверждены document.file_name, изменение caption и forward_origin. Граничный размер и остальные эксперименты #6 не проверены. Токен остаётся у пользователя.

Документация API: [Bot API](https://core.telegram.org/bots/api),
[sendDocument](https://core.telegram.org/bots/api#senddocument),
[getFile](https://core.telegram.org/bots/api#getfile),
[MessageOriginChannel](https://core.telegram.org/bots/api#messageoriginchannel).

## Прокси без proxychains

```bash
./build/tgfs probe \
  --token-file "$HOME/.config/tgfs/secrets/test.token" \
  --storage-chat-id -1003916921678 \
  --service-chat-id -1004400939172 \
  --proxy socks5h://127.0.0.1:10808
```

`--proxy` поддерживает http://, https://, socks5:// и socks5h://.
Для SOCKS5 используйте socks5h, если DNS должен разрешаться на прокси.
Явный прокси применяется ко всем запросам, включая multipart и скачивание,
и имеет приоритет над proxy/NO_PROXY переменными окружения. Если прокси
недоступен, запрос завершается ошибкой без перехода на прямое соединение.
`--direct` отключает прокси окружения; вместе с `--proxy` не допускается.
Без этих аргументов сохраняется стандартное поведение libcurl:
HTTPS_PROXY/https_proxy, ALL_PROXY/all_proxy и NO_PROXY/no_proxy.

Пароли в аргументе --proxy не принимаются (URL с userinfo отвергается).
Для прокси с авторизацией пока используйте стандартные переменные окружения
libcurl, заданные вне истории shell; не сохраняйте секреты в Git или mount options.
Полный конфигурационный файл TGFS пока не подключён.

Проверено локальным SOCKS5-сервером: весь probe, включая download, проходит
через прокси; недоступный прокси не вызывает прямого запроса; credentials
в CLI отвергаются без вывода секрета. Работоспособность встроенного прокси
с Telegram предстоит подтвердить повторным запуском пользователя.

Семантика: [CURLOPT_PROXY](https://curl.se/libcurl/c/CURLOPT_PROXY.html),
[CURLOPT_NOPROXY](https://curl.se/libcurl/c/CURLOPT_NOPROXY.html).
