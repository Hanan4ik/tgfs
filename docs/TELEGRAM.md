# Telegram: факты и непроверенные предположения

Проверка официальных источников: 2026-11-02. Это документальная проверка; поведение TGFS на живом канале не тестировалось.

## Облачный Bot API

[FAQ по файлам](https://core.telegram.org/bots/faq#handling-media) указывает отправку файлов до 50 MB и скачивание через getFile до 20 MB. Следствие для TGFS: выбирать chunk по минимуму возможностей чтения и записи. Предлагаемые 16 MiB = 16 777 216 байт ниже обоих пределов. Не переносить неоднозначное обозначение MB в точный byte-limit без проверки.

## Собственный Bot API server

[Официальный сервер](https://github.com/tdlib/telegram-bot-api#usage) в local-режиме поддерживает upload до 2000 MB и скачивание без указанного ограничения размера. Это не метод чтения истории и не собственное хранилище вместо Telegram. Сервер нужно отдельно запускать, обслуживать и правильно переводить на него бота; произвольный api_url не означает эти возможности. Для эксплуатации локального сервера дополнительно нужны api_id и api_hash приложения. TGFS может сохранять небольшой размер частей и в таком режиме ради задержки и random read.

## Важные детали Bot API

Источник: [справочник Bot API](https://core.telegram.org/bots/api), разделы getUpdates, getChat/ChatFullInfo, forwardMessage, copyMessage, getFile, sendDocument, deleteMessage и Sending files.

- file_id позволяет скачать документ напрямую, привязан к боту; file_unique_id для скачивания не подходит.
- forwardMessage возвращает Message; copyMessage — MessageId. Protected content пересылке не подлежит.
- ChatFullInfo.pinned_message — самое новое по дате отправки закреплённое сообщение, не список всех закреплений.
- Updates хранятся не дольше 24 часов и не являются журналом хранения TGFS.
- deleteMessage имеет ограничение возраста сообщения 48 часов, в дополнение к требованиям прав. Значит старые данные нельзя обещать автоматически очищать этим методом.
- Caption document ограничен 1024 символами; большой каталог нужно хранить документом.

## TDLib и авторизация

TDLib предоставляет [getMessage](https://core.telegram.org/tdlib/docs/classtd_1_1td__api_1_1get_message.html), [getChatHistory](https://core.telegram.org/tdlib/docs/classtd_1_1td__api_1_1get_chat_history.html) и [downloadFile](https://core.telegram.org/tdlib/docs/classtd_1_1td__api_1_1download_file.html), в том числе параметры offset/limit загрузки.

Но метод MTProto [messages.getHistory](https://core.telegram.org/method/messages.getHistory) доступен пользовательским аккаунтам. Замена HTTP-клиента на TDLib при прежнем bot token не гарантирует чтения всей истории. Для альтернативы с user session нужны api_id/api_hash, вход пользователя, сессионное состояние и проверка прав доступа к каналу.

Из этих фактов следует рекомендация, а не доказанная реализация: Bot API годится как кандидат для контролируемого TGFS-формата с явным каталогом; пользовательский TDLib backend удобнее исследовать для восстановления по истории и импорта обычных каналов.

## Эксперименты до принятия архитектуры

[Задача #6](https://github.com/Hanan4ik/tgfs/issues/6) проверяет bootstrap/pin, получение известного сообщения через пересылку, восстановление file_id другим ботом, права и обработку timeout. Для TDLib отдельно проверять ограничения авторизации, идентификаторы сообщений и файлы. Не приписывать TDLib bot-сессии возможности user-сессии.
