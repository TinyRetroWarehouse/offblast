#define _GNU_SOURCE
#define SCALING 2.0
#define PHI 1.618033988749895

#define SMALL_FONT_SIZE 12
#define COLS_ON_SCREEN 5
#define NAVIGATION_MOVE_DURATION 250 
#define COLS_TOTAL 10 


// * TODO MULTIPLE ROWS
// TODO GRADIENT LAYERS
// TODO ROW NAMES
// TODO PLATFORM IN INFO
// TODO PLATFORM BADGES ON MIXED LISTS
// TODO GRANDIA IS BEING DETECTED AS "D" DETECT BETTER!

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <json-c/json.h>
#include <murmurhash.h>
#include <curl/curl.h>
#include <math.h>

#include "offblast.h"
#include "offblastDbFile.h"

typedef struct UiTile{
    struct LaunchTarget *target;
    struct UiTile *next; 
    struct UiTile *previous; 
} UiTile;

typedef struct UiRow {
    uint32_t length;
    struct UiTile *cursor;
    struct UiTile *tiles;
} UiRow;

typedef struct OffblastUi {
        int32_t winWidth;
        int32_t winHeight;
        int32_t winFold;
        int32_t winMargin;
        int32_t boxWidth;
        int32_t boxHeight;
        int32_t boxPad;
        int32_t descriptionWidth;
        int32_t descriptionHeight;
        double titlePointSize;
        double infoPointSize;

        TTF_Font *titleFont;
        TTF_Font *infoFont;
        TTF_Font *debugFont;

        SDL_Texture *titleTexture;
        SDL_Texture *infoTexture;
        SDL_Texture *descriptionTexture;
        SDL_Renderer *renderer;

        uint32_t rowCursor;
        UiRow *rows;
} OffblastUi;

typedef struct Animation {
    uint32_t animating;
    uint32_t direction;
    uint32_t startTick;
    uint32_t durationMs;
    void *callbackArgs;
    void (* callback)(struct Animation*);
} Animation;

typedef struct InfoFadedCallbackArgs {
    OffblastUi *ui;
    LaunchTarget *newTarget;
    OffblastBlobFile *descriptionFile;
} InfoFadedCallbackArgs;



uint32_t megabytes(uint32_t n);
uint32_t needsReRender(SDL_Window *window, OffblastUi *ui);
double easeOutCirc(double t, double b, double c, double d);
double easeInOutCirc (double t, double b, double c, double d);
char *getCsvField(char *line, int fieldNo);
double goldenRatioLarge(double in, uint32_t exponent);
void horizontalMoveDone(struct Animation *context);
UiTile *rewindTiles(UiTile *fromTile, uint32_t depth);
void infoFaded(struct Animation *context);

void changeColumn(
        Animation *theAnimation, 
        Animation *titleAnimation, 
        uint32_t direction);


SDL_Texture *createTitleTexture(
        SDL_Renderer *renderer,
        TTF_Font *titleFont,
        char *titleString);

SDL_Texture *createInfoTexture(
        SDL_Renderer *renderer,
        TTF_Font *infoFont,
        LaunchTarget *target);

SDL_Texture *createDescriptionTexture(
        SDL_Renderer *renderer,
        TTF_Font *infoFont,
        char *description,
        int32_t width, int32_t height); 



