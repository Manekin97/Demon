#include <stdlib.h> 
#include <stdio.h> 
#include <fcntl.h> 
#include <unistd.h> 
#include <dirent.h> 
#include <errno.h> 
#include <signal.h> 
#include <stdbool.h> 
#include <string.h> 
#include <limits.h>
#include <utime.h>
#include <time.h>
#include <syslog.h>
#include <sys/types.h> 
#include <sys/stat.h> 
#include <sys/mman.h> 
#include <linux/fs.h> 
       
#include "list.c"

#define BUFFER_SIZE 131072

int fileSizeThreshHold = 4096; 
int sleepInterval = 600; 
bool recursiveSearch = false; 

int MmapCopy(char * srcPath, char * destPath) {
    struct stat fileInfo; 

    int source = open(srcPath, O_RDONLY); 
    if (source == -1) {
        return -1; 
    }

    int destination = open(destPath, O_RDWR | O_CREAT, 00777); //  Nie wiem narazie co zrobić z dostepem, wiec zostawie 00777
    if (destination == -1) {
        return -1; 
    }

    int fileSize; 
    if (fstat(source,  & fileInfo) == -1) {
        return -1; 
    }
    else {
        fileSize = fileInfo.st_size; 
    }

    int * srcAddress = mmap(NULL, fileSize, PROT_READ, MAP_PRIVATE, source, 0); 
    if (srcAddress == MAP_FAILED) {
        return -1; 
    }

    ftruncate(destination, fileSize); 

    int * destAddress = mmap(NULL, fileSize, PROT_READ | PROT_WRITE, MAP_SHARED, destination, 0); 
    if (destAddress == MAP_FAILED) { 
        return -1; 
    }

    memcpy(srcAddress, destAddress, fileSize); 
    
    if (munmap(srcAddress, fileSize) == -1) {
        return -1; 
    }

    if (munmap(destAddress, fileSize) == -1) {
        return -1; 
    }

    if (close(source) == -1) {
        return -1; 
    }

    if (close(destination) == -1) { 
        return -1; 
    }

    return 0; 
}

int Copy(char * srcPath, char * destPath) {
    char * buffer = malloc(sizeof(char) * BUFFER_SIZE); 

    int source = open(srcPath, O_RDONLY); 
    if (source == -1) {
        return -1; 
    }

    int destination = open(destPath, O_WRONLY | O_CREAT, 00777); //  Nie wiem narazie co zrobić z dostepem, wiec zostawie 00777
    if (destination == -1) {
        return -1; 
    }

    ssize_t bytesRead; 
    ssize_t bytesWritten; 
    while ((bytesRead = read(source, buffer, BUFFER_SIZE)) != 0) {
        if (bytesRead == -1) {
            if (errno == EINTR) {
            continue; 
        }

        return -1; 
        }

        bytesWritten = write(destination, buffer, bytesRead); 
        if (bytesWritten == -1) {
            return -1; 
        }

        if (bytesRead != bytesWritten) {
            return -1; 
        }
    }

    if (close(source) == -1) { 
        return -1; 
    }

    if (close(destination) == -1) {
        return -1; 
    }

    free(buffer); 
    return 0; 
}

void Daemonize() {
    pid_t pid; 

    pid = fork(); 
    if (pid == -1) {
        perror("fork");
        exit(EXIT_FAILURE); 
    }
    else if (pid > 0) {
        printf("Parent process has been terminated with succes\n");
        exit(EXIT_SUCCESS); 
    }

    if (setsid() == -1) {
        perror("setsid");
        exit(EXIT_FAILURE); 
    }

    signal(SIGHUP, SIG_IGN);
    pid = fork();

    if (pid == -1) {
        perror("fork");
        exit(EXIT_FAILURE); 
    }
    else if (pid > 0) {
        printf("Parent process has been terminated with succes\n");
        exit(EXIT_SUCCESS); 
    }

    if (chdir("/") == -1) {
        perror("chdir");
        exit(EXIT_FAILURE); 
    }

    umask(0);

    for (int i = 0; i < sysconf(_SC_OPEN_MAX); i++) {
        close(i); 
    }

    openlog(NULL, LOG_USER, LOG_USER);  

    if (open("/dev/null", O_RDONLY) == -1) {
        syslog(LOG_INFO, "STDIN could not be opened properly");
        exit(EXIT_FAILURE);
    }
    if (open("/dev/null", O_WRONLY) == -1) {
        syslog(LOG_INFO, "STDOUT could not be opened properly");
        exit(EXIT_FAILURE);
    }
    if (open("/dev/null", O_RDWR) == -1) {
        syslog(LOG_INFO, "STDERR could not be opened properly");
        exit(EXIT_FAILURE);
    }
}

