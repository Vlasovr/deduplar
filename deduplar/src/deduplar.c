#define _POSIX_C_SOURCE 200809L  // чтобы гарантированно получить PATH_MAX

#include <stdio.h>               // FILE*, fopen, fprintf, snprintf
#include <stdlib.h>              // EXIT_SUCCESS, EXIT_FAILURE, malloc, free, getenv, exit
#include <string.h>              // strcmp, strlen, snprintf, strdup, memcmp, strerror
#include <dirent.h>              // DIR, opendir, readdir, closedir
#include <sys/stat.h>            // stat, S_ISREG, S_ISDIR
#include <sys/types.h>           // off_t, pid_t
#include <openssl/evp.h>         // EVP_MD_CTX, EVP_md5, EVP_DigestInit_ex и др.
#include <unistd.h>              // fork, unlink, getpid, sleep, chdir, close, write
#include <time.h>                // time, localtime, strftime
#include <fcntl.h>               // open, O_CREAT, O_EXCL, O_WRONLY
#include <syslog.h>              // syslog, LOG_ERR
#include <signal.h>              // signal, SIGTERM, SIGINT, SIGSEGV, SIGABRT
#include <errno.h>               // errno

#include <limits.h>              // PATH_MAX

/* =================================================================
   Общие константы
   ================================================================= */
#define APP_NAME        "deduplar"                           // имя утилиты
#define APP_CACHE_FMT   "%s/Library/Caches/" APP_NAME        // формат папки кэша
#define HASH_TEXT_LEN   33                                   // 32 байта hex-MD5 + '\0'
#define MSG_BUF_LEN     700                                  // размер буфера для сообщений
#define SCAN_INTERVAL   20                                   // пауза между сканированиями (сек)

/* -----------------------------------------------------------------
   Глобальные переменные (заполняются в main)
   ----------------------------------------------------------------- */
static char g_cache_dir[PATH_MAX];   // каталог для логов, lock-файла и bin-файла
static char g_log_path [PATH_MAX];   // путь к текстовому логу
static char g_lock_path[PATH_MAX];   // путь к lock-файлу
static char g_bin_path [PATH_MAX];   // путь к бинарному журналу
static int  g_lock_fd   = -1;        // дескриптор lock-файла (открыт до завершения)

/* -----------------------------------------------------------------
   Структура, описывающая файл при обходе
   ----------------------------------------------------------------- */
typedef struct {
    off_t path_len;                  // длина пути (включая '\0')
    char *path;                      // NUL-terminated путь к файлу
    off_t size;                      // размер файла, байт
    char  md5[HASH_TEXT_LEN];        // hex-строка MD5
} file_info_t;

/* -----------------------------------------------------------------
   Вспомогательная функция: собрать путь по формату и HOME
   ----------------------------------------------------------------- */
static inline void make_path(char *dst, size_t dstsz,
                             const char *fmt, const char *home)
{
    // dst := formatted(fmt, home), например "/Users/you/Library/Caches/deduplar"
    snprintf(dst, dstsz, fmt, home);
}

/* -----------------------------------------------------------------
   Прототипы всех функций
   ----------------------------------------------------------------- */
static void log_msg(const char *lvl, const char *msg);
static int same_inode(const char *a, const char *b);
static void write_bin(const file_info_t *fi);
static char *lookup_duplicate(const file_info_t *needle);
static void compute_md5(const char *path, char out[HASH_TEXT_LEN]);
static void traverse(const char *dir);
static void reset_bin(void);
static void cleanup(void);
static void on_signal(int sig);
static void daemonize(void);
static int ensure_singleton(const char *lock_path);
static int create_lock_file(const char *lock_path);
static void kill_running(const char *lock_path);

/* =================================================================
   Точка входа
   ================================================================= */
