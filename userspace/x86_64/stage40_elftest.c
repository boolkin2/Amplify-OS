// ============================================================================
// elftest.c — проверка самоинтроспекции ELF через open+read (этап 40).
// ----------------------------------------------------------------------------
// Мигрирован из userspace/elftest/main.c. Старая версия рисовала GUI-окно
// через myos_* API; для POSIX-совместимого теста смотрит свой собственный
// ELF-заголовок из /hello.elf (известный файл в initrd).
// Маркер для автотеста: "STAGE40_ELFTEST_OK".
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

int main(void) {
    printf("[elftest] stat(/hello.elf)\n");
    struct stat st;
    if (stat("/hello.elf", &st) < 0) {
        printf("[elftest] stat failed\n");
        return 1;
    }
    printf("[elftest] size=%ld mode=0%o\n", (long)st.st_size, (unsigned)st.st_mode);

    int fd = open("/hello.elf", O_RDONLY);
    if (fd < 0) {
        printf("[elftest] open failed\n");
        return 1;
    }
    unsigned char hdr[4];
    if (read(fd, hdr, 4) != 4) {
        printf("[elftest] read failed\n");
        close(fd);
        return 1;
    }
    printf("[elftest] ELF magic: %02X %02X %02X %02X\n",
           hdr[0], hdr[1], hdr[2], hdr[3]);
    close(fd);

    if (hdr[0] == 0x7F && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F') {
        printf("STAGE40_ELFTEST_OK\n");
        return 0;
    }
    printf("[elftest] not an ELF file\n");
    return 1;
}