void DirSync(const char *srcPath, const char *destPath) {
    DIR *source = NULL; 
    DIR *destination = NULL;  

    struct dirent *srcDirInfo = NULL; 
    struct dirent *destDirInfo = NULL;

    struct stat *srcFileInfo = NULL;
    struct stat *destFileInfo = NULL;    

    List *srcDirFiles = InitList();
    List *destDirFiles = InitList();

    char *fullSrcFilePath = malloc(PATH_MAX * sizeof(char));
    char *fullDestFilePath = malloc(PATH_MAX * sizeof(char));
    char *resolvedPath = malloc(PATH_MAX * sizeof(char));

    struct utimbuf *newTime = NULL;

    realpath(srcPath, resolvedPath);   
    source = opendir(resolvedPath);
    if (!source) {
        syslog(LOG_INFO, "opendir(): \"%s\" %s", srcPath, strerror(errno));
        exit(EXIT_FAILURE); 
    }

    realpath(destPath, resolvedPath); 
    destination = opendir(resolvedPath); 
    if (!destination) {
        syslog(LOG_INFO, "opendir(): \"%s\" could not be opened properly", destPath); 
        exit(EXIT_FAILURE); 
    }

    while ((srcDirInfo = readdir(source)) != NULL) {
        if(srcDirInfo->d_type == DT_REG){
            Append(srcDirInfo->d_name, srcDirFiles);
        }
    }

    while ((destDirInfo = readdir(destination)) != NULL) {
        if(destDirInfo->d_type == DT_REG) {
            Append(destDirInfo->d_name, destDirFiles);
        }
    }

    Node *currentSrc = srcDirFiles->head;
    Node *currentDest = destDirFiles->head;
    while(currentSrc != NULL) {
        while(currentDest != NULL) {
            if(strcmp(currentSrc->fileName, currentDest->fileName) == 0) {
                sprintf(fullSrcFilePath, "%s/%s", realpath(srcPath, resolvedPath), srcDirInfo->d_name);
                if (stat(fullSrcFilePath, srcFileInfo) == -1) {
                    syslog(LOG_INFO, "stat(): Could not get information about %s", fullSrcFilePath); 
                    exit(EXIT_FAILURE);
                }

                sprintf(fullDestFilePath, "%s/%s", realpath(destPath, resolvedPath), srcDirInfo->d_name);
                if (stat(fullDestFilePath, destFileInfo) == -1) {
                    syslog(LOG_INFO, "stat(): Could not get information about %s", fullDestFilePath); 
                    exit(EXIT_FAILURE);
                }

                if(srcFileInfo->st_mtime > destFileInfo->st_mtime) {
                    if(srcFileInfo->st_size < fileSizeThreshHold) {
                        if(Copy(fullSrcFilePath, fullDestFilePath) == -1) {
                            syslog(LOG_INFO, "Copy(): Could not copy %s to %s", fullSrcFilePath, fullDestFilePath); 
                            exit(EXIT_FAILURE);
                        }
                    }
                    else {
                        if(MmapCopy(fullSrcFilePath, fullDestFilePath) == -1) {
                            syslog(LOG_INFO, "MmapCopy(): Could not copy %s to %s", fullSrcFilePath, fullDestFilePath); 
                            exit(EXIT_FAILURE);
                        }
                    }

                    //  To potem można przyspieszyć, bo teraz to szuka od początku w liście, a mogę po prostu podać currentSrc
                    time_t *currentTime = NULL;
                    time(currentTime);
                    newTime->actime = *currentTime;
                    newTime->modtime = *currentTime;
                    utime(fullDestFilePath, newTime);
                    syslog(LOG_INFO, "<%s>: %s has been copied to %s", asctime(gmtime(currentTime)), fullSrcFilePath, fullDestFilePath);                     
                    Remove(currentSrc->fileName, srcDirFiles);
                    Remove(currentDest->fileName, destDirFiles);
                    break;
                }
            }

            currentDest = currentDest->next;
        }

        currentSrc = currentSrc->next;
    }

    currentSrc = srcDirFiles->head;
    currentDest = destDirFiles->head;

    while(currentSrc != NULL) { 
        sprintf(fullSrcFilePath, "%s/%s", realpath(srcPath, resolvedPath), currentSrc->fileName);    
        sprintf(fullDestFilePath, "%s/%s", realpath(destPath, resolvedPath), currentSrc->fileName);    
        if (stat(fullSrcFilePath, srcFileInfo) == -1) {
            syslog(LOG_INFO, "stat(): Could not get information about %s", fullSrcFilePath); 
            exit(EXIT_FAILURE);
        }

        if(srcFileInfo->st_size < fileSizeThreshHold) {
            if(Copy(fullSrcFilePath, fullDestFilePath) == -1) {
                syslog(LOG_INFO, "Copy(): Could not copy %s to %s", fullSrcFilePath, fullDestFilePath);                 
                exit(EXIT_FAILURE);
            }
        }
        else {
            if(MmapCopy(fullSrcFilePath, fullDestFilePath) == -1) {
                syslog(LOG_INFO, "MmapCopy(): Could not copy %s to %s", fullSrcFilePath, fullDestFilePath);                                 
                exit(EXIT_FAILURE);
            }
        }

        time_t *currentTime = NULL;
        time(currentTime);
        syslog(LOG_INFO, "<%s>: %s has been copied to %s", asctime(gmtime(currentTime)), fullSrcFilePath, fullDestFilePath);                         
    }

    while(currentDest != NULL) {
        sprintf(fullDestFilePath, "%s/%s", realpath(destPath, resolvedPath), currentDest->fileName);            
        if(remove(fullDestFilePath) == -1) {
            syslog(LOG_INFO, "remove(): Could not remove %s", fullDestFilePath);                 
            exit(EXIT_FAILURE);
        }

        time_t *currentTime = NULL;
        time(currentTime);
        syslog(LOG_INFO, "<%s>: %s has been removed", asctime(gmtime(currentTime)), fullDestFilePath);                                         
    }
}