int main (int argc, char** argv) {

    printf("\nStarting up OffBlast with %d args.\n\n", argc);


    char *homePath = getenv("HOME");
    assert(homePath);

    char *configPath;
    asprintf(&configPath, "%s/.offblast", homePath);

    int madeConfigDir;
    madeConfigDir = mkdir(configPath, S_IRWXU);
    
    if (madeConfigDir == 0) {
        printf("Created offblast directory\n");
        // TODO create a config file too
    }
    else {
        switch (errno) {
            case EEXIST:
                break;

            default:
                printf("Couldn't create offblast dir %d\n", errno);
                return errno;
        }
    }

/*
    CURL *curl = curl_easy_init();
    if (!curl) {
        printf("couldn't init curl\n");
        return 1;
    }
*/

    char *configFilePath;
    asprintf(&configFilePath, "%s/config.json", configPath);
    FILE *configFile = fopen(configFilePath, "r");

    if (configFile == NULL) {
        printf("Config file config.json is missing, exiting..\n");
        return 1;
    }

    fseek(configFile, 0, SEEK_END);
    long configSize = ftell(configFile);
    fseek(configFile, 0, SEEK_SET);

    char *configText = calloc(1, configSize + 1);
    fread(configText, 1, configSize, configFile);
    fclose(configFile);


    json_tokener *tokener = json_tokener_new();
    json_object *configObj = NULL;

    configObj = json_tokener_parse_ex(tokener,
            configText,
            configSize);

    assert(configObj);

    json_object *paths = NULL;
    json_object_object_get_ex(configObj, "paths", &paths);

    assert(paths);

    json_object *configForOpenGameDb;
    json_object_object_get_ex(configObj, "opengamedb", 
            &configForOpenGameDb);

    assert(configForOpenGameDb);
    const char *openGameDbPath = 
        json_object_get_string(configForOpenGameDb);

    printf("Found OpenGameDb at %s\n", openGameDbPath);


    char *pathInfoDbPath;
    asprintf(&pathInfoDbPath, "%s/pathinfo.bin", configPath);
    struct OffblastDbFile pathDb = {0};
    if (!init_db_file(pathInfoDbPath, &pathDb, sizeof(PathInfo))) {
        printf("couldn't initialize path db, exiting\n");
        return 1;
    }
    PathInfoFile *pathInfoFile = (PathInfoFile*) pathDb.memory;
    free(pathInfoDbPath);

    char *launchTargetDbPath;
    asprintf(&launchTargetDbPath, "%s/launchtargets.bin", configPath);
    OffblastDbFile launchTargetDb = {0};
    if (!init_db_file(launchTargetDbPath, &launchTargetDb, 
                sizeof(LaunchTarget))) 
    {
        printf("couldn't initialize path db, exiting\n");
        return 1;
    }
    LaunchTargetFile *launchTargetFile = 
        (LaunchTargetFile*) launchTargetDb.memory;
    free(launchTargetDbPath);

    char *descriptionDbPath;
    asprintf(&descriptionDbPath, "%s/descriptions.bin", configPath);
    OffblastDbFile descriptionDb = {0};
    if (!init_db_file(descriptionDbPath, &descriptionDb, 
                sizeof(OffblastBlobFile) + 33333))
    {
        printf("couldn't initialize the descriptions file, exiting\n");
        return 1;
    }
    OffblastBlobFile *descriptionFile = 
        (OffblastBlobFile*) descriptionDb.memory;
    free(descriptionDbPath);


#if 0
    // XXX DEBUG Dump out all launch targets
    for (int i = 0; i < launchTargetFile->nEntries; i++) {
        printf("Reading from local game db\n");
        printf("found game\t%d\t%u\n", 
                i, launchTargetFile->entries[i].targetSignature); 

        printf("%s\n", launchTargetFile->entries[i].name);
        printf("%s\n", launchTargetFile->entries[i].fileName);
        printf("%s\n", launchTargetFile->entries[i].path);
        printf("--\n\n");

    } // XXX DEBUG ONLY CODE
#endif 



    size_t nPaths = json_object_array_length(paths);
    for (int i=0; i<nPaths; i++) {

        json_object *workingPathNode = NULL;
        json_object *workingPathStringNode = NULL;
        json_object *workingPathExtensionNode = NULL;
        json_object *workingPathPlatformNode = NULL;

        const char *thePath = NULL;
        const char *theExtension = NULL;
        const char *thePlatform = NULL;

        workingPathNode = json_object_array_get_idx(paths, i);
        json_object_object_get_ex(workingPathNode, "path",
                &workingPathStringNode);
        json_object_object_get_ex(workingPathNode, "extension",
                &workingPathExtensionNode);
        json_object_object_get_ex(workingPathNode, "platform",
                &workingPathPlatformNode);

        thePath = json_object_get_string(workingPathStringNode);
        theExtension = json_object_get_string(workingPathExtensionNode);
        thePlatform = json_object_get_string(workingPathPlatformNode);

        printf("Running Path for %s: %s\n", theExtension, thePath);

        uint32_t platformScraped = 0;
        for(uint32_t i=0; i < launchTargetFile->nEntries; ++i) {
            if (strcmp(launchTargetFile->entries[i].platform, 
                        thePlatform) == 0) 
            {
                printf("%s already scraped.\n", thePlatform);
                platformScraped = 1;
                break;
            }
        }

        if (!platformScraped) {

            char *openGameDbPlatformPath;
            asprintf(&openGameDbPlatformPath, "%s/%s.csv", openGameDbPath, 
                    thePlatform);
            printf("Looking for file %s\n", openGameDbPlatformPath);

            FILE *openGameDbFile = fopen(openGameDbPlatformPath, "r");
            if (openGameDbFile == NULL) {
                printf("looks like theres no opengamedb for the platform\n");
                free(openGameDbPlatformPath);
                break;
            }
            free(openGameDbPlatformPath);
            openGameDbPlatformPath = NULL;

            char *csvLine = NULL;
            size_t csvLineLength = 0;
            size_t csvBytesRead = 0;
            uint32_t onRow = 0;

            while ((csvBytesRead = getline(
                            &csvLine, &csvLineLength, openGameDbFile)) != -1) 
            {
                if (onRow > 0) {

                    char *gameName = getCsvField(csvLine, 1);
                    char *gameSeed;

                    asprintf(&gameSeed, "%s_%s", thePlatform, gameName);

                    uint32_t targetSignature = 0;

                    lmmh_x86_32(gameSeed, strlen(gameSeed), 33, 
                            &targetSignature);

                    int32_t indexOfEntry = launchTargetIndexByTargetSignature(
                            launchTargetFile, targetSignature);

                    if (indexOfEntry == -1) {

                        char *gameDate = getCsvField(csvLine, 2);
                        char *scoreString = getCsvField(csvLine, 3);
                        char *metaScoreString = getCsvField(csvLine, 4);
                        char *description = getCsvField(csvLine, 6);

                        printf("\n%s\n%u\n%s\n%s\ng: %s\n\nm: %s\n", 
                                gameSeed, 
                                targetSignature, 
                                gameName, 
                                gameDate,
                                scoreString, metaScoreString);

                        LaunchTarget *newEntry = 
                            &launchTargetFile->entries[launchTargetFile->nEntries];
                        printf("writing new game to %p\n", newEntry);

                        newEntry->targetSignature = targetSignature;

                        memcpy(&newEntry->name, 
                                gameName, 
                                strlen(gameName));

                        memcpy(&newEntry->platform, 
                                thePlatform,
                                strlen(thePlatform));

                        // TODO harden
                        if (strlen(gameDate) != 10) {
                            printf("INVALID DATE FORMAT\n");
                        }
                        else {
                            memcpy(&newEntry->date, gameDate, 10);
                        }

                        float score = -1;
                        if (strlen(scoreString) != 0) {
                            score = atof(scoreString) * 2 * 10;
                        }
                        if (strlen(metaScoreString) != 0) {
                            if (score == -1) {
                                score = atof(metaScoreString);
                            }
                            else {
                                score = (score + atof(metaScoreString)) / 2;
                            }
                        }

                        // XXX TODO check we have enough space to write
                        // the description into the file
                        OffblastBlob *newDescription = (OffblastBlob*) 
                            &descriptionFile->memory[descriptionFile->cursor];

                        newDescription->targetSignature = targetSignature;
                        newDescription->length = strlen(description);

                        memcpy(&newDescription->content, description, 
                                strlen(description));
                        *(newDescription->content + strlen(description)) = '\0';

                        newEntry->descriptionOffset = descriptionFile->cursor;
                        
                        descriptionFile->cursor += 
                            sizeof(OffblastBlob) + strlen(description) + 1;


                        // TODO round properly
                        newEntry->ranking = (uint32_t) score;

                        // TODO check we have the space for it
                        launchTargetFile->nEntries++;

                        free(gameDate);
                        free(scoreString);
                        free(metaScoreString);
                        free(description);

                    }
                    else {
                        printf("%d index found, We already have %u:%s\n", 
                                indexOfEntry,
                                targetSignature, 
                                gameSeed);
                    }

                    free(gameSeed);
                    free(gameName);
                }

                onRow++;
            }
            free(csvLine);
            fclose(openGameDbFile);
        }


        DIR *dir = opendir(thePath);
        // TODO NFS shares when unavailable just lock this up!
        if (dir == NULL) {
            printf("Path %s failed to open\n", thePath);
            break;
        }

        unsigned int nEntriesToAlloc = 10;
        unsigned int nAllocated = nEntriesToAlloc;

        void *fileNameBlock = calloc(nEntriesToAlloc, 256);
        char (*matchingFileNames)[256] = fileNameBlock;

        int numItems = 0;
        struct dirent *currentEntry;

        while ((currentEntry = readdir(dir)) != NULL) {

            char *ext = strrchr(currentEntry->d_name, '.');

            if (ext && strcmp(ext, theExtension) == 0){

                memcpy(matchingFileNames + numItems, 
                        currentEntry->d_name, 
                        strlen(currentEntry->d_name));

                numItems++;
                if (numItems == nAllocated) {

                    unsigned int bytesToAllocate = nEntriesToAlloc * 256;
                    nAllocated += nEntriesToAlloc;

                    void *newBlock = realloc(fileNameBlock, 
                            nAllocated * 256);

                    if (newBlock == NULL) {
                        printf("failed to reallocate enough ram\n");
                        return 0;
                    }

                    fileNameBlock = newBlock;
                    matchingFileNames = fileNameBlock;

                    memset(
                            matchingFileNames+numItems, 
                            0x0,
                            bytesToAllocate);

                }
            }
        }

        uint32_t contentSignature = 0;
        uint32_t pathSignature = 0;
        lmmh_x86_32(thePath, strlen(thePath), 33, &pathSignature);
        lmmh_x86_32(matchingFileNames, numItems*256, 33, &contentSignature);

        printf("got sig: signature:%u contentsHash:%u\n", pathSignature, 
                contentSignature);

        uint32_t rescrapeRequired = (pathInfoFile->nEntries == 0);

        // This goes through everything we have in the file now
        // We need something to detect whether it's in the file
        uint32_t isInFile = 0;
        for (uint32_t i=0; i < pathInfoFile->nEntries; i++) {
            if (pathInfoFile->entries[i].signature == pathSignature
                    && pathInfoFile->entries[i].contentsHash 
                    != contentSignature) 
            {
                printf("Contents of directory %s have changed!\n", thePath);
                isInFile =1;
                rescrapeRequired = 1;
                break;
            }
            else if (pathInfoFile->entries[i].signature == pathSignature)
            {
                printf("Contents unchanged for: %s\n", thePath);
                isInFile = 1;
                break;
            }
        }

        if (!isInFile) {
            printf("%s isn't in the db, adding..\n", thePath);

            // TODO do we have the allocation to add it?
            //
            pathInfoFile->entries[pathInfoFile->nEntries].signature = 
                pathSignature;
            pathInfoFile->entries[pathInfoFile->nEntries].contentsHash = 
                contentSignature;
            pathInfoFile->nEntries++;

            rescrapeRequired = 1;
        }

        if (rescrapeRequired) {
            void *romData = calloc(1, ROM_PEEK_SIZE);

            for (uint32_t j=0;j<numItems; j++) {

                char *romPathTrimmed; 
                asprintf(&romPathTrimmed, "%s/%s", 
                        thePath,
                        matchingFileNames[j]);

                // TODO check it's not disc 2 or 3 etc

                uint32_t romSignature;
                FILE *romFd = fopen(romPathTrimmed, "rb");
                if (! romFd) {
                    printf("cannot open from rom\n");
                }

                if (!fread(romData, ROM_PEEK_SIZE, 1, romFd)) {
                    printf("cannot read from rom\n");
                }
                else {
                    lmmh_x86_32(romData, ROM_PEEK_SIZE, 33, &romSignature);
                    memset(romData, 0x0, ROM_PEEK_SIZE);
                    printf("signature is %u\n", romSignature);
                }

                memset(romData, 0x0, ROM_PEEK_SIZE);
                fclose(romFd);

                // Now we have the signature we can add it to our DB
                int32_t indexOfEntry = launchTargetIndexByRomSignature(
                        launchTargetFile, romSignature);

                if (indexOfEntry > -1) {
                    printf("this target is already in the db\n");
                }
                else {

                    indexOfEntry = launchTargetIndexByNameMatch(
                            launchTargetFile, matchingFileNames[j]);

                    printf("found by name at index %d\n", indexOfEntry);

                    if (indexOfEntry > -1) {

                        LaunchTarget *theTarget = 
                            &launchTargetFile->entries[indexOfEntry];

                        theTarget->romSignature = romSignature;

                        memcpy(&theTarget->fileName, 
                                &matchingFileNames[j], 
                                strlen(matchingFileNames[j]));

                        memcpy(&theTarget->path, 
                                romPathTrimmed,
                                strlen(romPathTrimmed));
                    
                    }
                }

                free(romPathTrimmed);
            }; 

            free(romData);
        }

        matchingFileNames = NULL;
        free(fileNameBlock);
        closedir(dir);
    }

    close(pathDb.fd);
    close(launchTargetDb.fd);



    const char *userName = NULL;
    {
        json_object *usersObject = NULL;
        json_object_object_get_ex(configObj, "users", &usersObject);

        if (usersObject == NULL) {
            userName = "Anonymous";
        }
        else {
            json_object *tmp = json_object_array_get_idx(usersObject, 0);
            assert(tmp);
            userName = json_object_get_string(tmp);
        }
    }

    printf("got user name %s\n", userName);



    // XXX START SDL HERE

    



    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("SDL initialization Failed, exiting..\n");
        return 1;
    }

    if (TTF_Init() == -1) {
        printf("TTF initialization Failed, exiting..\n");
        return 1;
    }

    // Let's create the window
    SDL_Window* window = SDL_CreateWindow("OffBlast", 
            SDL_WINDOWPOS_UNDEFINED, 
            SDL_WINDOWPOS_UNDEFINED,
            640,
            480,
            SDL_WINDOW_FULLSCREEN_DESKTOP | 
                SDL_WINDOW_ALLOW_HIGHDPI);

    if (window == NULL) {
        printf("SDL window creation failed, exiting..\n");
        return 1;
    }

    OffblastUi *ui = calloc(1, sizeof(OffblastUi));
    needsReRender(window, ui);

    ui->renderer = SDL_CreateRenderer(window, -1, 
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
            );


    ui->titlePointSize = goldenRatioLarge(ui->winWidth, 7);
    ui->titleFont = TTF_OpenFont(
            "fonts/Roboto-Regular.ttf", ui->titlePointSize);
    if (!ui->titleFont) {
        printf("Title font initialization Failed, %s\n", TTF_GetError());
        return 1;
    }

    ui->infoPointSize = goldenRatioLarge(ui->winWidth, 9);
    ui->infoFont = TTF_OpenFont(
            "fonts/Roboto-Regular.ttf", ui->infoPointSize);

    if (!ui->infoFont) {
        printf("Font initialization Failed, %s\n", TTF_GetError());
        return 1;
    }

    ui->debugFont = TTF_OpenFont(
            "fonts/Roboto-Regular.ttf", goldenRatioLarge(ui->winWidth, 11));

    if (!ui->debugFont) {
        printf("Font initialization Failed, %s\n", TTF_GetError());
        return 1;
    }

    Animation *theAnimation = calloc(1, sizeof(Animation));
    Animation *titleAnimation = calloc(1, sizeof(Animation));

    InfoFadedCallbackArgs *infoFadedArgs = 
        calloc(1, sizeof(InfoFadedCallbackArgs));

    infoFadedArgs->ui = ui;
    infoFadedArgs->newTarget = NULL;
    infoFadedArgs->descriptionFile = descriptionFile;

    titleAnimation->callbackArgs = infoFadedArgs;

    int running = 1;
    uint32_t lastTick = SDL_GetTicks();
    uint32_t renderFrequency = 1000/60;

    // Init Ui
    // Ok at the minute we have a single row.. let's make it two
    // 1. Your Library
    // 2. Essential Playstation
    ui->rows = calloc(2, sizeof(UiRow));
    UiRow *rows = ui->rows;



    // PREP your library
    // walk through the targets and grab out anything that has 
    // a filepath
    // TODO put a limit on this