int main(int argc, char *argv[])
{
    // ----------------------------------------------------------------
    // 1. Получаем HOME для формирования путей
    // ----------------------------------------------------------------
    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "%s: HOME не задан\n", APP_NAME);
        return EXIT_FAILURE;
    }

    // ----------------------------------------------------------------
    // 2. Формируем пути к cache-каталогу и трём необходимые строки
    // ----------------------------------------------------------------
    make_path(g_cache_dir, sizeof(g_cache_dir), APP_CACHE_FMT, home);
    make_path(g_log_path,   sizeof(g_log_path),
              APP_CACHE_FMT "/" APP_NAME ".log",  home);
    make_path(g_lock_path,  sizeof(g_lock_path),
              APP_CACHE_FMT "/" APP_NAME ".lock", home);
    make_path(g_bin_path,   sizeof(g_bin_path),
              APP_CACHE_FMT "/" APP_NAME ".bin",  home);

    // ----------------------------------------------------------------
    // 3. Обработка опции "-kill": если указано, шлём SIGTERM запущенному демону
    // ----------------------------------------------------------------
    if (argc > 1 && strcmp(argv[1], "-kill") == 0) {
        kill_running(g_lock_path);
        return EXIT_SUCCESS;
    }

    // ----------------------------------------------------------------
    // 4. Убедимся, что каталог кэша существует
    // ----------------------------------------------------------------
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", g_cache_dir);
    system(cmd);

    // ----------------------------------------------------------------
    // 5. Проверяем, не запущен ли уже другой экземпляр (через lock-файл)
    // ----------------------------------------------------------------
    if (ensure_singleton(g_lock_path) != 0)
        return EXIT_FAILURE;

    // ----------------------------------------------------------------
    // 6. Устанавливаем обработчики сигналов для корректной остановки
    // ----------------------------------------------------------------
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGSEGV, on_signal);
    signal(SIGABRT, on_signal);
 
    daemonize();

    // ----------------------------------------------------------------
    // 7. Создаём lock-файл (записываем в него PID) и сохраняем FD открытым
    // ----------------------------------------------------------------
    if ((g_lock_fd = create_lock_file(g_lock_path)) < 0) {
        cleanup();
        return EXIT_FAILURE;
    }

    // ----------------------------------------------------------------
    // 8. Выбираем целевую директорию для сканирования (аргумент или HOME)
    // ----------------------------------------------------------------
    const char *target = (argc > 1 && argv[1][0] != '-') ? argv[1] : home;

    // ----------------------------------------------------------------
    // 9. Бесконечный цикл: логируем, обнуляем журнал, запускаем обход, спим
    // ----------------------------------------------------------------
    while (1) {
        char buf[MSG_BUF_LEN];

        snprintf(buf, sizeof(buf), "Начало обхода %s", target);
        log_msg("INFO", buf);

        reset_bin();           // очищаем бинарный журнал
        traverse(target);      // рекурсивно обходим и заменяем дубликаты

        snprintf(buf, sizeof(buf),
                 "Завершено, повтор через %d сек", SCAN_INTERVAL);
        log_msg("INFO", buf);

        sleep(SCAN_INTERVAL);
    }

    return EXIT_SUCCESS;
}

/* =================================================================
   Реализация функций
   ================================================================= */

/* -----------------------------------------------------------------
   log_msg: записывает строку в лог-файл с пометкой времени и уровня
   ----------------------------------------------------------------- */
static void log_msg(const char *lvl, const char *msg)
{
    // Открываем лог на дозапись
    FILE *f = fopen(g_log_path, "a");
    if (!f) {
        // При ошибке пишем в syslog и выходим
        syslog(LOG_ERR, "Ошибка открытия %s: %s", g_log_path, strerror(errno));
        cleanup();
        exit(EXIT_FAILURE);
    }
    // Текущее время
    time_t t = time(NULL);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));

    // Запись: [YYYY-MM-DD HH:MM:SS] [INFO|ERROR] Сообщение
    fprintf(f, "[%s] [%s] %s\n", ts, lvl, msg);
    fclose(f);
}

/* -----------------------------------------------------------------
   same_inode: проверяет, указывают ли два пути на один и тот же файл
   возвращает 1, если inode и устройство совпадают, иначе 0
   ----------------------------------------------------------------- */
static int same_inode(const char *a, const char *b)
{
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0)
        return 0;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

/* -----------------------------------------------------------------
   write_bin: добавляет запись о файле в бинарный журнал
   формат записи: path_len + path + size + md5
   ----------------------------------------------------------------- */
