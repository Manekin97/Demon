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

// funckje nie powinny miec exita, tylko zwracać ujemna liczbe, potem trzeba to poprawić
// czy potrzebne jest realpath? chyba nie

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

int RegularCopy(char *srcPath, char *destPath) {
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

int Copy(char *srcPath, char *destPath, struct stat *srcFileInfo) {
    if (srcFileInfo->st_size < fileSizeThreshHold) {
        if (RegularCopy(srcPath, destPath) == -1) {
            syslog(LOG_INFO, "RegularCopy(): Could not copy %s to %s", srcPath, destPath);
            exit(EXIT_FAILURE);
        }
    }
    else {
        if (MmapCopy(srcPath, destPath) == -1) {
            syslog(LOG_INFO, "MmapCopy(): Could not copy %s to %s", srcPath, destPath);
            exit(EXIT_FAILURE);
        }
    }
}

char *AppendFileNameToPath(char *path, char *filename) {
    char *newPath = malloc(PATH_MAX * sizeof(char));
    if(sprintf(newPath, "%s/%s", path, filename) == -1) {
        syslog(LOG_INFO, "sprintf(): Error appending filename");
        exit(EXIT_FAILURE);
    }

    return newPath;
}

struct stat *GetFileInfo(char *path) {
    struct stat *fileInfo = malloc(sizeof(struct stat));
    
    if (stat(path, fileInfo) == -1) {
        syslog(LOG_INFO, "stat(): %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    return fileInfo;
}

int CopyAllFilesFromList(List *list, char *srcDir, char *destDir) {
    char *fullSrcFilePath = NULL;
    char *fullDestFilePath = NULL;
    struct stat *fileInfo = NULL;    
    
    Node *current = list->head;
    while(current != NULL) { 
        fullSrcFilePath = AppendFileNameToPath(srcDir, current->fileName);
        fullDestFilePath = AppendFileNameToPath(destDir, current->fileName);        
        fileInfo = GetFileInfo(fullSrcFilePath);    
        
        Copy(fullSrcFilePath, fullDestFilePath, fileInfo);

        syslog(LOG_INFO, "%s has been copied to %s", fullSrcFilePath, fullDestFilePath); 
        current = current->next;                       
    }

    free(fullSrcFilePath);
    free(fullDestFilePath);
    free(fileInfo);    
}

int RemoveAllFilesFromList(List *list, char *path) {
    char *fullPath = NULL;
    
    Node *current = list->head;
    while(current != NULL) {
        fullPath = AppendFileNameToPath(path, current->fileName);           
        if(remove(fullPath) == -1) {
            syslog(LOG_INFO, "remove(): Could not remove %s", fullPath);                 
            exit(EXIT_FAILURE);
        }

        syslog(LOG_INFO, "%s has been removed", fullPath);    
        current = current->next;                                     
    }
}

int SyncModTime(struct stat *fileInfo, char *destPath) {
    struct utimbuf *newTime = malloc(sizeof(struct utimbuf));
    time_t *t = malloc(sizeof(time_t));
    
    newTime->actime = time(t);
    newTime->modtime = fileInfo->st_mtime;
    if (utime(destPath, newTime) == -1) {
        syslog(LOG_INFO, "utime(): %s", strerror(errno));
    }
}

List *GetAllFileNamesFromDir(DIR *dir, bool ignoreNonRegFiles) {
    List *list = InitList();
    struct dirent *dirInfo = NULL;

    if(ignoreNonRegFiles) {
        while ((dirInfo = readdir(dir)) != NULL) {
            if(dirInfo->d_type == DT_REG){
                Append(dirInfo->d_name, list);
            }
        }
    }
    else {
        while ((dirInfo = readdir(dir)) != NULL) {
            Append(dirInfo->d_name, list);
        }
    }   

    return list;
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

    struct stat *srcFileInfo = NULL;
    struct stat *destFileInfo = NULL;

    char *fullSrcFilePath = NULL;
    char *fullDestFilePath = NULL;
    char *resolvedPath = malloc(PATH_MAX * sizeof(char));

    bool found = false;
    Node *tmp = malloc(sizeof(struct node));

    source = opendir(srcPath);
    if (!source) {
        syslog(LOG_INFO, "opendir(): \"%s\" %s", srcPath, strerror(errno));
        exit(EXIT_FAILURE); 
    }
 
    destination = opendir(resolvedPath); 
    if (!destination) {
        syslog(LOG_INFO, "opendir(): \"%s\" could not be opened properly", destPath); 
        exit(EXIT_FAILURE); 
    }

    List *srcDirFiles = GetAllFileNamesFromDir(source, true);
    List *destDirFiles = GetAllFileNamesFromDir(destination, true);

    Node *currentSrc = srcDirFiles->head;
    Node *currentDest = destDirFiles->head;
    while(currentSrc != NULL) {
        while(currentDest != NULL) {
            if(strcmp(currentSrc->fileName, currentDest->fileName) == 0) { 
                fullSrcFilePath = AppendFileNameToPath(realpath(srcPath, resolvedPath), currentSrc->fileName);
                fullDestFilePath = AppendFileNameToPath(realpath(destPath, resolvedPath), currentDest->fileName);
                
                srcFileInfo = GetFileInfo(fullSrcFilePath);
                destFileInfo = GetFileInfo(fullDestFilePath);

                if(srcFileInfo->st_mtime > destFileInfo->st_mtime) {
                    Copy(fullSrcFilePath, fullDestFilePath, srcFileInfo);
                    SyncModTime(srcFileInfo, fullDestFilePath);
                    syslog(LOG_INFO, "%s has been copied to %s cause of mod", fullSrcFilePath, fullDestFilePath);   
                }
                else { // jak nie to tylko usunac z listy
                    Remove(currentDest->fileName, destDirFiles);
                    syslog(LOG_INFO, "%s usuniety z listy destDirFiles", currentDest->fileName);                   
                    currentDest = destDirFiles->head;  
                    found = true;              
                    break;
                }
            }

            if(found) {
                Remove(currentDest->fileName, destDirFiles);
                syslog(LOG_INFO, "%s usuniety z listy destDirFiles", currentDest->fileName);                   
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
            syslog(LOG_INFO, "%s usuniety z listy srcDirFiles", currentSrc->fileName);                               
            found = false;
            currentSrc = tmp;
        }
        else {
            currentSrc = currentSrc->next;
        }
    }

    // currentSrc = srcDirFiles->head;
    // currentDest = destDirFiles->head;

    // syslog(LOG_INFO, "currentSrc:");
    // while(currentSrc != NULL) {
    //     syslog(LOG_INFO, "%s", currentSrc->fileName);
    //     currentSrc = currentSrc->next;        
    // }

    // syslog(LOG_INFO, "currentDest:");
    // while(currentDest != NULL) {
    //     syslog(LOG_INFO, "%s", currentDest->fileName);
    //     currentDest = currentDest->next;        
    // }

    CopyAllFilesFromList(srcDirFiles, srcPath, destPath);
    RemoveAllFilesFromList(destDirFiles, destPath);

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
    free(srcDirFiles);
    free(destDirFiles);    
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
    exit(EXIT_SUCCESS); 
}

//  @TODO
//  Poprawić syslogi
//  Dodac rekurencyjne przeszukiwanie katalogów
// dm w src < dm w dest
//  usuwanie pliku z listy potem można przyspieszyć, bo teraz to szuka od początku w liście, a mogę po prostu podać np. currentSrc