void Sigusr1Handler(int signo) {
    printf("Process was awakened"); //  To powinno iść do logu
}

int main(int argc, char * const argv[]) {
    const char * srcPath = argv[1]; 
    const char * destPath = argv[2]; 

    if (signal(SIGUSR1,  &Sigusr1Handler) == SIG_ERR) {
        printf("Signal SIGUSR1 could not be handled"); //a nie perror?
        exit(EXIT_FAILURE); 
    }

    int argument; 
    while ((argument = getopt(argc, argv, "Rt:i:")) != -1) {
        switch (argument) {
            case 't':
                fileSizeThreshHold = atoi(optarg); 
                break; 
            case 'i':
                sleepInterval = atoi(optarg); 
                break; 
            case 'R':
                recursiveSearch = true; 
                break; 
            case '?':
                printf("Wrong Arguments\n"); 
                return-1; 
        }
    }
    
    Daemonize(); 
 
    while (1) { 
        DirSync(srcPath, destPath);
        sleep(sleepInterval); 
    }

    return 0; 
}

//  @TODO
//  Dodać syslogi
//  Dodać sprawdzanie w DirSync(), czy pliki sa zwykle (ma pomijać katalogi i inne gówna, które nie są plikami)
//  Dodac rekurencyjne przeszukiwanie katalogów