static void write_bin(const file_info_t *fi)
{
    FILE *f = fopen(g_bin_path, "ab");
    if (!f) {
        log_msg("ERROR", "Не удалось открыть бинарный журнал");
        cleanup();
        exit(EXIT_FAILURE);
    }
    fwrite(&fi->path_len, sizeof(fi->path_len), 1, f);
    fwrite( fi->path,        1, fi->path_len, f);
    fwrite(&fi->size,     sizeof(fi->size), 1, f);
    fwrite( fi->md5,        1, HASH_TEXT_LEN,  f);
    fclose(f);
}

/* -----------------------------------------------------------------
   lookup_duplicate: ищет в бинарном журнале первый файл с тем же
   размером и MD5, что needle.
   Возвращает path найденного дубликата (malloc’ит!), или NULL
   ----------------------------------------------------------------- */
static char *lookup_duplicate(const file_info_t *needle)
{
    FILE *f = fopen(g_bin_path, "rb");
    if (!f) return NULL;  // журнал пока не создан

    while (1) {
        file_info_t fi;
        if (!fread(&fi.path_len, sizeof(fi.path_len), 1, f))
            break;  // EOF

        // читаем путь
        fi.path = malloc(fi.path_len);
        fread(fi.path, 1, fi.path_len, f);

        // читаем размер и md5
        fread(&fi.size,      sizeof(fi.size), 1, f);
        fread( fi.md5,       1, HASH_TEXT_LEN, f);

        // сравниваем
        if (fi.size == needle->size
         && memcmp(fi.md5, needle->md5, HASH_TEXT_LEN) == 0)
        {
            fclose(f);
            return fi.path;  // нашли дубликат
        }
        free(fi.path);
    }

    fclose(f);
    return NULL;  // не нашли
}

/* -----------------------------------------------------------------
   compute_md5: вычисляет MD5-фингерпринт файла в виде hex-строки
   ----------------------------------------------------------------- */
static void compute_md5(const char *path, char out[HASH_TEXT_LEN])
{
    // буфер для байтов хеша и длина
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int len = 0;

    // создаём контекст OpenSSL
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    FILE *f = fopen(path, "rb");
    if (!ctx || !f) {
        log_msg("ERROR", "Не удалось открыть файл для MD5");
        if (ctx) EVP_MD_CTX_free(ctx);
        if (f) fclose(f);
        return;
    }

    // инициализация MD5
    EVP_DigestInit_ex(ctx, EVP_md5(), NULL);

    // читаем по кускам и обновляем контекст
    char buf[4096];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
        EVP_DigestUpdate(ctx, buf, r);
    }

    // финализируем и получаем байты
    EVP_DigestFinal_ex(ctx, hash, &len);

    // очищаем
    EVP_MD_CTX_free(ctx);
    fclose(f);

    // конвертируем байты в hex-строку
    for (unsigned i = 0; i < len; ++i) {
        sprintf(&out[i*2], "%02x", hash[i]);
    }
    out[HASH_TEXT_LEN - 1] = '\0';
}

/* -----------------------------------------------------------------
   traverse: рекурсивно обходит dir, для каждого файла:
     - вычисляет MD5 и ищет дубликат
     - если дубликат найден и не тот же inode:
         удаляет файл и создаёт жесткую ссылку на оригинал
     - иначе записывает в журнал
   ----------------------------------------------------------------- */
static void traverse(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        log_msg("ERROR", "Не удалось открыть каталог");
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        // пропускаем . и ..
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;

        // формируем полный путь
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            // рекурсивно в поддиректорию
            traverse(path);
        }
        else if (S_ISREG(st.st_mode)) {
            // обрабатываем файл
            file_info_t fi;
            fi.size     = st.st_size;
            fi.path_len = strlen(path) + 1;
            fi.path     = strdup(path);

            compute_md5(path, fi.md5);

            char *dup = lookup_duplicate(&fi);
            if (!dup) {
                // новый уникальный файл — запомним
                write_bin(&fi);
            } else if (!same_inode(dup, fi.path)) {
                // найден дубликат на другом inode’е
                unlink(fi.path);
                if (link(dup, fi.path) == 0) {
                    char msg[MSG_BUF_LEN];
                    snprintf(msg, sizeof(msg),
                             "Заменено %s -> жесткая ссылка на %s",
                             fi.path, dup);
                    log_msg("INFO", msg);
                } else {
                    log_msg("ERROR", "Не удалось создать hardlink");
                }
            }
            free(fi.path);
            if (dup) free(dup);
        }
    }
    closedir(d);
}

