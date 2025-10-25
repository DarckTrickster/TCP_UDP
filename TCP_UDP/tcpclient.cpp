#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <clocale>
#include <cerrno>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include <sys/time.h>  // Добавляем для struct timeval

#define BUFFER_LENGTH 8192

// Структура сообщения
struct Message {
    uint8_t d;
    uint8_t m;
    uint16_t y;
    uint8_t h1;
    uint8_t m1;
    uint8_t s1;
    uint8_t h2;
    uint8_t m2;
    uint8_t s2;
    char message[BUFFER_LENGTH];
};

// Прототипы
int connect_with_retry(int* sock_out, const struct sockaddr_in* serverAddress);
int readMessageLine(const char* line, Message* msg);
int formatMessage(const Message* msg, uint32_t msgId, char* outBuf);
int sendAll(int sockfd, const void* data, int length);
int recvOk(int sockfd);

int main(int argc, char* argv[])
{
    
    setlocale(LC_ALL, "ru");

    if (argc != 3)
    {
        fprintf(stderr, "Формат: %s IP:port file.txt\n", argv[0]);
        return EXIT_FAILURE;
    }

    // Разбор аргумента адрес:порт
    char* colon = strchr(argv[1], ':');
    if (!colon)
    {
        fprintf(stderr, "Некорректный формат адреса (IP:port)\n");
        return EXIT_FAILURE;
    }

    char ip[64];
    size_t iplen = (size_t)(colon - argv[1]);
    if (iplen >= sizeof(ip))
    {
        fprintf(stderr, "IP адрес слишком длинный\n");
        return EXIT_FAILURE;
    }

    strncpy(ip, argv[1], iplen);//Парсит IP и порт, получает имя файла
    ip[iplen] = '\0';
    int port = atoi(colon + 1);
    const char* filename = argv[2];

    // Заполнение sockaddr_in
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &serverAddr.sin_addr) <= 0)
    {
        fprintf(stderr, "Некорректный ip адрес\n");
        return EXIT_FAILURE;
    }

    printf("Подключение к серверу: %s:%d\n", ip, port);
    int sock;
    if (!connect_with_retry(&sock, &serverAddr))
        return EXIT_FAILURE;

    printf("Подключен.\n");

    // Отправляем 'put'
    if (!sendAll(sock, "put", 3)) {
        fprintf(stderr, "Ошибка отправки стартового сообщения 'put'\n");
        close(sock);
        return EXIT_FAILURE;
    }

    // Открытие входного файла
    FILE* f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "Ошибка при открытии файла '%s': %s\n", filename, strerror(errno));
        close(sock);
        return EXIT_FAILURE;
    }

    char line[BUFFER_LENGTH]; //Читает файл построчно
    uint32_t msgCount = 0;
    // Чтение и отправки строк
    while (fgets(line, sizeof(line), f))
    {
        // Удаление \r\n
        line[strcspn(line, "\r\n")] = '\0';

        // Игнорирование пустых строк
        if (line[0] == '\0')
            continue;

        Message msg;
        if (!readMessageLine(line, &msg))
        {
            // Если строка не соответствует формату — пропуск строки
            fprintf(stderr, "Пропущена некорректная строка: \"%s\"\n", line);
            continue;
        }

        // Формирование пакета
        char outBuf[BUFFER_LENGTH]; //[ID][дата][время1][время2][длина][сообщение].
        int outLen = formatMessage(&msg, msgCount, outBuf);
        if (outLen <= 0)
        {
            fprintf(stderr, "Ошибка при форматировании сообщения %u\n", msgCount);
            fclose(f);
            close(sock);
            return EXIT_FAILURE;
        }

        if (!sendAll(sock, outBuf, outLen))
        {
            fprintf(stderr, "Не удалось отправить сообщение %u\n", msgCount);
            fclose(f);
            close(sock);
            return EXIT_FAILURE;
        }

        msgCount++;
    }

    fclose(f);

    // Ожидание корректного количества "ok" от сервера
    for (uint32_t i = 0; i < msgCount; ++i)
    {
        if (!recvOk(sock))
        {
            fprintf(stderr, "Не получен ответ 'ok' для сообщения %u\n", i);
            close(sock);
            return EXIT_FAILURE;
        }
    }

    printf("Отправлено сообщений: %u.\n", msgCount);

    close(sock);
    return EXIT_SUCCESS;
}

// Повторные попытки подключения (10 попыток, 100 ms между)
int connect_with_retry(int* sock_out, const struct sockaddr_in* serverAddress)
{
    for (int attempt = 1; attempt <= 10; ++attempt) {
        int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s < 0) {
            fprintf(stderr, "Ошибка создания сокета: %s\n", strerror(errno));
            return 0;
        }

        // Установка таймаута 100ms для connect()
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;  // 100ms 
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        printf("Попытка подключения %d/10 к %s:%d...\n",
            attempt,
            inet_ntoa(serverAddress->sin_addr),
            ntohs(serverAddress->sin_port));

        if (connect(s, (const struct sockaddr*)serverAddress, sizeof(*serverAddress)) == 0) {
            printf("Успешное подключение!\n");

            // Сбрасываем таймаут обратно в блокирующий режим для нормальной работы
            struct timeval notv;
            notv.tv_sec = 0;
            notv.tv_usec = 0;
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &notv, sizeof(notv));

            *sock_out = s;
            return 1;
        }

        // Не удалось подключиться
        fprintf(stderr, "Попытка %d не удалась: %s\n", attempt, strerror(errno));
        close(s);

        // Пауза между попытками (кроме последней)
        if (attempt < 10) {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 100 * 1000 * 1000; // 100 ms
            nanosleep(&ts, NULL);
        }
    }

    fprintf(stderr, "Ошибка подключения к серверу (10 попыток)\n");
    return 0;
}