#define ROW_INDEX_LIBRARY 0
#define ROW_INDEX_TOP_RATED 1
    uint32_t libraryLength = 0;
    for (uint32_t i = 0; i < launchTargetFile->nEntries; i++) {
        LaunchTarget *target = &launchTargetFile->entries[i];
        if (strlen(target->fileName) != 0) 
            libraryLength++;
    }
    rows[ROW_INDEX_LIBRARY].length = libraryLength; 
    rows[ROW_INDEX_LIBRARY].tiles = calloc(libraryLength, sizeof(UiTile)); 
    assert(rows[ROW_INDEX_LIBRARY].tiles);
    rows[ROW_INDEX_LIBRARY].cursor = &rows[ROW_INDEX_LIBRARY].tiles[0];
    for (uint32_t i = 0, j = 0; i < launchTargetFile->nEntries; i++) {
        LaunchTarget *target = &launchTargetFile->entries[i];
        if (strlen(target->fileName) != 0) {
            rows[ROW_INDEX_LIBRARY].tiles[j].target = target;
            if (j+1 == libraryLength) {
                rows[ROW_INDEX_LIBRARY].tiles[j].next = 
                    &rows[ROW_INDEX_LIBRARY].tiles[0];
            }
            else {
                rows[ROW_INDEX_LIBRARY].tiles[j].next = 
                    &rows[ROW_INDEX_LIBRARY].tiles[j+1];
            }
           if (j==0) {
               rows[ROW_INDEX_LIBRARY].tiles[j].previous = 
                   &rows[ROW_INDEX_LIBRARY].tiles[libraryLength -1];
           }
           else {
               rows[ROW_INDEX_LIBRARY].tiles[j].previous 
                   = &rows[ROW_INDEX_LIBRARY].tiles[j-1];
           }
           j++;
        }
    }


    // PREP essential PS1
    theAnimation->callbackArgs = ui; // TODO this is now a mess
    uint32_t topRatedLength = 9;
    rows[ROW_INDEX_TOP_RATED].length = topRatedLength;
    rows[ROW_INDEX_TOP_RATED].tiles = calloc(topRatedLength, sizeof(UiTile));
    assert(rows[ROW_INDEX_TOP_RATED].tiles);
    rows[ROW_INDEX_TOP_RATED].cursor = &rows[ROW_INDEX_TOP_RATED].tiles[0];
    for (uint32_t i = 0; i < rows[ROW_INDEX_TOP_RATED].length; i++) {
        rows[ROW_INDEX_TOP_RATED].tiles[i].target = 
            &launchTargetFile->entries[i];

        if (i+1 == rows[ROW_INDEX_TOP_RATED].length) {
            rows[ROW_INDEX_TOP_RATED].tiles[i].next = 
                &rows[ROW_INDEX_TOP_RATED].tiles[0]; 
        }
        else {
            rows[ROW_INDEX_TOP_RATED].tiles[i].next = 
                &rows[ROW_INDEX_TOP_RATED].tiles[i+1]; 
        }

        if (i == 0) {
            rows[ROW_INDEX_TOP_RATED].tiles[i].previous = 
                &rows[ROW_INDEX_TOP_RATED].tiles[topRatedLength-1];
        }
        else {
            rows[ROW_INDEX_TOP_RATED].tiles[i].previous = 
                &rows[ROW_INDEX_TOP_RATED].tiles[i-1];
        }
    }

    ui->titleTexture = createTitleTexture(
            ui->renderer,
            ui->titleFont,
            rows[0].cursor->target->name);

    ui->infoTexture = createInfoTexture(
            ui->renderer,
            ui->infoFont,
            rows[0].cursor->target
    );

    OffblastBlob *descriptionBlob = (OffblastBlob*)
        &descriptionFile->memory[rows[0].cursor->target->descriptionOffset];
    ui->descriptionTexture = createDescriptionTexture(
            ui->renderer,
            ui->infoFont,
            descriptionBlob->content,
            ui->descriptionWidth,
            ui->descriptionHeight
    );


    while (running) {

        if (needsReRender(window, ui) == 1) {
            // TODO something
        }

        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                printf("shutting down\n");
                running = 0;
                break;
            }
            else if (event.type == SDL_KEYUP) {
                SDL_KeyboardEvent *keyEvent = (SDL_KeyboardEvent*) &event;
                if (keyEvent->keysym.scancode == SDL_SCANCODE_ESCAPE) {
                    printf("escape pressed, shutting down.\n");
                    running = 0;
                    break;
                }
                else if (
                        keyEvent->keysym.scancode == SDL_SCANCODE_DOWN ||
                        keyEvent->keysym.scancode == SDL_SCANCODE_J) 
                {
                    ui->rowCursor++; // TODO for now
                    if (ui->rowCursor >= 2) ui->rowCursor = 1;
                }
                else if (
                        keyEvent->keysym.scancode == SDL_SCANCODE_UP ||
                        keyEvent->keysym.scancode == SDL_SCANCODE_K) 
                {
                    ui->rowCursor--; // TODO for now
                    if (ui->rowCursor < 0) ui->rowCursor = 0;
                }
                else if (
                        keyEvent->keysym.scancode == SDL_SCANCODE_RIGHT ||
                        keyEvent->keysym.scancode == SDL_SCANCODE_L) 
                {
                    infoFadedArgs->newTarget
                        = rows[ui->rowCursor].cursor->next->target;
                    changeColumn(theAnimation, titleAnimation, 1);
                }
                else if (
                        keyEvent->keysym.scancode == SDL_SCANCODE_LEFT ||
                        keyEvent->keysym.scancode == SDL_SCANCODE_H) 
                {
                    infoFadedArgs->newTarget
                        = rows[ui->rowCursor].cursor->previous->target;
                    changeColumn(theAnimation, titleAnimation, 0);
                }
                else {
                    printf("key up %d\n", keyEvent->keysym.scancode);
                }
            }

        }

        SDL_SetRenderDrawColor(ui->renderer, 0x03, 0x03, 0x03, 0xFF);
        SDL_RenderClear(ui->renderer);
        

        // Title 
        if (titleAnimation->animating == 1) {
            uint8_t change = easeInOutCirc(
                        (double)SDL_GetTicks() - titleAnimation->startTick,
                        1.0,
                        255.0,
                        (double)titleAnimation->durationMs);

            if (titleAnimation->direction == 0) {
                change = 256 - change;
            }
            else {
                if (change == 0) change = 255;
            }

            SDL_SetTextureAlphaMod(ui->titleTexture, change);
            SDL_SetTextureAlphaMod(ui->infoTexture, change);
            SDL_SetTextureAlphaMod(ui->descriptionTexture, change);
        }
        else {
            SDL_SetTextureAlphaMod(ui->titleTexture, 255);
            SDL_SetTextureAlphaMod(ui->infoTexture, 255);
            SDL_SetTextureAlphaMod(ui->descriptionTexture, 255);
        }

        SDL_Rect titleRect = {0, 0, 0, 0}; 
        SDL_QueryTexture(ui->titleTexture, NULL, NULL, 
                &titleRect.w, &titleRect.h);

        SDL_Rect infoRect = {0, 0, 0, 0}; 
        SDL_QueryTexture(ui->infoTexture, NULL, NULL, 
                &infoRect.w, &infoRect.h);

        SDL_Rect descRect = {0, 0, 0, 0}; 
        SDL_QueryTexture(ui->descriptionTexture, NULL, NULL, 
                &descRect.w, &descRect.h);

        titleRect.x = ui->winMargin;
        infoRect.x = ui->winMargin;
        descRect.x = ui->winMargin;

        titleRect.y = goldenRatioLarge((double) ui->winHeight, 5);
        infoRect.y = (titleRect.y + 
            ui->titlePointSize + 
            goldenRatioLarge((double) ui->titlePointSize, 2));
        descRect.y = (infoRect.y + 
            ui->infoPointSize + 
            goldenRatioLarge((double) ui->infoPointSize, 2));

        SDL_RenderCopy(ui->renderer, ui->titleTexture, NULL, &titleRect);
        SDL_RenderCopy(ui->renderer, ui->infoTexture, NULL, &infoRect);
        SDL_RenderCopy(ui->renderer, ui->descriptionTexture, NULL, &descRect);



        // Blocks
        SDL_SetRenderDrawColor(ui->renderer, 0xFF, 0xFF, 0xFF, 0x66);

        for (uint32_t onRow = 0; onRow < 2; onRow++) {

            SDL_Rect rowRects[COLS_TOTAL];

            UiTile *tileToRender = 
                rewindTiles(rows[onRow].cursor, COLS_ON_SCREEN);

            for (int32_t i = -COLS_ON_SCREEN; i < COLS_TOTAL; i++) {

                rowRects[i].x = 
                    ui->winMargin + i * (ui->boxWidth + ui->boxPad);


                if (theAnimation->animating != 0 && onRow == ui->rowCursor) {
                    double change = easeInOutCirc(
                            (double)SDL_GetTicks() - theAnimation->startTick,
                            0.0,
                            (double)ui->boxWidth + ui->boxPad,
                            (double)theAnimation->durationMs);

                    if (theAnimation->direction > 0) {
                        change = -change;
                    }

                    rowRects[i].x += change;

                }

                rowRects[i].y = ui->winFold + (onRow * ui->boxHeight);
                if (onRow > 0) {
                    rowRects[i].y += ui->boxPad;
                }

                rowRects[i].w = ui->boxWidth;
                rowRects[i].h = ui->boxHeight;
                SDL_RenderFillRect(ui->renderer, &rowRects[i]);
                tileToRender = tileToRender->next;
            }
        }


        // TODO run this in a callback function
        if (theAnimation->animating && SDL_GetTicks() > 
                theAnimation->startTick + theAnimation->durationMs) 
        {
            theAnimation->animating = 0;
            theAnimation->callback(theAnimation);
        }
        else if (titleAnimation->animating && SDL_GetTicks() > 
                titleAnimation->startTick + titleAnimation->durationMs) 
        {
            titleAnimation->animating = 0;
            titleAnimation->callback(titleAnimation);
        }



        // DEBUG FPS INFO
        uint32_t frameTime = SDL_GetTicks() - lastTick;
        char *fpsString;
        asprintf(&fpsString, "Frame Time: %u", frameTime);
        SDL_Color fpsColor = {255,255,255,255};

        SDL_Surface *fpsSurface = TTF_RenderText_Solid(
                ui->debugFont,
                fpsString,
                fpsColor);

        free(fpsString);

        if (!fpsSurface) {
            printf("Font render failed, %s\n", TTF_GetError());
            return 1;
        }

        SDL_Texture* fpsTexture = SDL_CreateTextureFromSurface(
                ui->renderer, fpsSurface);

        SDL_FreeSurface(fpsSurface);

        SDL_Rect fpsRect = {
            SMALL_FONT_SIZE*SCALING,
            SMALL_FONT_SIZE*SCALING,
            0, 0};

        SDL_QueryTexture(fpsTexture, NULL, NULL, &fpsRect.w, &fpsRect.h);
        SDL_RenderCopy(ui->renderer, fpsTexture, NULL, &fpsRect);
        SDL_DestroyTexture(fpsTexture);


        SDL_RenderPresent(ui->renderer);

        if (SDL_GetTicks() - lastTick < renderFrequency) {
            SDL_Delay(renderFrequency - (SDL_GetTicks() - lastTick));
        }

        lastTick = SDL_GetTicks();
    }

    free(theAnimation);
    free(titleAnimation);
    free(ui);

    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

