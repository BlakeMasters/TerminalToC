#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <utime.h>
#include <errno.h>
#include <arpa/inet.h>

#define USTAR_MAGIC "ustar"
#define USTAR_MAGIC_LEN 6
#define USTAR_VERSION "00"
#define BLOCK_SIZE 512

struct __attribute__((packed)) ustar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[USTAR_MAGIC_LEN]; /* Adjusted size to include null terminator */
    char version[2];              /* Adjusted size to include null terminator */
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12]; /* Adjusted to ensure the struct size is exactly 512 bytes */
};

/* Function declarations */
void fillHeader(struct ustar_header *header, const char *filePath, struct stat *fileStat, char typeflag);
void createArchive(const char *tarFile, int argc, char *argv[], int verbose, int strict);
void listContents(const char *tarFile, int verbose, int strict);
void extractArchive(const char *tarFile, int verbose, int strict);
void extractFile(int fd, struct ustar_header *hdr, const char *filePath, int verbose);
void writeHeader(int fd, struct ustar_header *header);
void writeFileContent(int tarFd, const char *filePath, off_t fileSize);
void calculateChecksum(struct ustar_header *header);
void printVerboseInfo(const struct ustar_header *hdr); 
int checkMagicAndVersion(const char *magic, const char *version, int strict); 
int32_t extract_special_int(char *where, int len);
int insert_special_int(char *where, size_t size, int32_t val);
void finalizeArchive(int archiveFd);