// Чтение строки в формате dd.mm.yyyy hh:mm:ss hh:mm:ss Message
int readMessageLine(const char* line, Message* msg)
{
    // Инициализация message пустой строкой
    msg->message[0] = '\0';
    int d, mo, y;
    int h1, m1, s1;
    int h2, m2, s2;
    char msgText[BUFFER_LENGTH];
    int parsed = sscanf(line, "%d.%d.%d %d:%d:%d %d:%d:%d %[^\n]", //Парсит строку формата дд.мм.гггг чч:мм:сс чч:мм:сс сообщение
        &d, &mo, &y, &h1, &m1, &s1, &h2, &m2, &s2, msgText);
    if (parsed != 10)
        return 0;

    // Валидация полей
    if (d <= 0 || d > 31)
        return 0;
    if (mo <= 0 || mo > 12)
        return 0;
    if (y < 0 || y > 9999)
        return 0;
    if (h1 < 0 || h1 > 23)
        return 0;
    if (m1 < 0 || m1 > 59)
        return 0;
    if (s1 < 0 || s1 > 59)
        return 0;
    if (h2 < 0 || h2 > 23)
        return 0;
    if (m2 < 0 || m2 > 59)
        return 0;
    if (s2 < 0 || s2 > 59)
        return 0;

    msg->d = (uint16_t)d;
    msg->m = (uint16_t)mo;
    msg->y = (uint16_t)y;
    msg->h1 = (uint8_t)h1;
    msg->m1 = (uint8_t)m1;
    msg->s1 = (uint8_t)s1;
    msg->h2 = (uint8_t)h2;
    msg->m2 = (uint8_t)m2;
    msg->s2 = (uint8_t)s2;

    strncpy(msg->message, msgText, BUFFER_LENGTH - 1);
    msg->message[BUFFER_LENGTH - 1] = '\0';
    return 1;
}

// Форматирование сообщения в сетевой порядок
int formatMessage(const Message* msg, uint32_t msgId, char* outBuf)
{
    uint32_t offset = 0;
    uint32_t netIdx = htonl(msgId);
    memcpy(outBuf + offset, &netIdx, 4); offset += 4;
    uint32_t dateVal = (uint32_t)msg->y * 10000u + (uint32_t)msg->m * 100u + (uint32_t)msg->d;
    uint32_t netDate = htonl(dateVal);
    memcpy(outBuf + offset, &netDate, 4); offset += 4;
    uint32_t t1 = (uint32_t)msg->h1 * 10000u + (uint32_t)msg->m1 * 100u + (uint32_t)msg->s1;
    uint32_t netT1 = htonl(t1);
    memcpy(outBuf + offset, &netT1, 4); offset += 4;
    uint32_t t2 = (uint32_t)msg->h2 * 10000u + (uint32_t)msg->m2 * 100u + (uint32_t)msg->s2;
    uint32_t netT2 = htonl(t2);
    memcpy(outBuf + offset, &netT2, 4); offset += 4;
    size_t msgLen = strlen(msg->message);
    if (msgLen > (size_t)(BUFFER_LENGTH - offset - 4))
        msgLen = BUFFER_LENGTH - offset - 4;

    uint32_t netLen = htonl((uint32_t)msgLen);
    memcpy(outBuf + offset, &netLen, 4); offset += 4;

    if (msgLen > 0)
    {
        memcpy(outBuf + offset, msg->message, msgLen);
        offset += (uint32_t)msgLen;
    }

    return (int)offset;
}

// Отправка всех байт
int sendAll(int sockfd, const void* data, int length)
{
    const char* p = (const char*)data;
    int remaining = length;
    while (remaining > 0) {
        ssize_t sent = send(sockfd, p, (size_t)remaining, 0);
        if (sent <= 0) {
            fprintf(stderr, "Ошибка отправки: %s\n", strerror(errno));
            return 0;
        }
        p += sent;
        remaining -= (int)sent;
    }
    return 1;
}

// Получение "ok"
int recvOk(int sockfd)
{
    char buf[2];
    ssize_t r = recv(sockfd, buf, 2, MSG_WAITALL);
    if (r < 2) {
        if (r < 0) fprintf(stderr, "Ошибка при чтении: %s\n", strerror(errno));
        else fprintf(stderr, "Соединение закрыто сервером\n");
        return 0;
    }
    return (buf[0] == 'o' && buf[1] == 'k') ? 1 : 0;
}