double easeOutCirc(double t, double b, double c, double d) 
{
	t /= d;
	t--;
	double change = c * sqrt(1.0f - t*t) + b;
    return change;
};


double easeInOutCirc (double t, double b, double c, double d) {
	t /= d/2.0;
	if (t < 1.0) return -c/2.0 * (sqrt(1.0 - t*t) - 1.0) + b;
	t -= 2.0;
	return c/2.0 * (sqrt(1.0 - t*t) + 1.0) + b;
};

char *getCsvField(char *line, int fieldNo) 
{
    char *cursor = line;
    char *fieldStart = NULL;
    char *fieldEnd = NULL;
    char *fieldString = NULL;
    int inQuotes = 0;

    for (uint32_t i = 0; i < fieldNo; ++i) {

        fieldStart = cursor;
        fieldEnd = cursor;
        inQuotes = 0;

        while (cursor != NULL) {

            if (*cursor == '"') {
                inQuotes++;
            }
            else if (*cursor == ',' && !(inQuotes & 1)) {
                fieldEnd = cursor - 1;
                cursor++;
                break;
            }

            cursor++;
        }
    }

    if (*fieldStart == '"') fieldStart++;
    if (*fieldEnd == '"') fieldEnd--;

    uint32_t fieldLength = (fieldEnd - fieldStart) + 1;

    fieldString = calloc(1, fieldLength + sizeof(char));
    memcpy(fieldString, fieldStart, fieldLength);

    return fieldString;
}