/* -----------------------------------------------------------------
   reset_bin: сбрасывает (обнуляет) бинарный журнал для нового прохода
   ----------------------------------------------------------------- */
static void reset_bin(void)
{
    FILE *f = fopen(g_bin_path, "wb");
    if (f) fclose(f);
}

/* -----------------------------------------------------------------
   cleanup: закрывает lock-файл, удаляет lock и bin-файлы
   ----------------------------------------------------------------- */
static void cleanup(void)
{
    if (g_lock_fd >= 0) close(g_lock_fd);
    unlink(g_lock_path);
    unlink(g_bin_path);
}

/* -----------------------------------------------------------------
   on_signal: обработчик завершения по сигналам SIGTERM, SIGINT и др.
   сохраняет лог, чистит и выходит
   ----------------------------------------------------------------- */
static void on_signal(int sig)
{
    switch (sig) {
        case SIGINT:  log_msg("INFO","SIGINT получен"); break;
        case SIGTERM: log_msg("INFO","SIGTERM получен"); break;
        case SIGSEGV: log_msg("ERROR","SIGSEGV — сегфолт"); break;
        case SIGABRT: log_msg("ERROR","SIGABRT — аварийный сброс"); break;
        default: break;
    }
    cleanup();
    exit((sig==SIGINT||sig==SIGTERM) ? EXIT_SUCCESS : EXIT_FAILURE);
}

/* -----------------------------------------------------------------
   daemonize: делает двойной fork, setsid, перенаправляет stdin/out/err
   ..................................................................
   1) Первый fork + exit родителя
   2) setsid() — новый сеанс, управляющий терминал отрывается
   3) Игнорируем SIGHUP
   4) Второй fork + exit первого потомка
   5) chdir("/") и umask(0)
   6) Перенаправление STDIN/STDOUT/STDERR в /dev/null
   ----------------------------------------------------------------- */
static void daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    if (setsid() < 0) exit(EXIT_FAILURE);
    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    chdir("/");
    umask(0);

    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2) close(fd);
    }
}

/* -----------------------------------------------------------------
   ensure_singleton: пробует создать уникальный lock-файл.
   если файл уже существует — сообщает, что экземпляр запущен
   ----------------------------------------------------------------- */
static int ensure_singleton(const char *lock_path)
{
    int fd = open(lock_path, O_CREAT|O_EXCL|O_WRONLY, 0644);
    if (fd < 0) {
        if (errno == EEXIST)
            fprintf(stderr, APP_NAME ": уже запущен\n");
        else
            perror(APP_NAME ": lock error");
        return -1;
    }
    close(fd);
    unlink(lock_path);
    return 0;
}

/* -----------------------------------------------------------------
   create_lock_file: создаёт lock-файл, записывает в него PID,
   возвращает открытый дескриптор для удержания блокировки
   ----------------------------------------------------------------- */
static int create_lock_file(const char *lock_path)
{
    int fd = open(lock_path, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) {
        perror("lock create");
        return -1;
    }
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
    write(fd, buf, n);
    return fd;
}

/* -----------------------------------------------------------------
   kill_running: читает PID из lock-файла и шлёт SIGTERM
   ----------------------------------------------------------------- */
static void kill_running(const char *lock_path)
{
    FILE *f = fopen(lock_path, "r");
    if (!f) {
        fprintf(stderr, APP_NAME ": процесс не найден\n");
        return;
    }
    int pid;
    if (fscanf(f, "%d", &pid) == 1) {
        if (kill(pid, SIGTERM) == 0)
            fprintf(stderr, APP_NAME ": отправлен SIGTERM %d\n", pid);
        else
            perror(APP_NAME ": kill error");
    }
    fclose(f);
}
