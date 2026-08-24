# Native StreamRadioBridge 1.0.5

## Назначение

В полном дистрибутиве этот мост уже собран. Он решает три задачи, которых нет
в Lua sandbox Scrap Mechanic:

1. нормализовать URL YouTube/TikTok/VK;
2. получить audio-only дорожку через разрешённый resolver и локальный кэш;
3. создать локальный FMOD 3D sound, прикреплённый к `interactable`, без создания video surface.

Текущая DLL внедряет таблицу `StreamRadioBridge` в защищённое Lua-окружение
Scrap Mechanic 1.0.5 и использует FMOD игры для отдельного 3D-источника.
Старые offsets/хуки SM-CustomAudioExtension 0.7.4 не используются.

## Lua ABI

Мод ищет глобальную таблицу `StreamRadioBridge`:

```lua
StreamRadioBridge.update(bridge, {
    url = "https://...",
    playing = true,
    position = 12.5, -- seconds, authoritative server position
    volume = 0.75,   -- local player only
    loop = false,
    interactable = self.interactable
})

StreamRadioBridge.stop(bridge, interactable)

-- необязательно: вернуть локальный путь к уже загруженной thumbnail-картинке
StreamRadioBridge.getPreviewImage(bridge, url)
```

`update` может дополнительно вернуть таблицу вида
`{ duration = 183.4, status = "ready" }`. Длительность нужна GUI для
динамической дорожки перемотки. Если `duration` не возвращается, GUI показывает
только текущую позицию и `LIVE`, а перемотка отключена.

`update` вызывается только на клиенте. Мост обязан:

- кэшировать resolved audio по URL и не отправлять аудио-байты через `network`;
- привязать sound instance к world position/velocity `interactable`;
- использовать `position` для seek/drift correction и не запускать второй экземпляр при каждом update;
- применять `volume` только к текущему клиентскому экземпляру;
- возвращать длительность аудио в секундах после разрешения потока, если нужна перемотка;
- сообщать состояние `loading`, `ready` или `error` через отдельный необязательный callback, если он будет добавлен;
- полностью игнорировать video stream/rendering.

`getPreviewImage` — необязательный метод. Он не должен возвращать удалённый
HTTP-URL: Lua GUI принимает локальный путь, доступный игре. Если метода нет или
картинка ещё не готова, GUI оставляет иконку радио и текст «АУДИО-РЕЖИМ».

## Реализованный pipeline

```text
URL -> allow-list/domain validation -> yt-dlp audio-only download
    -> ffmpeg Ogg conversion -> FMOD 3D sound at boombox position
    -> drift correction from server position -> local volume
```

Resolver должен корректно обрабатывать истечение подписанных URL, удаление видео, региональные ошибки, сетевой timeout и ограничение размера кэша. Серверу нельзя доверять произвольные локальные пути или shell-команды; сервер синхронизирует только URL и транспортное состояние.

## Нельзя делать

- подключать старый CAE DLL 0.7.4 к 1.0.5 без пересборки и проверки;
- рендерить YouTube/TikTok/VK WebView/видео;
- принимать от сервера локальный путь к DLL/EXE или выполнять его из Lua;
- передавать через `sm.network` PCM/MP3 поток.

## Минимальные native tests

- два клиента слышат один и тот же URL с разницей позиции не более выбранного drift threshold;
- клиент A меняет громкость и не меняет громкость клиента B;
- перемещение машины переносит 3D-источник вместе с boombox;
- stop/destroy/перезагрузка мира освобождают FMOD instance и HTTP reader;
- GUI никогда не создаёт video texture;
- Creative, vanilla Survival и Fant 3 Survival не требуют замены игровых файлов.