uint32_t needsReRender(SDL_Window *window, OffblastUi *ui) 
{
    int32_t newWidth, newHeight;
    uint32_t updated = 0;

    SDL_GetWindowSize(window, &newWidth, &newHeight);

    if (newWidth != ui->winWidth || newHeight != ui->winHeight) {
        printf("rerendering needed\n");

        ui->winWidth = newWidth;
        ui->winHeight= newHeight;
        ui->winFold = newHeight * 0.5;
        ui->winMargin = goldenRatioLarge((double) newWidth, 5);

        ui->boxWidth = newWidth / COLS_ON_SCREEN;
        ui->boxHeight = 500; // TODO use golden
        ui->boxPad = goldenRatioLarge((double) ui->winWidth, 9);

        ui->descriptionWidth = 
            goldenRatioLarge((double) newWidth, 1) - ui->winMargin;

        // TODO find a better limit
        ui->descriptionHeight = 400;

        updated = 1;
    }

    return updated;
}

void changeColumn(
        Animation *theAnimation, 
        Animation *titleAnimation,
        uint32_t direction) 
{
    if (theAnimation->animating == 0)  // TODO some kind of input lock
    {
        theAnimation->startTick = SDL_GetTicks();
        theAnimation->direction = direction;
        theAnimation->durationMs = NAVIGATION_MOVE_DURATION;
        theAnimation->animating = 1;
        theAnimation->callback = &horizontalMoveDone;

        titleAnimation->startTick = SDL_GetTicks();
        titleAnimation->direction = 0;
        titleAnimation->durationMs = NAVIGATION_MOVE_DURATION / 2;
        titleAnimation->animating = 1;
        titleAnimation->callback = &infoFaded;
    }
}