int main(int argc, char *argv[]) {
    int opt;
    int createFlag = 0, listFlag = 0, extractFlag = 0, verboseFlag = 0, strictFlag = 0;
    char *filename = NULL;

    while ((opt = getopt(argc, argv, "ctxvf:S")) != -1) {
        switch (opt) {
            case 'c':
                createFlag = 1;
                break;
            case 't':
                listFlag = 1;
                break;
            case 'x':
                extractFlag = 1;
                break;
            case 'v':
                verboseFlag = 1;
                break;
            case 'f':
                filename = optarg;
                break;
            case 'S':
                strictFlag = 1;
                break;
            default: /* '?' */
                fprintf(stderr, "Usage: %s -ctxv -f filename.tar [files...]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    if (filename == NULL) {
        fprintf(stderr, "An archive filename must be specified with -f option.\n");
        exit(EXIT_FAILURE);
    }

    if (createFlag + listFlag + extractFlag != 1) {
        fprintf(stderr, "One of -c, -t, or -x options must be specified.\n");
        exit(EXIT_FAILURE);
    }

    if (createFlag) {
        createArchive(filename, argc - optind, &argv[optind], verboseFlag, strictFlag);
    } else if (listFlag) {
        listContents(filename, verboseFlag, strictFlag);
    } else if (extractFlag) {
        extractArchive(filename, verboseFlag, strictFlag);
    }

    return 0;
}


void calculateChecksum(struct ustar_header *hdr) {
    unsigned char *bytes = (unsigned char *)hdr;
    unsigned int checksum = 0;
    memset(hdr->chksum, ' ', sizeof(hdr->chksum)); /* Fill checksum field with spaces */
    int i;
    for ( i= 0; i < sizeof(struct ustar_header); i++) {
        checksum += bytes[i];
    }

    snprintf(hdr->chksum, sizeof(hdr->chksum), "%06o", checksum);
}

void createArchive(const char *tarFile, int argc, char *argv[], int verbose, int strict) {
    int tarFd = open(tarFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (tarFd == -1) {
        perror("Failed to open tar file for writing");
        exit(EXIT_FAILURE);
    }

    struct stat fileStat;
    struct ustar_header hdr;
    int i;
    for (i = 0; i < argc; i++) {
        if (lstat(argv[i], &fileStat) == -1) {
            perror("Failed to get file stats");
            continue; /* Skip to the next file */
        }

        char typeflag = S_ISDIR(fileStat.st_mode) ? '5' : S_ISLNK(fileStat.st_mode) ? '2' : '0';
        fillHeader(&hdr, argv[i], &fileStat, typeflag);
        writeHeader(tarFd, &hdr);

        if (typeflag == '0') { /* Regular file */
            writeFileContent(tarFd, argv[i], fileStat.st_size);
        }

        if (verbose) {
            printf("Added %s\n", argv[i]);
        }
    }

    /* Write two empty blocks as the end of archive marker */
    char endBlock[BLOCK_SIZE] = {0};
    write(tarFd, endBlock, BLOCK_SIZE);
    write(tarFd, endBlock, BLOCK_SIZE);

    close(tarFd);
}


void printVerboseInfo(const struct ustar_header *hdr) {
    mode_t mode;
    sscanf(hdr->mode, "%o", &mode);
    printf("%c%c%c%c%c%c%c%c%c%c ", 
           (mode & S_IRUSR) ? 'r' : '-', (mode & S_IWUSR) ? 'w' : '-', (mode & S_IXUSR) ? 'x' : '-',
           (mode & S_IRGRP) ? 'r' : '-', (mode & S_IWGRP) ? 'w' : '-', (mode & S_IXGRP) ? 'x' : '-',
           (mode & S_IROTH) ? 'r' : '-', (mode & S_IWOTH) ? 'w' : '-', (mode & S_IXOTH) ? 'x' : '-',
           hdr->typeflag);
    
    printf("%s ", hdr->name);

    long size;
    sscanf(hdr->size, "%lo", &size);
    printf("%ld ", size);

    time_t mtime;
    sscanf(hdr->mtime, "%lo", &mtime);
    char timebuf[18];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M", localtime(&mtime));
    printf("%s\n", timebuf);
}

void listContents(const char *tarFile, int verbose, int strict) {
    int fd = open(tarFile, O_RDONLY);
    if (fd == -1) {
        perror("Failed to open tar file");
        exit(EXIT_FAILURE);
    }

    struct ustar_header hdr;
    while (read(fd, &hdr, sizeof(struct ustar_header)) == sizeof(struct ustar_header)) {
        if (checkMagicAndVersion(hdr.magic, hdr.version, strict) == 0) { /*Call to checkMagicAndVersion*/
            fprintf(stderr, "Not a valid ustar archive\n");
            exit(EXIT_FAILURE);
        }

        if (verbose) {
            printVerboseInfo(&hdr);
        } else {
            printf("%s\n", hdr.name);
        }

        long size;
        sscanf(hdr.size, "%lo", &size);
        lseek(fd, (size + 511) & ~511, SEEK_CUR); /* Skip to the next header */
    }

    close(fd);
}

void extractArchive(const char *tarFile, int verbose, int strict) {
    int fd = open(tarFile, O_RDONLY);
    if (fd < 0) {
        perror("Failed to open archive for extraction");
        exit(EXIT_FAILURE);
    }

    struct ustar_header hdr;
    while (read(fd, &hdr, sizeof(hdr)) == sizeof(hdr)) {
        if (checkMagicAndVersion(hdr.magic, hdr.version, strict) == 0) { /*Call to checkMagicAndVersion*/
            fprintf(stderr, "Archive format not recognized or corrupted\n");
            exit(EXIT_FAILURE);
        }

        long size;
        sscanf(hdr.size, "%lo", &size);

        if (verbose) {
            printf("Extracting %s\n", hdr.name);
        }

        /* Determine file type and handle accordingly */
        char filePath[256];
        snprintf(filePath, sizeof(filePath), "%s", hdr.name);

        if (hdr.typeflag == '0' || hdr.typeflag == '\0') { /* Regular file */
            extractFile(fd, &hdr, filePath, verbose);
        } else if (hdr.typeflag == '5') { /* Directory */
            mkdir(filePath, 0755);
        } else if (hdr.typeflag == '2') { /* Symbolic link */
            symlink(hdr.linkname, filePath);
        }

        lseek(fd, (size + 511) & ~511, SEEK_CUR); /* Move to the next header */
    }

    close(fd);
}

void extractFile(int fd, struct ustar_header *hdr, const char *filePath, int verbose) {
    int outFileFd = open(filePath, O_WRONLY | O_CREAT | O_TRUNC, strtol(hdr->mode, NULL, 8));
    if (outFileFd == -1) {
        perror("Failed to create output file");
        exit(EXIT_FAILURE);
    }

    char buffer[BLOCK_SIZE];
    long fileSize;
    sscanf(hdr->size, "%lo", &fileSize);
    ssize_t bytesRemaining = fileSize;

    while (bytesRemaining > 0) {
        ssize_t bytesRead = read(fd, buffer, sizeof(buffer));
        if (bytesRead == -1) {
            perror("Error reading from archive");
            exit(EXIT_FAILURE);
        }

        ssize_t bytesToWrite = (bytesRemaining < bytesRead) ? bytesRemaining : bytesRead;
        if (write(outFileFd, buffer, bytesToWrite) != bytesToWrite) {
            perror("Error writing to output file");
            exit(EXIT_FAILURE);
        }

        bytesRemaining -= bytesToWrite;
    }

    close(outFileFd);

    if (verbose) {
        printf("Extracted file: %s\n", filePath);
    }
}


int checkMagicAndVersion(const char *magic, const char *version, int strict) {
    if (strict) {
        return strncmp(magic, USTAR_MAGIC, USTAR_MAGIC_LEN) == 0 && strncmp(version, USTAR_VERSION, 2) == 0;
    }
    return strncmp(magic, USTAR_MAGIC, USTAR_MAGIC_LEN) == 0;
}

int32_t extract_special_int(char *where, int len) {
    int32_t val = -1;
    if ((len >= sizeof(val)) && (where[0] & 0x80)) {
        val = *(int32_t *)(where + len - sizeof(val));
        val = ntohl(val);
    }
    return val;
}

int insert_special_int(char *where, size_t size, int32_t val) {
    int err = 0;
    if (val < 0 || (size < sizeof(val))) {
        err++;
    } else {
        memset(where, 0, size);
        *(int32_t *)(where + size - sizeof(val)) = htonl(val);
        *where |= 0x80;
    }
    return err;
}

void fillHeader(struct ustar_header *header, const char *filePath, struct stat *fileStat, char typeflag) {
    memset(header, 0, sizeof(struct ustar_header)); /* Clear the header struct */

    /* Fill the header based on fileStat and filePath */
    snprintf(header->name, sizeof(header->name), "%s", filePath);
    snprintf(header->mode, sizeof(header->mode), "%07o", fileStat->st_mode & 0777);
    snprintf(header->uid, sizeof(header->uid), "%07o", fileStat->st_uid);
    snprintf(header->gid, sizeof(header->gid), "%07o", fileStat->st_gid);
    snprintf(header->size, sizeof(header->size), "%011lo", (unsigned long)fileStat->st_size);
    snprintf(header->mtime, sizeof(header->mtime), "%011lo", (unsigned long)fileStat->st_mtime);
    header->typeflag = typeflag;
    strncpy(header->magic, USTAR_MAGIC, USTAR_MAGIC_LEN);
    strncpy(header->version, USTAR_VERSION, sizeof(header->version));

    calculateChecksum(header);
}

void writeFileContent(int tarFd, const char *filePath, off_t fileSize) {
    int fileFd = open(filePath, O_RDONLY);
    if (fileFd < 0) {
        perror("Error opening file to write content");
        exit(EXIT_FAILURE);
    }

    char buffer[BLOCK_SIZE];
    ssize_t bytesRead;
    while ((bytesRead = read(fileFd, buffer, BLOCK_SIZE)) > 0) {
        write(tarFd, buffer, bytesRead);
        if (bytesRead < BLOCK_SIZE) {
            /*Pad the remaining part of the block with zeros*/
            memset(buffer + bytesRead, 0, BLOCK_SIZE - bytesRead);
            write(tarFd, buffer + bytesRead, BLOCK_SIZE - bytesRead);
        }
    }
    close(fileFd);
}

void writeHeader(int fd, struct ustar_header *header) {
    calculateChecksum(header);
    if (write(fd, header, sizeof(struct ustar_header)) != sizeof(struct ustar_header)) {
        perror("Error writing header to archive");
        exit(EXIT_FAILURE);
    }
}

void finalizeArchive(int archiveFd) {
    char endBlock[BLOCK_SIZE * 2] = {0};
    write(archiveFd, endBlock, sizeof(endBlock));
}
