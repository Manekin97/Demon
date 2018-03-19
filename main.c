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

int fileSizeThreshold = 4096; 
int sleepInterval = 300; 
bool recursiveSearch = false; 

//  @TODO
//  Poprawić syslogi
//  Zostało jeszcze parę wycieków pamieci (Przy AppendToPath i listy(8B chuj wie czemu))

/*Funkcja, która pobiera informacje o pliku *path i zwraca wskaźnik na struct stat*/
struct stat *GetFileInfo(const char *path) {
    struct stat *fileInfo = malloc(sizeof(struct stat));
    
    if (stat(path, fileInfo) == -1) {
        syslog(LOG_INFO, "stat(): %s (%s)", path, strerror(errno));
        return NULL;
    }

    return fileInfo;
}

/*Funkcja, która ustawia czas modyfikacji do pliku *destPath zgodnie z *fileInfo i czas dostępu na aktualny czas*/
int SyncModTime(struct stat *fileInfo, const char *destPath) {
    struct utimbuf newTime;
    
    newTime.actime = time(NULL);    //  Ustaw datę doestępu na datę teraźniejszą
    newTime.modtime = fileInfo->st_mtime;   //  Ustaw datę modyfiakcji na datę modyfikacji pliku źródłowego
    if (utime(destPath, &newTime) == -1) {
        syslog(LOG_INFO, "utime(): %s", strerror(errno));
        return -1;
    }
 
    return 0;
}