UiTile *rewindTiles(UiTile *fromTile, uint32_t depth) {

    if (depth == 0) {
        return fromTile;
    }
    else {
        fromTile = fromTile->previous;
        return rewindTiles(fromTile, --depth);
    }
}

SDL_Texture *createTitleTexture(
        SDL_Renderer *renderer,
        TTF_Font *titleFont,
        char *titleString) 
{
    SDL_Color titleColor = {255,255,255,255};
    SDL_Surface *titleSurface = TTF_RenderText_Blended(titleFont, titleString, titleColor);

    if (!titleSurface) {
        printf("Font render failed, %s\n", TTF_GetError());
        return NULL;
    }

    SDL_Texture* texture = 
            SDL_CreateTextureFromSurface(renderer, titleSurface);

    SDL_FreeSurface(titleSurface);
    
    return texture;
}

double goldenRatioLarge(double in, uint32_t exponent) {
    if (exponent == 0) {
        return in;
    }
    else {
        return goldenRatioLarge(1/PHI * in, --exponent); 
    }
}

void horizontalMoveDone(struct Animation *context) {

    OffblastUi *ui = context->callbackArgs;

    if (context->direction == 1) {
        ui->rows[ui->rowCursor].cursor = 
            ui->rows[ui->rowCursor].cursor->next;
    }
    else {
        ui->rows[ui->rowCursor].cursor = 
            ui->rows[ui->rowCursor].cursor->previous;
    }
}

