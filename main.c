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

//  @TODO
//  Poprawić syslogi
//  Dodałem optymalizacje pamięci, sprawdzić, czy działa

struct stat *GetFileInfo(const char *path) {
    struct stat *fileInfo = malloc(sizeof(struct stat));
    
    if (stat(path, fileInfo) == -1) {
        syslog(LOG_INFO, "stat(): %s (%s)", path, strerror(errno));
        return NULL;
    }

    return fileInfo;
}

int MmapCopy(const char *srcPath, const char *destPath) {
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

int RegularCopy(const char *srcPath, const char *destPath) {
    // char *buffer = malloc(sizeof(char) * BUFFER_SIZE); 
    char buffer[BUFFER_SIZE];

    int source = open(srcPath, O_RDONLY); 
    if (source == -1) {
        return -1; 
    }

    int destination = open(destPath, O_WRONLY | O_CREAT | O_TRUNC, 00777); //  Nie wiem narazie co zrobić z dostepem, wiec zostawie 00777
    if (destination == -1) {
        return -1; 
    }

    ssize_t bytesRead; 
    ssize_t bytesWritten; 
    while ((bytesRead = read(source, &buffer, BUFFER_SIZE)) != 0) {
        if (bytesRead == -1) {
            if (errno == EINTR) {
                continue; 
            }

            return -1; 
        }

        bytesWritten = write(destination, &buffer, bytesRead); 
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

    //free(buffer); 
    return 0; 
}

int Copy(const char *srcPath, const char *destPath) {
    struct stat *srcFileInfo = GetFileInfo(srcPath);

    if (srcFileInfo->st_size < fileSizeThreshHold) {
        if (RegularCopy(srcPath, destPath) == -1) {
            syslog(LOG_INFO, "RegularCopy(): Could not copy %s to %s", srcPath, destPath);
            return -1;
        }
    }
    else {
        if (MmapCopy(srcPath, destPath) == -1) {
            syslog(LOG_INFO, "MmapCopy(): Could not copy %s to %s", srcPath, destPath);
            return -1;
        }
    }

    free(srcFileInfo);
    return 0;
}

char *AppendToPath(const char *path, char *filename) {
    //char *newPath = malloc(PATH_MAX * sizeof(char));
    char *newPath = malloc(sizeof(path) + sizeof(filename) + 1);    //  nie wiem, czy tu nie trzeba +2 bo jeszcze \0
    if(sprintf(newPath, "%s/%s", path, filename) < 0) {
        return NULL;
    }

    return newPath;
}

int CopyAllFilesFromList(List *list, const char *srcDir, const char *destDir) {
    char *fullSrcFilePath = NULL;
    char *fullDestFilePath = NULL;
    struct stat *fileInfo = NULL;    
    
    Node *current = list->head;
    while(current != NULL) { 
        fullSrcFilePath = AppendToPath(srcDir, current->filename); 
        fullDestFilePath = AppendToPath(destDir, current->filename);       
        fileInfo = GetFileInfo(fullSrcFilePath);    
        
        if(Copy(fullSrcFilePath, fullDestFilePath) == -1) {
            syslog(LOG_INFO, "Copy(): Could not copy"); 

            free(fullSrcFilePath);
            free(fullDestFilePath);
            free(fileInfo); 

            return -1;
        }

        syslog(LOG_INFO, "%s has been copied to %s", fullSrcFilePath, fullDestFilePath); 
        current = current->next;                       
    }

    free(fullSrcFilePath);
    free(fullDestFilePath);
    free(fileInfo);    

    return 0;
}

int RemoveAllFilesFromList(List *list, const char *path) {
    char *fullPath = NULL;
    
    Node *current = list->head;
    while(current != NULL) {
        fullPath = AppendToPath(path, current->filename);           
        if(remove(fullPath) == -1) {
            syslog(LOG_INFO, "remove(): Could not remove %s", fullPath);   
            free(fullPath);            

            return -1;
        }

        syslog(LOG_INFO, "%s has been removed", fullPath);    
        current = current->next;                                     
    }

    free(fullPath);
    return 0;
}

int SyncModTime(struct stat *fileInfo, const char *destPath) {
    struct utimbuf newTime;
    //struct utimbuf *newTime = malloc(sizeof(struct utimbuf)); // to lepiej statycznie zadeklarować
    //time_t *t = malloc(sizeof(time_t)); // to chyba nie potrzebne, bo mozena wywołać time(NULL)
    
    //newTime->actime = time(t);
    newTime->actime = time(NULL);
    newTime->modtime = fileInfo->st_mtime;
    if (utime(destPath, &newTime) == -1) {
        syslog(LOG_INFO, "utime(): %s", strerror(errno));
        //free(newTime);
        //free(t); 

        return -1;
    }

    //free(newTime);
    //free(t);    
    return 0;
}

int CompareModTime(const char *srcPath, const char *destPath) {
    struct stat *srcFileInfo = NULL;
    struct stat *destFileInfo = NULL;

    srcFileInfo = GetFileInfo(srcPath);
    destFileInfo = GetFileInfo(destPath);

    if (srcFileInfo->st_mtime > destFileInfo->st_mtime) {
        free(srcFileInfo);
        free(destFileInfo);

        return 1;
    }
    else if (srcFileInfo->st_mtime == destFileInfo->st_mtime){
        free(srcFileInfo);
        free(destFileInfo);

        return 0;
    }
    else {
        free(srcFileInfo);
        free(destFileInfo);

        return -1;
    }
}

int FindAndCopy(List *list, const char *srcPath, const char *destPath, char *filename) {
    char *fullSrcFilePath = NULL;
    char *fullDestFilePath = NULL;

    struct stat *srcFileInfo = NULL;
    struct stat *destFileInfo = NULL;

    Node *current = list->head;
    while(current != NULL) {
        if(strcmp(current->filename, filename) == 0) { 
            fullSrcFilePath = AppendToPath(srcPath, filename);
            fullDestFilePath = AppendToPath(destPath, filename);
            
            if(CompareModTime(fullSrcFilePath, fullDestFilePath) != 0) {
                if(Copy(fullSrcFilePath, fullDestFilePath) == -1) {
                    syslog(LOG_INFO, "Copy(): Could not copy \"%s\" to \"%s\"", fullSrcFilePath, fullDestFilePath); 
                    RemoveAt(current, list);                 

                    free(fullSrcFilePath);
                    free(fullDestFilePath);
                    free(srcFileInfo);
                    free(destFileInfo);

                    return 1;
                }
            }             

            RemoveAt(current, list); 
            free(fullSrcFilePath);
            free(fullDestFilePath);
            free(srcFileInfo);
            free(destFileInfo);

            return 0;
        }
        else {
            current = current->next;
        }
    }

    free(fullSrcFilePath);
    free(fullDestFilePath);
    free(srcFileInfo);
    free(destFileInfo);
    
    return -1;
}

int CopyDirectory(DIR *source, const char *srcPath, const char *destPath) {
    struct dirent *srcFileInfo = NULL;

    if(mkdir(destPath, 0777) == -1) { // narazie nie wiem jaki dostep, wiec zostaje 0777
        syslog(LOG_INFO, "mkdir(): \"%s\" (%s)", destPath, strerror(errno));
        return -1;
    } 

    source = opendir(destPath); // to było zakomoentowane
    if (!source) {
        syslog(LOG_INFO, "opendir(): \"%s\" (%s)", destPath, strerror(errno)); 
        return -1;
    }    

    source = opendir(srcPath); 
    if (!source) {
        syslog(LOG_INFO, "opendir(): \"%s\" (%s)", srcPath, strerror(errno)); 
        return -1;
    }  

    while((srcFileInfo = readdir(source)) != NULL) {
        if(strcmp(srcFileInfo->d_name, ".") == 0 || strcmp(srcFileInfo->d_name, "..") == 0) {
            continue;
        }

        if(srcFileInfo->d_type == DT_REG) {
            if(Copy(AppendToPath(srcPath, srcFileInfo->d_name), AppendToPath(destPath, srcFileInfo->d_name)) == -1) {
                syslog(LOG_INFO, "Copy(): \"%s\" could not be copied", AppendToPath(srcPath, srcFileInfo->d_name));                                 
                return -1;
            }            
        }
        else if(srcFileInfo->d_type == DT_DIR) {
            if(CopyDirectory(source, AppendToPath(srcPath, srcFileInfo->d_name), AppendToPath(destPath, srcFileInfo->d_name)) == -1) {
                syslog(LOG_INFO, "CopyDirectory(): Could not copy \"%s\" to \"%s\"", srcPath, destPath);                   
                return -1;
            }
        }
    }

    if(closedir(source) == -1) {
        syslog(LOG_INFO, "closedir(): \"%s\" (%s)", srcPath, strerror(errno));
        return -1;
    }
                     
    return 0;
}

int RemoveDirectory(const char *path) {
    DIR *directory = NULL;
    struct dirent *fileInfo = NULL;

    directory = opendir(path); 
    if (!directory) {
        syslog(LOG_INFO, "opendir(): \"%s\" (%s))", path, strerror(errno)); 
        return -1;
    }  

    while((fileInfo = readdir(directory)) != NULL) {
        if(strcmp(fileInfo->d_name, ".") == 0 || strcmp(fileInfo->d_name, "..") == 0) {
            continue;
        }
        
        if(fileInfo->d_type == DT_REG) {
            if(remove(AppendToPath(path, fileInfo->d_name)) == -1) {
                syslog(LOG_INFO, "remove(): \"%s\" (%s)", AppendToPath(path, fileInfo->d_name), strerror(errno));                             
                return -1;
            }
        }
        else if(fileInfo->d_type == DT_DIR){
            if(RemoveDirectory(AppendToPath(path, fileInfo->d_name)) == -1) {
                syslog(LOG_INFO, "RemoveDirectory(): Could not remove %s", AppendToPath(path, fileInfo->d_name));    
                return -1;
            }
        }
    }

    if(remove(path) == -1) {
        syslog(LOG_INFO, "remove(): \"%s\" (%s)", path, strerror(errno));
        return -1;
    }

    if(closedir(directory) == -1) {
        syslog(LOG_INFO, "closedir(): \"%s\" (%s)", path, strerror(errno));
        return -1;
    }

    return 0;
}

void Daemonize() { // tu raczej nie powinno być perrorów, tylko syslogi
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
        if(close(i) == -1) {
            syslog(LOG_INFO, "%s", strerror(errno));   
            exit(EXIT_FAILURE);        
        }
    }

    openlog(NULL, LOG_USER, LOG_USER);
    syslog(LOG_INFO, "App started");

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

int SynchronizeDirectories(const char *srcPath, const char *destPath) {
    DIR *source = NULL; 
    DIR *destination = NULL;  

    struct dirent *srcFileInfo = NULL; 
    struct dirent *destFileInfo = NULL;

    List *srcDirFiles = InitList();
    List *destDirFiles = InitList();
    List *srcDirectories = InitList();

    Node *nodePtr = malloc(sizeof(struct node));    

    source = opendir(srcPath);
    if (!source) {
        syslog(LOG_INFO, "opendir(): \"%s\" (%s)", srcPath, strerror(errno));

        free(srcDirFiles);
        free(destDirFiles); 
        free(nodePtr);

        return -1; 
    }
 
    destination = opendir(destPath); 
    if (!destination) {
        if(errno == ENOENT && recursiveSearch) {
            if(CopyDirectory(source, srcPath, destPath) == -1) {
                syslog(LOG_INFO, "CopyDirectory(): Could not copy \"%s\" to \"%s\"", srcPath, destPath);   

                free(srcDirFiles);
                free(destDirFiles); 
                free(nodePtr);

                return -1;
            }

            return 0;
        }
        else {
            syslog(LOG_INFO, "opendir(): \"%s\" (%s)", destPath, strerror(errno)); 

            free(srcDirFiles);
            free(destDirFiles); 
            free(nodePtr);

            return -1;             
        }
    }

    while ((srcFileInfo = readdir(source)) != NULL) {
        if(strcmp(srcFileInfo->d_name, ".") == 0 || strcmp(srcFileInfo->d_name, "..") == 0) {
            continue;
        }

        if(srcFileInfo->d_type == DT_REG){
            Append(srcFileInfo->d_name, srcDirFiles);
        }
        else if(srcFileInfo->d_type == DT_DIR && recursiveSearch) {
            Append(srcFileInfo->d_name, srcDirectories);

            if(SynchronizeDirectories(AppendToPath(srcPath, srcFileInfo->d_name), AppendToPath(destPath, srcFileInfo->d_name)) == -1) {
                syslog(LOG_INFO, "SynchronizeDirectories(): Could not synchronize \"%s\" and \"%s\"", AppendToPath(srcPath, srcFileInfo->d_name), AppendToPath(destPath, srcFileInfo->d_name)); 

                free(srcDirFiles);
                free(destDirFiles); 
                free(nodePtr);       

                return -1;                 
            }
        }
    }
     
    while((destFileInfo = readdir(destination)) != NULL) {   
        if(destFileInfo->d_type == DT_REG){
            Append(destFileInfo->d_name, destDirFiles);
        }
        else if(destFileInfo->d_type == DT_DIR) { // to nie dziala
            if(strcmp(destFileInfo->d_name, ".") == 0 || strcmp(destFileInfo->d_name, "..") == 0) {
                continue;
            }
            
            //syslog(LOG_INFO, "znaleziono katalog \"%s\"", destFileInfo->d_name);

            if(Contains(srcDirectories, destFileInfo->d_name) == -1) {
                if(RemoveDirectory(AppendToPath(destPath, destFileInfo->d_name)) == -1) {
                    syslog(LOG_INFO, "RemoveDirectory(): Could not remove directory");                         
                    return -1;
                }
            }
        }
    }

    Node *current = srcDirFiles->head;
    int result;
    while(current != NULL) {
        result = FindAndCopy(destDirFiles, srcPath, destPath, current->filename);
        if(result == 0) {
            nodePtr = current->next;
            RemoveAt(current, srcDirFiles); 
            current = nodePtr;
        }
        else if (result == 1) {
            syslog(LOG_INFO, "FindAndCopy(): Could not copy files");     

            free(srcDirFiles);
            free(destDirFiles); 
            free(nodePtr);

            return -1;
        }
        else {
            current = current->next;
        }
    }

    if(CopyAllFilesFromList(srcDirFiles, srcPath, destPath) == -1) {
        syslog(LOG_INFO, "CopyAllFilesFromList(): Could not copy files.");

        free(srcDirFiles);
        free(destDirFiles); 
        free(nodePtr);

        return -1;
    }

    if(RemoveAllFilesFromList(destDirFiles, destPath) == -1) {
        syslog(LOG_INFO, "RemoveAllFilesFromList(): Could not remove files");

        free(srcDirFiles);
        free(destDirFiles); 
        free(nodePtr);

        return -1;
    }

    if(closedir(source) == -1) {
        syslog(LOG_INFO, "\"%s\" (%s)", srcPath, strerror(errno));

        free(srcDirFiles);
        free(destDirFiles); 
        free(nodePtr);

        return -1;
    }

    if(closedir(destination) == -1) {
        syslog(LOG_INFO, "\"%s\" (%s)", destPath, strerror(errno));        

        free(srcDirFiles);
        free(destDirFiles); 
        free(nodePtr);

        return -1;
    }

    free(srcDirFiles);
    free(destDirFiles); 
    free(nodePtr); 
    
    return 0;
}

void SignalHandler(int signo) {
    switch(signo) {
        case SIGUSR1:
            syslog(LOG_INFO, "SIGUSR1: Daemon was awakened by user");
            break;
        case SIGTERM:
            syslog(LOG_INFO, "SIGTERM: Daemon was terminated by user");
            break;               
    }
}

int main(int argc, char *const argv[]) {
    const char *srcPath = argv[1]; 
    const char *destPath = argv[2]; 

    // struct stat *srcDirInfo = malloc(sizeof(struct stat));
    // struct stat *destDirInfo = malloc(sizeof(struct stat));
    struct stat srcDirInfo;
    struct stat destDirInfo;  

    if (signal(SIGUSR1, &SignalHandler) == SIG_ERR) {
        perror("signal()");
        exit(EXIT_FAILURE); 
    }

    if (signal(SIGTERM, &SignalHandler) == SIG_ERR) {
        perror("signal()");
        exit(EXIT_FAILURE); 
    }

    unsigned int argument;
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
                exit(EXIT_FAILURE);                 
        }
    }

    if(stat(srcPath, &srcDirInfo) == -1) {
        perror("stat");
        exit(EXIT_FAILURE);
    }
    
    if(stat(destPath, &destDirInfo) == -1) {
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
        if(SynchronizeDirectories(srcPath, destPath) == -1) {
            syslog(LOG_INFO, "SynchronizeDirectories(): An error has occured. Process has been terminated.");
            exit(EXIT_FAILURE);                 
        }
        
        syslog(LOG_INFO, "Daemon went to sleep for %d seconds", sleepInterval);
        sleep(sleepInterval); 
    }

    // free(srcDirInfo);
    // free(destDirInfo);
    exit(EXIT_SUCCESS); 
}