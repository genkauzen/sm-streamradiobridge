# Публикация `genkauzen/sm-streamradiobridge`

В этой папке уже подготовлены `manifest.json`, единый `StreamRadioLauncher` и
workflow для релизов. Сейчас push не выполняется автоматически: на компьютере
нет GitHub-токена/SSH-ключа, а удалённый репозиторий должен быть создан или
должен быть доступен владельцу `genkauzen`.

После входа в GitHub выполните в PowerShell из корня проекта:

```powershell
git init
git add .
git commit -m "Stream Radio Core 1.0.5 build 4"
git branch -M main
git remote add origin https://github.com/genkauzen/sm-streamradiobridge.git
git push -u origin main
```

Для автообновления лаунчер ожидает GitHub Release с ZIP-файлом. Создайте тег
`v1.0.5-build4` — workflow `.github/workflows/release.yml` соберёт
`StreamRadio_1.0.5.zip` из `dist` и опубликует его. После публикации ссылка из
`manifest.json` начинает работать автоматически.

Если репозиторий приватный, публичный лаунчер не сможет получить release без
токена; в таком случае оставьте `AutoUpdate` выключенным или добавьте авторизованный
endpoint в `StreamRadioLauncher.ps1`.