void infoFaded(struct Animation *context) {

    InfoFadedCallbackArgs *args = context->callbackArgs;

    if (context->direction == 0) {

        SDL_DestroyTexture(args->ui->titleTexture);
        SDL_DestroyTexture(args->ui->infoTexture);
        SDL_DestroyTexture(args->ui->descriptionTexture);

        args->ui->titleTexture = createTitleTexture(
                args->ui->renderer,
                args->ui->titleFont,
                args->newTarget->name);

        args->ui->infoTexture = createInfoTexture(
                args->ui->renderer,
                args->ui->infoFont,
                args->newTarget);

        OffblastBlob *descriptionBlob = (OffblastBlob*)
            &args->descriptionFile->memory[args->newTarget->descriptionOffset];

        args->ui->descriptionTexture = createDescriptionTexture(
                args->ui->renderer,
                args->ui->infoFont,
                descriptionBlob->content,
                args->ui->descriptionWidth,
                args->ui->descriptionHeight);

        assert(args->ui->titleTexture);
        assert(args->ui->infoTexture);
        assert(args->ui->descriptionTexture);

        context->startTick = SDL_GetTicks();
        context->direction = 1;
        context->durationMs = NAVIGATION_MOVE_DURATION / 2;
        context->animating = 1;
        context->callback = &infoFaded;
    }
    else {
        context->animating = 0;
    }
}

