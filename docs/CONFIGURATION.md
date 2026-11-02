# Конфигурация и интерфейс команд

Статус: proposed; всё ниже — эскиз, команд ещё нет. [#4](https://github.com/Hanan4ik/tgfs/issues/4).

## Один тип, несколько профилей

```sh
mount -t tgfs default /media/channel1
mount -t tgfs channel2 /media/channel2
tgfs mount --profile channel2 /media/channel2

tgfs-quick new-config --channel-config ~/channel2.toml --bot-config ~/bot2.toml channel2
```

new-config проверяет TOML и создаёт профиль со ссылками на конфиги; не генерирует драйвер или новый filesystem type. Нужно явно выбрать системный или пользовательский scope; существующий профиль без явного replace не перезаписывается. Системный профиль не должен зависеть от `~` другого пользователя.

```fstab
default  /media/channel1  tgfs  ro,_netdev,nofail  0  0
channel2 /media/channel2  tgfs  ro,_netdev,nofail  0  0
```

Одноаргументное `mount -t tgfs /media/channel1` не является общим способом указать default: mount ищет запись fstab. Для короткого `mount /media/channel1` нужна такая запись. Установка предоставляет mount.tgfs; внутри это FUSE subtype tgfs, а не зарегистрированный новый драйвер на каждый профиль. См. [mount(8)](https://man7.org/linux/man-pages/man8/mount.8.html).

## Пути

| Назначение | Системный scope | Пользовательский scope |
| --- | --- | --- |
| Bot default | /etc/tgfs/bot.toml | $XDG_CONFIG_HOME/tgfs/bot.toml |
| Channel default | /etc/tgfs/channel.toml | $XDG_CONFIG_HOME/tgfs/channel.toml |
| Именованные профили | /etc/tgfs/profiles.d/name.toml | $XDG_CONFIG_HOME/tgfs/profiles.d/name.toml |
| Индекс, intent, dirty staging | /var/lib/tgfs/fs_uuid/ | $XDG_STATE_HOME/tgfs/fs_uuid/ |
| Чистый cache | /var/cache/tgfs/fs_uuid/ | $XDG_CACHE_HOME/tgfs/fs_uuid/ |

XDG fallback — стандартные ~/.config, ~/.local/state, ~/.cache. Default синтетически использует bot.toml + channel.toml; отдельный profiles.d/default.toml не обязателен. Команда tgfs mount работает без root через FUSE в пределах прав пользователя. Системный helper не читает произвольный профиль из пользовательского каталога автоматически.

## bot.toml

```toml
schema_version = 1
backend = "bot_api"

[bot]
token_file = "/etc/tgfs/secrets/default.token"
api_mode = "cloud" # cloud | local; возможности проверяются отдельно
api_url = "https://api.telegram.org"

[proxy]
enabled = false
# url = "socks5h://login:password@host:port"
```

Вопрос «использовать ли прокси?» задаёт мастер настройки, а daemon читает готовый конфиг без интерактивных запросов. Один URL вместо одновременно HTTP_PROXY/HTTPS_PROXY/SOCKS5_PROXY устраняет неоднозначный выбор. Для SOCKS семантику локального/удалённого DNS надо поддержать и проверить конкретной HTTP-библиотекой; `socks5h` означает требуемый remote DNS, а не обещание любой библиотеки. Другие поддержанные schemes описываются после выбора транспорта. Proxy credentials также секрет.

При local Bot API URL может указывать на localhost или отдельный сервис. Наличие URL не доказывает local-режим и большой лимит. Прокси HTTP-клиента TGFS регулирует маршрут до Bot API; доступ самого локального API server к Telegram настраивается у этого сервера отдельно. Если getFile возвращает локальный путь сервера, TGFS использует его лишь при явно настроенной общей файловой области; иначе нужен доступный механизм передачи байт.

Для TDLib потребуется отдельная схема с api_id, api_hash и приватной сессией; пользовательский вход и 2FA выполняются настройкой, не mount helper при загрузке ОС. Точный формат отложен до выбора backend.

## channel.toml

```toml
schema_version = 1

[channel]
id = -1001234567890
null_channel_id = -1001234567891 # необязателен для TDLib/если recovery не нужен
autoclean_null = true
# superblock_message_id = 42 # заполняется после init
# expected_fs_uuid = "..."    # защита от подключения другого тома

[filesystem]
read_only = true
chunk_size_bytes = 16777216 # 16 MiB, кандидат default
```

Числовые channel_id валидируются как Telegram IDs выбранного backend с фактической проверкой типа чата; проверка строкового префикса `-100` сама по себе недостаточна. API capabilities ограничивают chunk сверху; конфиг может его уменьшить. Режим read-only не запрещает ограниченные служебные пересылки recovery; требование абсолютно без записи в Telegram — отдельный режим.

## Профиль channel2.toml

```toml
schema_version = 1
bot_config = "/etc/tgfs/bots.d/bot2.toml"
channel_config = "/etc/tgfs/channels.d/channel2.toml"
```

Системные пути абсолютные; в пользовательском scope относительные пути разрешаются относительно файла профиля, не cwd. Явный CLI-профиль выбирает scope; скрытого смешивания user и system нет. Приоритет overrides и допустимые mount options нужно утвердить перед реализацией.

Токен хранить отдельно с правами 0600 и владельцем процесса; не передавать в argv, fstab, URLs логов или репозиторий. Каталоги конфигурации и session-файлы защищать правами. Профили не исполняют shell; имена профилей проверяются, обход путей запрещён. Дополнительные mount options выбираются из разрешённого списка. Root helper не должен превращать пользовательский путь в доступ к root-secret.
