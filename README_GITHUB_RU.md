# Публикация `genkauzen/sm-streamradiobridge`

В этой папке подготовлены `manifest.json`, единый `StreamRadioLauncher` и
workflow для релизов. Репозиторий опубликован по адресу:

`https://github.com/genkauzen/sm-streamradiobridge`

После входа в GitHub выполните в PowerShell из корня проекта:

```powershell
git init
git add .
git commit -m "Stream Radio Core 1.0.5 build 4"
git branch -M main
git remote add origin https://github.com/genkauzen/sm-streamradiobridge.git
git push -u origin main
```

Для автообновления лаунчер использует GitHub Release с ZIP-файлом. Тег
`v1.0.5-build4` уже опубликован, а workflow `.github/workflows/release.yml`
собрал `StreamRadio_1.0.5.zip` из `dist`. Ссылка из `manifest.json` работает
автоматически.

Если репозиторий приватный, публичный лаунчер не сможет получить release без
токена; в таком случае оставьте `AutoUpdate` выключенным или добавьте авторизованный
endpoint в `StreamRadioLauncher.ps1`.