SDL_Texture *createInfoTexture(
        SDL_Renderer *renderer,
        TTF_Font *infoFont,
        LaunchTarget *target) 
{
    SDL_Color color = {220,220,220,255};

    char *tempString;
    asprintf(&tempString, "%.4s     %u%%", 
            target->date, target->ranking);

    SDL_Surface *infoSurface = TTF_RenderText_Blended(
            infoFont, tempString, color);

    if (!infoSurface) {
        printf("Font render failed, %s\n", TTF_GetError());
        return NULL;
    }

    SDL_Texture* texture = 
            SDL_CreateTextureFromSurface(renderer, infoSurface);

    SDL_FreeSurface(infoSurface);
    free(tempString);
    
    return texture;
}

SDL_Texture *createDescriptionTexture(
        SDL_Renderer *renderer,
        TTF_Font *infoFont,
        char *description,
        int32_t width, int32_t height) 
{

    SDL_Color color = {220,220,220,255};
    SDL_Surface *surface = TTF_RenderText_Blended_Wrapped(
            infoFont, description, color, width);

    if (!surface) {
        printf("Font render failed, %s\n", TTF_GetError());
        return NULL;
    }

    SDL_Texture* texture = 
            SDL_CreateTextureFromSurface(renderer, surface);

    SDL_FreeSurface(surface);
    return texture;

}

uint32_t megabytes(uint32_t n) {
    return n * 1024 * 1024;
}
