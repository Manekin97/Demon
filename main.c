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
#define MAX_SLEEP_TIME 86400

int fileSizeThreshHold = 4096; 
int sleepInterval = 300; 
bool recursiveSearch = false; 

int MmapCopy(char *srcPath, char *destPath) {
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
    if (fstat(source, &fileInfo) == -1) {
        return -1; 
    }
    else {
        fileSize = fileInfo.st_size; 
    }

    int *srcAddress = mmap(NULL, fileSize, PROT_READ, MAP_PRIVATE, source, 0); 
    if (srcAddress == MAP_FAILED) {
        return -1; 
    }

    if(ftruncate(destination, fileSize)) {
        return -1;
    } 

    int *destAddress = mmap(NULL, fileSize, PROT_READ | PROT_WRITE, MAP_SHARED, destination, 0); 
    if (destAddress == MAP_FAILED) { 
        return -1; 
    }

    memcpy(destAddress, srcAddress, fileSize);
    
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

int Copy(char *srcPath, char *destPath) {
    char *buffer = malloc(sizeof(char) * BUFFER_SIZE); 

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

    struct stat *srcFileInfo = malloc(sizeof(struct stat));
    struct stat *destFileInfo = malloc(sizeof(struct stat)); 

    List *srcDirFiles = InitList();
    List *destDirFiles = InitList();

    char *fullSrcFilePath = malloc(PATH_MAX * sizeof(char));
    char *fullDestFilePath = malloc(PATH_MAX * sizeof(char));
    char *resolvedPath = malloc(PATH_MAX * sizeof(char));

    bool found = false;
    Node *tmp = malloc(sizeof(struct node));
    struct utimbuf *newTime = malloc(sizeof(struct utimbuf));

    source = opendir(srcPath);
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
                sprintf(fullSrcFilePath, "%s/%s", realpath(srcPath, resolvedPath), currentSrc->fileName);
                sprintf(fullDestFilePath, "%s/%s", realpath(destPath, resolvedPath), currentDest->fileName);
                
                if (stat(fullSrcFilePath, srcFileInfo) == -1) {
                    syslog(LOG_INFO, "stat(): %s", strerror(errno)); 
                    exit(EXIT_FAILURE);
                }

                if (stat(fullDestFilePath, destFileInfo) == -1) {
                    syslog(LOG_INFO, "stat(): %s", strerror(errno));
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

                    found = true;
                    newTime->actime = srcFileInfo->st_atime;
                    newTime->modtime = srcFileInfo->st_mtime;
                    if(utime(fullDestFilePath, newTime) == -1) {
                        syslog(strerror(errno));
                    }
                    syslog(LOG_INFO, "%s has been copied to %s cause of mod", fullSrcFilePath, fullDestFilePath);   
                }
            }

            if(found) {
                Remove(currentDest->fileName, destDirFiles);
                currentDest = destDirFiles->head;                
                break;
            }
            else {
                currentDest = currentDest->next;
            }
        }

        if(found) {
            tmp = currentSrc->next;
            Remove(currentSrc->fileName, srcDirFiles);
            found = false;
            currentSrc = tmp;
        }
        else {
            currentSrc = currentSrc->next;
        }
    }

    currentSrc = srcDirFiles->head;
    currentDest = destDirFiles->head;

    while(currentSrc != NULL) { 
        sprintf(fullSrcFilePath, "%s/%s", realpath(srcPath, resolvedPath), currentSrc->fileName);    
        sprintf(fullDestFilePath, "%s/%s", realpath(destPath, resolvedPath), currentSrc->fileName); 
        if (stat(fullSrcFilePath, srcFileInfo) == -1) {
            syslog(LOG_INFO, "stat(): %s", strerror(errno)); 
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

        syslog(LOG_INFO, "%s has been copied to %s", fullSrcFilePath, fullDestFilePath); 
        currentSrc = currentSrc->next;                       
    }

    while(currentDest != NULL) {
        sprintf(fullDestFilePath, "%s/%s", realpath(destPath, resolvedPath), currentDest->fileName);            
        if(remove(fullDestFilePath) == -1) {
            syslog(LOG_INFO, "remove(): Could not remove %s", fullDestFilePath);                 
            exit(EXIT_FAILURE);
        }

        syslog(LOG_INFO, "%s has been removed", fullDestFilePath);    

        currentDest = currentDest->next;                                     
    }

    if(closedir(source) == -1) {
        syslog(LOG_INFO, "Could not close \"%s\"", srcPath);
    }

    if(closedir(destination) == -1) {
        syslog(LOG_INFO, "Could not close \"%s\"", destPath);
    }

    free(srcFileInfo);
    free(destFileInfo);
    free(fullSrcFilePath);
    free(fullDestFilePath);
    free(resolvedPath);
    free(newTime);
}

void RecursiveDirSync(const char *srcPath, const char *destPath) {

}

void Sigusr1Handler(int signo) {
    syslog(LOG_INFO, "Daemon was awakened by signal SIGUSR1");
}

int main(int argc, char *const argv[]) {
    const char *srcPath = argv[1]; 
    const char *destPath = argv[2]; 

    struct stat *srcDirInfo = malloc(sizeof(struct stat));
    struct stat *destDirInfo = malloc(sizeof(struct stat));

    if (signal(SIGUSR1, &Sigusr1Handler) == SIG_ERR) {
        perror("singal()");
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
                if(sleepInterval > MAX_SLEEP_TIME) {
                    sleepInterval = MAX_SLEEP_TIME;
                }

                break; 
            case 'R':
                recursiveSearch = true; 
                break; 
            case '?':
                printf("Wrong Arguments\n"); 
                return -1; 
        }
    }

    if(stat(srcPath, srcDirInfo) == -1) {
        perror("stat");
        exit(EXIT_FAILURE);
    }
    
    if(stat(destPath, destDirInfo) == -1) {
        perror("stat");
        exit(EXIT_FAILURE);
    }

    if(!S_ISDIR(srcDirInfo->st_mode)) {
        printf("\"%s\" is not a directory", srcPath);
        exit(EXIT_FAILURE);
    }

    if(!S_ISDIR(destDirInfo->st_mode)) {
        printf("\"%s\" is not a directory", destPath);
        exit(EXIT_FAILURE);
    }

    Daemonize(); 
 
    while (1) { 
        if(!recursiveSearch) {
            DirSync(srcPath, destPath);
        }
        else {
            RecursiveDirSync(srcPath, destPath);
        }

        syslog(LOG_INFO, "Daemon went to sleep for %d s", sleepInterval);
        sleep(sleepInterval); 
    }

    free(srcDirInfo);
    free(destDirInfo);
    return 0; 
}

//  @TODO
//  Poprawić syslogi
//  Dodac rekurencyjne przeszukiwanie katalogów
// dm w src < dm w dest
//  usuwanie pliku z listy potem można przyspieszyć, bo teraz to szuka od początku w liście, a mogę po prostu podać np. currentSrc