/*Funkcja kopiująca plik *srcPath do *destPath, używając odwzorowania w pamięci*/
int MmapCopy(const char *srcPath, const char *destPath) {
    struct stat fileInfo; 

    int source = open(srcPath, O_RDONLY); 
    if (source == -1) {
        return -1; 
    }

    off_t fileSize; 
    if (fstat(source, &fileInfo) == -1) {   //  Pobierz informację o pliku źródłowym
        return -1; 
    }
    else {
        fileSize = fileInfo.st_size; 
    }

    int destination = open(destPath, O_RDWR | O_CREAT, fileInfo.st_mode);
    if (destination == -1) {
        return -1; 
    }

    int *srcAddress = mmap(NULL, fileSize, PROT_READ, MAP_PRIVATE, source, 0);  //  Stwórz odwzorowanie pliku źródłowego w pamięci
    if (srcAddress == MAP_FAILED) {
        return -1; 
    }

    if(ftruncate(destination, fileSize)) {  //  Zmniejsz rozmiar pliku docelowego do rozmiaru pliku źródłowego
        return -1;
    } 

    int *destAddress = mmap(NULL, fileSize, PROT_READ | PROT_WRITE, MAP_SHARED, destination, 0);    //  Stwórz odwzorowanie pliku docelowego w pamięci
    if (destAddress == MAP_FAILED) { 
        return -1; 
    }

    if(memcpy(destAddress, srcAddress, fileSize) == NULL) { //  Skopiuj zawartość odwzorowań
        return -1;
    }
    
    if (munmap(srcAddress, fileSize) == -1) {   //  Usuń odwzorowanie pliku źródłowego
        return -1; 
    }

    if (munmap(destAddress, fileSize) == -1) {   //  Usuń odwzorowanie pliku docelowego
        return -1; 
    }

    if(SyncModTime(&fileInfo, destPath) == -1) {    //  Ustaw datę modyfikacji pliku docelowego na datę modyfikacji pliku źródłowego
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

/*Funkcja kopiująca plik *srcPath do *destPath, używając API Linuxa*/
int RegularCopy(const char *srcPath, const char *destPath) { 
    char buffer[BUFFER_SIZE];
    struct stat fileInfo; 

    int source = open(srcPath, O_RDONLY); 
    if (source == -1) {
        return -1; 
    }

    if (fstat(source, &fileInfo) == -1) {
        return -1; 
    }

    int destination = open(destPath, O_WRONLY | O_CREAT | O_TRUNC, fileInfo.st_mode);
    if (destination == -1) {
        return -1; 
    }

    ssize_t bytesRead; 
    ssize_t bytesWritten; 
    while ((bytesRead = read(source, &buffer, BUFFER_SIZE)) != 0) { //  Dopóki pobrano jakieś dane
        if (bytesRead == -1) {
            if (errno == EINTR) {   //  Jeżeli odczyt został przerwany sygnałem
                continue;   //  Ponów próbę odczytu
            }

            return -1; 
        }

        bytesWritten = write(destination, &buffer, bytesRead);  //  Zapisz bytesRead odczytanych bajtów
        if (bytesWritten == -1) {
            return -1; 
        }

        if (bytesRead != bytesWritten) {    //  Jeżeli zapisano inną ilość bajtów niż odczytano
            return -1; 
        }
    }

    if(SyncModTime(&fileInfo, destPath) == -1) {    // Ustaw datę modyfikacji pliku docelowego na datę modyfikacji pliku źródłowego
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

/*Funkcja kopiująca plik *srcPath do *destPath, na podstawie rozmiaru pliku decyduje w jaki sposób plik zostanie skopiowany*/
int Copy(const char *srcPath, const char *destPath) {
    struct stat *srcFileInfo = GetFileInfo(srcPath);

    if (srcFileInfo->st_size < fileSizeThreshold) { //  Jeżeli rozmiar pliku jest mniejszy niż wartość progowa
        if (RegularCopy(srcPath, destPath) == -1) { //  Skopiuj używając API linuxa
            syslog(LOG_INFO, "RegularCopy(): Could not copy %s to %s", srcPath, destPath);
            return -1;
        }
    }
    else {  //  jeżeli rozmiar pliku jest większy niż wartość progowa
        if (MmapCopy(srcPath, destPath) == -1) {    //  Skopiuj używając odwzorowania w pamięci
            syslog(LOG_INFO, "MmapCopy(): Could not copy %s to %s", srcPath, destPath);
            return -1;
        }
    }

    free(srcFileInfo);
    return 0;
}

/*Funkcja dołącza nazwę pliku *filename do ściezki *path*/
char *AppendToPath(const char *path, char *filename) {
    char *newPath = malloc(PATH_MAX * sizeof(char));
    if(sprintf(newPath, "%s/%s", path, filename) < 0) {
        return NULL;
    }

    return newPath;
}

/*Funkcja kopiuje wszystkie pliki znajdujące się na liście *list do katalogu *destDir*/
int CopyAllFilesFromList(List *list, const char *srcDir, const char *destDir) {
    char *fullSrcFilePath = NULL;
    char *fullDestFilePath = NULL;
    
    Node *current = list->head;
    while(current != NULL) { 
        fullSrcFilePath = AppendToPath(srcDir, current->filename); 
        fullDestFilePath = AppendToPath(destDir, current->filename);          
        
        if(Copy(fullSrcFilePath, fullDestFilePath) == -1) {
            syslog(LOG_INFO, "Copy(): Could not copy"); 

            free(fullSrcFilePath);
            free(fullDestFilePath);

            return -1;
        }

        syslog(LOG_INFO, "%s has been copied to %s", fullSrcFilePath, fullDestFilePath); 

        free(fullSrcFilePath);
        free(fullDestFilePath);
        current = current->next;                       
    }

    return 0;
}

/*Funkcja usuwa wszystkie pliki znajdujące się na liście *list z katalogu *path*/
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

/*Funkcja porównuje czas modyfikacji pliku *srcPath oraz *destPath*/
int CompareModTime(const char *srcPath, const char *destPath) {
    struct stat *srcFileInfo = NULL;
    struct stat *destFileInfo = NULL;

    srcFileInfo = GetFileInfo(srcPath);
    destFileInfo = GetFileInfo(destPath);

    if (srcFileInfo->st_mtime > destFileInfo->st_mtime) {   //  Jeżeli plik źródłowy był modyfikowany później niz plik docelowy
        free(srcFileInfo);
        free(destFileInfo);

        return 1;
    }
    else if (srcFileInfo->st_mtime == destFileInfo->st_mtime) {   //  Jeżeli plik źródłowy i docelowy mają tą samą datę modyfikacji
        free(srcFileInfo);
        free(destFileInfo);

        return 0;
    }
    else {  //  Jeżeli plik źródłowy był modyfikowany wcześniej niz plik docelowy
        free(srcFileInfo);
        free(destFileInfo);

        return -1;
    }
}

/*Funkcja znajduje plik w liście *list o nazwie *filename i kopiuje go do *destPath jeżeli jego czas modyfikacji różni się od czasu modyfikacji pliku *srcPath*/
int FindAndCopy(List *list, const char *srcPath, const char *destPath, char *filename) {
    char *fullSrcFilePath = NULL;
    char *fullDestFilePath = NULL;

    Node *current = list->head;
    while(current != NULL) {
        if(strcmp(current->filename, filename) == 0) {  //  Jeżeli w liście istnieje plik o nazwie filename 
            fullSrcFilePath = AppendToPath(srcPath, filename);
            fullDestFilePath = AppendToPath(destPath, filename);
            
            if(CompareModTime(fullSrcFilePath, fullDestFilePath) != 0) {    //  Jeżeli czas modyfikacji się różni
                if(Copy(fullSrcFilePath, fullDestFilePath) == -1) { //  To skopiuj
                    syslog(LOG_INFO, "Copy(): Could not copy \"%s\" to \"%s\"", fullSrcFilePath, fullDestFilePath); 
                    RemoveAt(current, list);    //  Usuń z listy

                    free(fullSrcFilePath);
                    free(fullDestFilePath);

                    return 1;
                }
            }             

            RemoveAt(current, list);    //  Usuń  listy

            free(fullSrcFilePath);
            free(fullDestFilePath);

            return 0;
        }
        else {
            current = current->next;
        }
    }

    free(fullSrcFilePath);
    free(fullDestFilePath);
    
    return -1;
}

/*Funkcja rekurencyjnie kopiuje katalog *path wraz z jego plikami oraz podkatalogami*/
int CopyDirectory(const char *srcPath, const char *destPath) {
    struct dirent *srcFileInfo = NULL;
    DIR *source = NULL;

    struct stat *fileInfo = GetFileInfo(srcPath);

    if(mkdir(destPath, fileInfo->st_mode) == -1) {
        syslog(LOG_INFO, "mkdir(): \"%s\" (%s)", destPath, strerror(errno));
        free(fileInfo);
        return -1;
    } 

    source = opendir(srcPath);
    if (!source) {
        syslog(LOG_INFO, "opendir(): \"%s\" (%s)", srcPath, strerror(errno)); 
        free(fileInfo);
        return -1;
    }  

    while((srcFileInfo = readdir(source)) != NULL) {
        if(strcmp(srcFileInfo->d_name, ".") == 0 || strcmp(srcFileInfo->d_name, "..") == 0) {   //  Pomiń katalogi "." i ".."
            continue;
        }

        if(srcFileInfo->d_type == DT_REG) { //  Jeżeli jest zwykłym plikiem
            if(Copy(AppendToPath(srcPath, srcFileInfo->d_name), AppendToPath(destPath, srcFileInfo->d_name)) == -1) {   //  Skopiuj
                syslog(LOG_INFO, "Copy(): \"%s\" could not be copied", AppendToPath(srcPath, srcFileInfo->d_name));      
                free(fileInfo);                           
                return -1;
            }            
        }
        else if(srcFileInfo->d_type == DT_DIR) {    //  Jeżeli jest katalogiem
            if(CopyDirectory(AppendToPath(srcPath, srcFileInfo->d_name), AppendToPath(destPath, srcFileInfo->d_name)) == -1) {  // Rekrurencyjnie skopiuj katalog wraz z jego podkatalogami i plikami
                syslog(LOG_INFO, "CopyDirectory(): Could not copy \"%s\" to \"%s\"", srcPath, destPath);              
                free(fileInfo);     
                return -1;
            }
        }
    }

    if(closedir(source) == -1) {
        syslog(LOG_INFO, "closedir(): \"%s\" (%s)", srcPath, strerror(errno));
        free(fileInfo);
        return -1;
    }

    free(fileInfo);  
    return 0;
}

/*Funkcja rekurencyjnie usuwa katalog *path wraz z jego plikami oraz podkatalogami*/
int RemoveDirectory(const char *path) {
    DIR *directory = NULL;
    struct dirent *fileInfo = NULL;

    directory = opendir(path); 
    if (!directory) {
        syslog(LOG_INFO, "opendir(): \"%s\" (%s))", path, strerror(errno)); 
        return -1;
    }  

    while((fileInfo = readdir(directory)) != NULL) {
        if(strcmp(fileInfo->d_name, ".") == 0 || strcmp(fileInfo->d_name, "..") == 0) { //  Pomiń katalogi "." i ".."
            continue;
        }
        
        if(fileInfo->d_type == DT_REG) {    //  Jeżeli jest plikiem
            if(remove(AppendToPath(path, fileInfo->d_name)) == -1) {    // Usuń plik
                syslog(LOG_INFO, "remove(): \"%s\" (%s)", AppendToPath(path, fileInfo->d_name), strerror(errno));                             
                return -1;
            }
        }
        else if(fileInfo->d_type == DT_DIR){    //  Jeżeli jest katalogiem
            if(RemoveDirectory(AppendToPath(path, fileInfo->d_name)) == -1) {   //  Rekurencyjnie usuń podkatalog wraz z jego podkatalogami
                syslog(LOG_INFO, "RemoveDirectory(): Could not remove %s", AppendToPath(path, fileInfo->d_name));    
                return -1;
            }
        }
    }

    if(remove(path) == -1) {    //  Usuń katalog
        syslog(LOG_INFO, "remove(): \"%s\" (%s)", path, strerror(errno));
        return -1;
    }

    if(closedir(directory) == -1) {
        syslog(LOG_INFO, "closedir(): \"%s\" (%s)", path, strerror(errno));
        return -1;
    }

    return 0;
}

/*Funkcja demonizuje program*/
void Daemonize() {
    pid_t pid; 

    pid = fork(); //    Stwórz nowy proces
    if (pid == -1) {    //  Jeżeli wystąpił błąd
        syslog(LOG_INFO, "fork(): %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    else if (pid > 0) { //  Jeżeli proces jest rodzicem
        exit(EXIT_SUCCESS); //  Zakończ
    }

    if (setsid() == -1) {   //  Stwórz nową sesję
        syslog(LOG_INFO, "setsid(): %s", strerror(errno));        
        exit(EXIT_FAILURE);
    }

    signal(SIGHUP, SIG_IGN);    //  Zignoruj sygnał SIGHUP
    pid = fork();   //  Stwórz nowy proces

    if (pid == -1) {    //  Jeżeli wystąpił błąd
        syslog(LOG_INFO, "fork(): %s", strerror(errno));        
        exit(EXIT_FAILURE);
    }
    else if (pid > 0) { //  Jeżeli proces jest rodzicem
        exit(EXIT_SUCCESS); //  Zakończ
    }

    if (chdir("/") == -1) { //  Zmień aktualny katalog na /
        syslog(LOG_INFO, "chdir(): %s", strerror(errno));        
        exit(EXIT_FAILURE); 
    }

    umask(0);   //  Nadaj maskę do tworzenia plików

    for (int i = 0; i < sysconf(_SC_OPEN_MAX); i++) {   //  Zamknij wszystkie deskryptory
        close(i);
        // if(close(i) == -1) {
        //     syslog(LOG_INFO, "%s", strerror(errno));   
        //     exit(EXIT_FAILURE);        
        // }
    }

    openlog(NULL, LOG_USER, LOG_USER);  //  Otwórz log systemowy
    syslog(LOG_INFO, "App started");

    if (open("/dev/null", O_RDONLY) == -1) {    //  Otwórz STDIN jako /dev/null
        syslog(LOG_INFO, "STDIN could not be opened properly");
        exit(EXIT_FAILURE);
    }
    if (open("/dev/null", O_WRONLY) == -1) {    //  Otwórz STDOUT jako /dev/null
        syslog(LOG_INFO, "STDOUT could not be opened properly");
        exit(EXIT_FAILURE);
    }
    if (open("/dev/null", O_RDWR) == -1) {  //  Otwórz STDERR jako /dev/null
        syslog(LOG_INFO, "STDERR could not be opened properly");
        exit(EXIT_FAILURE);
    }
}

/*Funkcja synchronizuje katalogi *srcPath oraz *destPath*/
int SynchronizeDirectories(const char *srcPath, const char *destPath) {
    DIR *source = NULL; 
    DIR *destination = NULL;  

    struct dirent *srcFileInfo = NULL; 
    struct dirent *destFileInfo = NULL;

    List *srcDirFiles = InitList();
    List *destDirFiles = InitList();
    List *srcDirectories = InitList();
   
    Node *nodePtr;

    source = opendir(srcPath);
    if (!source) {
        syslog(LOG_INFO, "opendir(): \"%s\" (%s)", srcPath, strerror(errno));

        free(srcDirFiles);
        free(destDirFiles); 
        free(srcDirectories);

        return -1; 
    }
 
    destination = opendir(destPath); 
    if (!destination) {
        if(errno == ENOENT && recursiveSearch) {    // Jeżeli nie istnieje katalog *srcPath i włączona jest rekurywna synchronizacja
            if(CopyDirectory(srcPath, destPath) == -1) {
                syslog(LOG_INFO, "CopyDirectory(): Could not copy \"%s\" to \"%s\"", srcPath, destPath);   

                free(srcDirFiles);
                free(destDirFiles); 
                free(srcDirectories);

                return -1;
            }

            return 0;
        }
        else {
            syslog(LOG_INFO, "opendir(): \"%s\" (%s)", destPath, strerror(errno)); 

            free(srcDirFiles);
            free(destDirFiles); 
            free(srcDirectories);

            return -1;             
        }
    }

    /*Wczytaj nazwy plików z folderu źródłowego do listy srcDirFiles*/
    while ((srcFileInfo = readdir(source)) != NULL) {
        if(strcmp(srcFileInfo->d_name, ".") == 0 || strcmp(srcFileInfo->d_name, "..") == 0) {   //  Pomiń katalogi "." i ".."
            continue;
        }

        if(srcFileInfo->d_type == DT_REG){  //  Jeżeli jest zwykłym plikiem
            Append(srcFileInfo->d_name, srcDirFiles);   //  Dołącz do listy
        }
        else if(srcFileInfo->d_type == DT_DIR && recursiveSearch) { //  Jeżeli jest katalogiem i włączona jest opcja rekursywnej synchronizacji
            Append(srcFileInfo->d_name, srcDirectories);    //  Dołącz do listy

            if(SynchronizeDirectories(AppendToPath(srcPath, srcFileInfo->d_name), AppendToPath(destPath, srcFileInfo->d_name)) == -1) { //  Synchronizuj podkatalogi
                syslog(LOG_INFO, "SynchronizeDirectories(): Could not synchronize \"%s\" and \"%s\"", 
                    AppendToPath(srcPath, srcFileInfo->d_name),
                    AppendToPath(destPath, srcFileInfo->d_name)); 

                free(srcDirFiles);
                free(destDirFiles); 
                free(srcDirectories);       

                return -1;                 
            }
        }
    }
     
    /*Wczytaj nazwy plików z folderu docelowego do listy destDirFiles*/
    while((destFileInfo = readdir(destination)) != NULL) {   
        if(destFileInfo->d_type == DT_REG){  //  Jeżeli jest zwykłym plikiem
            Append(destFileInfo->d_name, destDirFiles); //  Dołącz do listy
        }
        else if(destFileInfo->d_type == DT_DIR) {   // Jeżeli jest katalogiem
            if(strcmp(destFileInfo->d_name, ".") == 0 || strcmp(destFileInfo->d_name, "..") == 0) {   //  Pomiń katalogi "." i ".."
                continue;
            }

            if(Contains(srcDirectories, destFileInfo->d_name) == -1) {  //  Jeżeli ten podkatalog nie znajduje się w katalogu źródłowym
                if(RemoveDirectory(AppendToPath(destPath, destFileInfo->d_name)) == -1) {   // To usuń go
                    syslog(LOG_INFO, "RemoveDirectory(): Could not remove directory");                         
                    return -1;
                }
            }
        }
    }

    /*Kopiuje pliki z katalogu źródłowego do katalogu docelowego jeżeli różni się ich data modyfikacji*/
    Node *current = srcDirFiles->head;
    int result;
    while(current != NULL) {
        result = FindAndCopy(destDirFiles, srcPath, destPath, current->filename);
        if(result == 0) {   //  Jeżeli został usunięty
            nodePtr = current->next;
            RemoveAt(current, srcDirFiles); //  usuń z listy
            current = nodePtr;
        }
        else if (result == 1) { //  Jeżeli wystąpił błąd
            syslog(LOG_INFO, "FindAndCopy(): Could not copy files");     

            free(srcDirFiles);
            free(destDirFiles); 
            free(srcDirectories);

            return -1;
        }
        else {  //  Jeżeli pliki mają tą samą datę modyfikacji
            current = current->next;    // Przejdź do kolejnego pliku
        }
    }

    /*Skopiuj wszytkie pliki pozostałe w liście srcDirFiles (czyli pliki, których nie ma w katalogu docelowym, ale sa w źródłowym) do katalogu docelowego*/
    if(CopyAllFilesFromList(srcDirFiles, srcPath, destPath) == -1) {
        syslog(LOG_INFO, "CopyAllFilesFromList(): Could not copy files.");

        free(srcDirFiles);
        free(destDirFiles); 
        free(srcDirectories);

        return -1;
    }

    /*Usuń wszytkie pliki pozostałe w liście destDirFiles (czyli pliki, których nie ma w katalogu źródłowym, ale są w docelowym) z katalogu docelowego*/    
    if(RemoveAllFilesFromList(destDirFiles, destPath) == -1) {
        syslog(LOG_INFO, "RemoveAllFilesFromList(): Could not remove files");

        free(srcDirFiles);
        free(destDirFiles); 
        free(srcDirectories);

        return -1;
    }

    /*Zamknięcie plików i zwolnienie pamięci*/
    if(closedir(source) == -1) {
        syslog(LOG_INFO, "\"%s\" (%s)", srcPath, strerror(errno));

        free(srcDirFiles);
        free(destDirFiles);
        free(srcDirectories); 

        return -1;
    }

    if(closedir(destination) == -1) {
        syslog(LOG_INFO, "\"%s\" (%s)", destPath, strerror(errno));        

        free(srcDirFiles);
        free(destDirFiles);
        free(srcDirectories);

        return -1;
    }

    DestroyList(srcDirFiles);
    DestroyList(destDirFiles);
    DestroyList(srcDirectories);
    
    free(srcDirFiles);
    free(destDirFiles); 
    free(srcDirectories);

    return 0;
}

/*Funkcja jest handlerem sygnałów*/
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

    struct stat srcDirInfo;
    struct stat destDirInfo;  

    if (signal(SIGUSR1, &SignalHandler) == SIG_ERR) {   //  Ustawienie handlera sygnału SIGUSR1
        perror("signal()");
        exit(EXIT_FAILURE); 
    }

    if (signal(SIGTERM, &SignalHandler) == SIG_ERR) {   //  Ustawienie handlera sygnału SIGTERM
        perror("signal()");
        exit(EXIT_FAILURE); 
    }

    /*pobranie opcjonalnych argumentów*/
    unsigned int argument;
    while ((argument = getopt(argc, argv, "Rs:i:")) != -1) {
        switch (argument) {
            case 's':
                fileSizeThreshold = atoi(optarg); 
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

    if(stat(srcPath, &srcDirInfo) == -1) {  //  Pobranie informacji o katalogu srcPath
        perror("stat");
        exit(EXIT_FAILURE);
    }
    
    if(stat(destPath, &destDirInfo) == -1) {  //  Pobranie informacji o katalogu destPath
        perror("stat");
        exit(EXIT_FAILURE);
    }

    if(!S_ISDIR(srcDirInfo.st_mode)) {  //  Sprawdzanie, czy srcPath jest katalogiem
        printf("\"%s\" is not a directory", srcPath);
        exit(EXIT_FAILURE);
    }

    if(!S_ISDIR(destDirInfo.st_mode)) {  //  Sprawdzanie, czy destPath jest katalogiem
        printf("\"%s\" is not a directory", destPath);
        exit(EXIT_FAILURE);
    }

    // Daemonize();       

    syslog(LOG_INFO, "Deamon started, RecursiveSearch=%s, sleepInterval=%ds, fileSizeThreshold=%d", 
        recursiveSearch ? "true" : false, 
        sleepInterval, 
        fileSizeThreshold);

    while (1) { 
        if(SynchronizeDirectories(srcPath, destPath) == -1) {
            syslog(LOG_INFO, "SynchronizeDirectories(): An error has occured. Process has been terminated.");
            exit(EXIT_FAILURE);                 
        }
        
        syslog(LOG_INFO, "Daemon went to sleep for %d seconds", sleepInterval);
        sleep(sleepInterval); 
    }

    exit(EXIT_SUCCESS); 
}