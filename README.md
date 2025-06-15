# Руководство по тестированию **deduplar**

> Это краткое руководство описывает, как создать небольшую тестовую среду, проверить работу демона **deduplar**, а затем запустить его на «боевой» папке *Downloads*.

---

## Предварительные требования

- Скомпилированный бинарный файл **deduplar** (или установленный через `make install`).
- Права администратора (требуются для остановки демона, очистки кешей и работы с системными каталогами).
- macOS 10.15+ или совместимый Linux‑дистрибутив с поддержкой hard‑link’ов.

> **Совет:** Если вы ещё не добавили `deduplar` в `$PATH`, указывайте полный путь, например `/Users/roman/Downloads/deduplar/build/deduplar`.

---

## Быстрый старт на тестовой папке

```bash
# 1️⃣  Создать директорию для экспериментов
mkdir -p ~/Desktop/test-deduplicator
cd ~/Desktop/test-deduplicator

# 2️⃣  Подготовить дубликаты
# 2a. Три одинаковых текстовых файла
echo "duplicate test" > fileA.txt
cp fileA.txt fileB.txt
cp fileA.txt fileC.txt

# 2b. Три копии исходника .c (замените путь при необходимости)
cp /Users/Roman/Documents/deduplar/src/deduplar.c file1.c
cp file1.c file2.c
cp file1.c file3.c

# 3️⃣  Удалить остатки старых запусков
sudo pkill -f deduplar            # убить все запущенные демоны
rm -rf ~/Library/Caches/deduplar  # кеш текущего пользователя
sudo rm -rf /var/root/Library/Caches/deduplar  # кеш root (если был sudo)

# 4️⃣  Запустить deduplar для тестовой папки
deduplar ~/Desktop/test-deduplicator
# или, если нет в PATH:
# /Users/Roman/Downloads/deduplar/build/deduplar ~/Desktop/test-deduplicator

# 5️⃣  Подождать пару секунд и следить за логом
tail -n +1 -f ~/Library/Caches/deduplar/deduplar.log

# 6️⃣  Проверить число жёстких ссылок (inode) в каталоге
ls -li ~/Desktop/test-deduplicator

# 7️⃣  Корректно остановить демон
deduplar -kill
```

---

## Запуск на реальной папке *Downloads*

После того как тестовая папка отработала без ошибок, можно перейти к реальным данным.

```bash
# 1️⃣  Убедиться, что демон остановлен и кеши очищены
pkill -f deduplar || true
rm -rf ~/Library/Caches/deduplar
sudo rm -rf /var/root/Library/Caches/deduplar

# 2️⃣  Запустить deduplar на каталоге Downloads
deduplar /Users/Roman/Downloads

# 3️⃣  Мониторинг лога (будут появляться записи для Downloads)
tail -f ~/Library/Caches/deduplar/deduplar.log

# 4️⃣  Пример поиска файлов с >1 жёсткой ссылкой
find /Users/Roman/Downloads -type f -links +1 -exec ls -li {} +

# 5️⃣  Остановить демон, когда процесс завершён
deduplar -kill
```

---

## Что проверять в логах

- **INFO** о старте демона и целевой директории.
- **LINKED** — строки, где показано объединение дубликатов в hard‑link’и.
- **STATS** — итоговая статистика (сколько файлов обработано, сколько сэкономлено).
- **WARN/ERROR** — отсутствие прав доступа, проблемные файлы и т. п.
