#include <3ds.h>
#include <citro2d.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cstdio>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <sys/types.h>
#include "fake_pthread.hpp"
#include "shield_ui_language.h"
#ifndef APP_BUILD_VERSION
#define APP_BUILD_VERSION 0
#endif
#ifndef APP_RELEASE_VERSION
#define APP_RELEASE_VERSION "dev"
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

// libctru switches main() to its own heap-backed stack, even in a CIA.
// The RSF StackSize only sizes the initial kernel stack. FFmpeg's ARM H.264
// ff_h264_queue_decode_slice alone uses >33 KiB, overflowing the default
// 32 KiB libctru stack before it can decode the first picture.
u32 __stacksize__ = 256 * 1024;
}

static const int MAX_W = 256, MAX_H = 144;
static const char *GUIDE_FILE = "sdmc:/3ds/MP4Player/guide_seen";
static const char *LANGUAGE_FILE = "sdmc:/3ds/MP4Player/ui-language.dat";
static ShieldUiLanguage uiLanguage = {false, false, nullptr};
static bool languagePromptRequested = false;
static const char *DIAG_FILE = "sdmc:/3ds/MP4Player/last_stage.txt";
static char lastStage[8] = "--";
static char lastDetail[64] = "--";
static C3D_RenderTarget *topTarget;
static C2D_SpriteSheet shieldSheet;
static C2D_Image shieldImage;
static C2D_TextBuf textBuf;
static C3D_Tex videoTex;
static Tex3DS_SubTexture videoSub;
static C2D_Image videoImage;
static bool videoTextureReady = false;
static bool videoEcoMode = false;
static bool videoStable15Mode = false;
static int videoDecoderThreads = 1;
static int videoWorkerPriority = 0x34;
static u32 playbackCpuTimeLimit = 0;
static bool videoDecodeCores[4] = {true,true,false,false};
static bool ndspReady = false;
static const int AUDIO_SLOTS = 16;
static ndspWaveBuf audioWave[AUDIO_SLOTS];
static uint8_t *audioPcm[AUDIO_SLOTS] = {nullptr};
static size_t audioCapacity[AUDIO_SLOTS] = {0};
static bool playerErrorShown = false;
static bool startExitRequested = false;
static bool wifiServiceReady = false;
static u64 lastActionAt = 0;
static u8 cachedBatteryPercent=0, cachedTemp=0;
static bool cachedBatteryValid=false, cachedTempValid=false;
static int brightness = 3;
static std::string currentDir = "sdmc:/";
struct BrowserEntry { std::string name, path; bool directory; };
struct BrowserSnapshot { std::string path; std::vector<BrowserEntry> entries; size_t selected; };
static std::vector<BrowserEntry> entries;
static std::vector<BrowserSnapshot> browserHistory;
static size_t selected = 0;
static bool browserReadError = false;
static bool debugVisible = false;
static const char *debugAction = "Explorateur SD";
static std::string debugCurrentFile;
static std::string debugProblem;
struct DebugStats {
    double sourceFps=0, decodeFps=0, imageFps=0;
    double readMsPerPacket=0, videoDecodeMsPerFrame=0, scaleMsPerFrame=0, renderPipelineMsPerFrame=0;
    double textureCopyMsPerFrame=0, textureFlushMsPerFrame=0;
    double audioFlushMsPerBuffer=0;
    double frameSyncMsPerFrame=0, drawSubmitMsPerFrame=0;
    int audioQueued=0, audioSlots=0, lateMs=0;
    unsigned audioBufferGrowths=0, audioFlushFallbacks=0;
    unsigned cadenceSkipped=0, lateSkipped=0, audioPrioritySkipped=0;
    int64_t positionSeconds=0, durationSeconds=0;
};
static DebugStats debugStats;
static u64 textureCopyTicks=0, textureFlushTicks=0;
static unsigned measuredTextureFrames=0;
static u64 audioCacheFlushTicks=0;
static unsigned measuredAudioCacheFlushes=0;
static unsigned audioCacheFlushFallbacks=0;
static u64 frameSyncTicks=0, drawSubmitTicks=0;
static unsigned measuredDrawFrames=0;

struct UiTranslation { const char *french; const char *english; };
static const UiTranslation UI_TRANSLATIONS[] = {
    {"Chargement", "Loading"},
    {"Ouverture du navigateur SD...", "Opening SD browser..."},
    {"Ouverture du dossier...", "Opening folder..."},
    {"Ouverture du fichier MP4...", "Opening MP4 file..."},
    {"Patientez", "Please wait"},
    {"Veuillez patienter", "Please wait"},
    {"Fichier inaccessible", "File unavailable"},
    {"Impossible d'ouvrir ce MP4 depuis la SD.", "Could not open this MP4 from the SD card."},
    {"MP4 incomplet ou non reconnu.\nCode technique : %d", "Incomplete or unsupported MP4.\nTechnical code: %d"},
    {"Memoire insuffisante pour ouvrir ce MP4.\nCode technique : %d", "Not enough memory to open this MP4.\nTechnical code: %d"},
    {"Acces SD ou structure MP4 invalide.\nCode technique : %d", "SD access or MP4 structure error.\nTechnical code: %d"},
    {"Ouverture impossible", "Unable to open"},
    {"Analyse impossible", "Unable to analyze"},
    {"Video absente", "Video missing"},
    {"Ce fichier MP4 ne contient pas de piste video lisible.", "This MP4 file has no readable video stream."},
    {"Codec video indisponible", "Video codec unavailable"},
    {"Ce codec n'est pas disponible dans le lecteur.", "This codec is not available in the player."},
    {"Dimensions invalides", "Invalid dimensions"},
    {"La piste video ne fournit pas de dimensions lisibles.", "The video stream has no readable dimensions."},
    {"Video trop lourde", "Video is too large"},
    {"La 3DS Old ne peut pas decoder cette resolution. Convertissez le MP4 en 144p ou 288p maximum.", "This device cannot decode this resolution. Convert the MP4 to 144p or 288p maximum."},
    {"Decodeur impossible", "Decoder error"},
    {"Le codec video ne peut pas demarrer sur cette 3DS.", "The video codec could not start."},
    {"Memoire video faible", "Low video memory"},
    {"Impossible de creer la texture 144p.", "Could not create the 144p texture."},
    {"Lecture MP4 impossible", "MP4 playback error"},
    {"Le conteneur MP4 n'est pas pret pour la lecture.", "The MP4 container is not ready for playback."},
    {"Lecture SD interrompue", "SD reading interrupted"},
    {"Impossible de lire la suite du fichier MP4.", "Could not read the rest of the MP4 file."},
    {"Image invalide", "Invalid frame"},
    {"Le decodeur n'a pas produit une image utilisable.", "The decoder did not produce a usable frame."},
    {"Image impossible", "Frame conversion error"},
    {"La conversion de l'image en 144p a echoue.", "Could not convert the frame to 144p."},
    {"Conversion impossible", "Conversion error"},
    {"Image non convertible en 144p.", "The frame cannot be converted to 144p."},
    {"Aucune piste MP4 lisible dans ce fichier.", "No readable MP4 streams were found in this file."},
    {"Recherche impossible", "Unable to seek"},
    {"Horodatage de la video invalide.", "The video timestamps are invalid."},
    {"Recherche dans la video...", "Seeking in video..."},
    {"Saut de 5 secondes", "Seeking by 5 seconds"},
    {"Le fichier ne permet pas ce saut. La lecture reprend a sa position actuelle.", "This file does not support seeking. Playback will resume at its current position."},
    {"Lecture impossible", "Playback error"},
    {"Memoire faible, fichier endommage ou profil non gere.", "Low memory, damaged file, or unsupported format."},
    {"Code %s  |  A ou B : retour", "Code %s  |  A or B: back"},
    {"Position : %02lld:%02lld / %02lld:%02lld     ", "Position: %02lld:%02lld / %02lld:%02lld     "},
    {"Luminosite : %d/5", "Brightness: %d/5"},
    {"SHIELD MP4 PLAYER", "SHIELD MP4 PLAYER"},
    {"Batterie", "Battery"}, {"Temperature", "Temperature"},
    {"n/d", "n/a"},
    {"Audio indisponible (DSP)", "Audio unavailable (DSP)"},
    {"Dossier :", "Folder:"},
    {"SELECT : diagnostic | %u elements", "SELECT: diagnostics | %u items"},
    {"Dossier inaccessible. B : remonter", "Folder unavailable. B: go up"},
    {"Aucun dossier ou MP4 ici.", "No folders or MP4 files here."},
    {"[DOSSIER] ", "[FOLDER] "}, {"[MP4] ", "[MP4] "},
    {"A : ouvrir / lire", "A: open / play"},
    {"B : dossier parent", "B: parent folder"},
    {"Haut/bas : selectionner | Gauche/droite : page\nSTART : quitter | Luminosite en lecture\n", "Up/down: select | Left/right: page\nSTART: quit | Brightness during playback\n"},
    {"SHIELD / DIAGNOSTIC", "SHIELD / DIAGNOSTICS"},
    {"SELECT : retour explorateur", "SELECT: back to browser"},
    {"Action : %s\n", "Action: %s\n"}, {"Video : %s\n", "Video: %s\n"},
    {"aucune", "none"}, {"Code : %s\n", "Code: %s\n"},
    {"Pistes : %.32s\n", "Streams: %.32s\n"},
    {"Probleme : %s\n", "Problem: %s\n"},
    {"aucun", "none"},
    {"Echant. C:%u | pertes R:%u A:%u\n", "Frames C:%u | drops R:%u A:%u\n"},
    {"Position : %lld / %lld s\n", "Position: %lld / %lld s\n"},
    {"FPS fichier : %.1f\n", "Source FPS: %.1f\n"},
    {"FPS recues  : %.1f\n", "Decoded FPS: %.1f\n"},
    {"FPS envoyees: %.1f\n", "Presented FPS: %.1f\n"},
    {"Mode video : %s\n", "Video mode: %s\n"},
    {"cadence stable 15 FPS", "stable 15 FPS cadence"},
    {"ECO (filtre de boucle reduit)", "ECO (reduced loop filter)"},
    {"normal", "normal"},
    {"Tampons audio : %d/%d\n", "Audio buffers: %d/%d\n"},
    {"Retard image : %d ms\n", "Frame delay: %d ms\n"},
    {"I/O pkt %.1f | appels codec %.1f ms\n", "Packet I/O %.1f | codec call %.1f ms\n"},
    {"Scale %.1f | transf+GPU %.1f ms\n", "Scale %.1f | transfer+GPU %.1f ms\n"},
    {"Copie/flush texture : %.1f / %.1f ms\n", "Texture copy/flush: %.1f / %.1f ms\n"},
    {"Flush audio par bloc : %.1f ms\n", "Audio flush per block: %.1f ms\n"},
    {"Agrandissements PCM : %u\n", "PCM buffer growths: %u\n"},
    {"Quota CPU app (max) : %u%%\n", "App CPU limit (max): %u%%\n"},
    {"Sync GPU / draw : %.1f / %.1f ms\n", "GPU sync / draw: %.1f / %.1f ms\n"},
    {"Repli flush DSP : %u\n", "DSP flush fallback: %u\n"},
    {"\nCes mesures changent toutes les 2 s.\n", "\nThese measurements update every 2 sec.\n"},
    {"Action : %-30.30s", "Action: %-30.30s"},
    {"Code : %-32.32s", "Code: %-32.32s"},
    {"Echant. C:%u | pertes R:%u A:%u              ", "Frames C:%u | drops R:%u A:%u              "},
    {"Position : %lld / %lld s          ", "Position: %lld / %lld s          "},
    {"FPS fichier : %.1f                    ", "Source FPS: %.1f                    "},
    {"FPS recues  : %.1f                    ", "Decoded FPS: %.1f                    "},
    {"FPS envoyees: %.1f                    ", "Presented FPS: %.1f                    "},
    {"Mode video : %-30s", "Video mode: %-30s"},
    {"Tampons audio : %d/%d                 ", "Audio buffers: %d/%d                 "},
    {"Retard image : %d ms                 ", "Frame delay: %d ms                 "},
    {"I/O pkt %.1f | appels codec %.1f ms        ", "Packet I/O %.1f | codec call %.1f ms        "},
    {"Scale %.1f | transf+GPU %.1f ms                ", "Scale %.1f | transfer+GPU %.1f ms                "},
    {"Copie/flush texture : %.1f / %.1f ms       ", "Texture copy/flush: %.1f / %.1f ms       "},
    {"Flush audio par bloc : %.1f ms            ", "Audio flush per block: %.1f ms            "},
    {"Agrandissements PCM : %u                  ", "PCM buffer growths: %u                  "},
    {"Quota CPU app (max) : %u%%                  ", "App CPU limit (max): %u%%                  "},
    {"Sync GPU / draw : %.1f / %.1f ms        ", "GPU sync / draw: %.1f / %.1f ms        "},
    {"Repli flush DSP : %u                  ", "DSP flush fallback: %u                  "},
    {"MP4 Player Old - Guide 1/3", "Shield MP4 Player - Guide 1/3"},
    {"Guide 2/3 - Commandes", "Guide 2/3 - Controls"},
    {"Guide 3/3 - Depannage", "Guide 3/3 - Help"},
    {"Fichiers MP4 locaux depuis la SD.\nL'image est adaptee a l'ecran.\nLe lecteur utilise les codecs disponibles.\nUn codec trop lourd peut ralentir la 3DS.", "Play local MP4 files from the SD card.\nVideo is fitted to the screen.\nThe player uses available codecs.\nHeavy codecs may slow down playback."},
    {"A : ouvrir / pause - B : retour / arreter\nGauche / droite : -5s / +5s a vitesse x1\nHaut/bas : luminosite en lecture uniquement\nSTART : quitter - tactile ignore.", "A: open / pause - B: back / stop\nLeft / right: seek -5s / +5s at x1\nUp/down: adjust brightness during playback\nSTART: quit - touch is disabled."},
    {"Chargement long : patienter quelques secondes.\nEcran noir ou erreur : codec non gere ou\nfichier endommage. Essayer un autre MP4.\nLes grandes videos peuvent etre plus lentes.", "Long loading: wait a few seconds.\nBlack screen or error: unsupported codec or\ndamaged file. Try another MP4.\nLarge videos may run more slowly."},
    {"A : j'ai compris", "A: Got it"},
    {"A : page suivante (%d/3)", "A: next page (%d/3)"},
    {"Choix de langue", "Language selection"},
    {"Votre langue est bien le francais ?\nSeules le francais et English sont disponibles.", "Is English the right language for you?\nOnly French and English are available."},
    {"Votre langue est bien English ?\nSeules le francais et English sont disponibles.", "Is English the right language for you?\nOnly French and English are available."},
    {"Seules les langues francaise et English sont disponibles.", "Only French and English are available."},
    {"Seules les langues francaise et English sont disponibles.\nLa langue detectee est-elle la bonne ?", "Only French and English are available.\nIs the detected language correct?"},
    {"A : oui   B : non / English", "A: yes   B: no / English"},
    {"A : oui / Francais   B : non", "A: yes   B: no / Francais"},
    {"X maintenu au demarrage : choisir la langue", "Hold X at startup to change language"},
    {"Explorateur SD", "SD browser"},
    {"Ouverture du MP4", "Opening MP4"},
    {"Lecture", "Playing"}, {"Pause", "Paused"},
    {"Probleme de lecture", "Playback problem"},
    {"Recherche +5 s", "Seek +5 s"},
    {"Recherche -5 s", "Seek -5 s"},
};

static const char *uiText(const char *text) {
    if(!text || uiLanguage.french) return text ? text : "";
    for(size_t i=0;i<sizeof(UI_TRANSLATIONS)/sizeof(UI_TRANSLATIONS[0]);++i)
        if(strcmp(text,UI_TRANSLATIONS[i].french)==0) return UI_TRANSLATIONS[i].english;
    return text;
}

static void showBrowser();
static void showDebugPanel();
static void updateDebugMetrics();
static void toggleDebug() {
    debugVisible=!debugVisible;
    if(debugVisible) showDebugPanel(); else showBrowser();
}

static bool fileExists(const char *path) { FILE *f=fopen(path,"rb"); if (!f) return false; fclose(f); return true; }
static bool flushAudioPcmCache(void *address,u32 size) {
    // The kernel flush cleans and invalidates the current process cache for
    // this virtual linear-memory range. Keep libctru's DSP flush as a fallback.
    if(R_SUCCEEDED(svcFlushProcessDataCache(CUR_PROCESS_HANDLE,(u32)(uintptr_t)address,size))) return false;
    DSP_FlushDataCache(address,size);
    return true;
}
static void reserveAudioPcmBuffers(SwrContext *swr,AVCodecContext *actx) {
    if(!swr || !actx || actx->sample_rate<=0) return;
    int inputSamples=actx->frame_size>0?std::min(actx->frame_size,8192):1024;
    inputSamples+=64; // room for resampler delay and codec padding
    int64_t outputSamples=av_rescale_rnd(swr_get_delay(swr,actx->sample_rate)+inputSamples,
                                         32728,actx->sample_rate,AV_ROUND_UP);
    if(outputSamples<=0 || (uint64_t)outputSamples>SIZE_MAX/4) return;
    size_t reserveBytes=(size_t)outputSamples*4; // stereo signed 16-bit PCM
    for(int i=0;i<AUDIO_SLOTS;i++) {
        if(audioCapacity[i]>=reserveBytes) continue;
        uint8_t *reserved=(uint8_t*)linearAlloc(reserveBytes);
        if(!reserved) continue;
        if(audioPcm[i]) linearFree(audioPcm[i]);
        audioPcm[i]=reserved;
        audioCapacity[i]=reserveBytes;
    }
}
static int readSdFile(void *opaque, uint8_t *buffer, int size) {
    FILE *file=(FILE*)opaque;
    size_t count=fread(buffer,1,size,file);
    if(count) return (int)count;
    return feof(file)?AVERROR_EOF:AVERROR(errno?errno:EIO);
}
static int64_t seekSdFile(void *opaque, int64_t offset, int whence) {
    FILE *file=(FILE*)opaque;
    if(whence&AVSEEK_SIZE) {
        off_t old=ftello(file);
        if(old<0 || fseeko(file,0,SEEK_END)!=0) return AVERROR(EIO);
        off_t size=ftello(file);
        if(fseeko(file,old,SEEK_SET)!=0 || size<0) return AVERROR(EIO);
        return size;
    }
    whence &= ~AVSEEK_FORCE;
    if((int64_t)(off_t)offset!=offset || whence<SEEK_SET || whence>SEEK_END)
        return AVERROR(EINVAL);
    if(fseeko(file,(off_t)offset,whence)!=0) return AVERROR(errno?errno:EIO);
    off_t position=ftello(file);
    return position<0?AVERROR(errno?errno:EIO):position;
}
static void saveStage(const char *code) {
    snprintf(lastStage,sizeof(lastStage),"%s",code);
    FILE *f=fopen(DIAG_FILE,"wb");
    if(f) { fprintf(f,"%s\n%s\n",lastStage,lastDetail); fclose(f); }
}
static void loadStage() {
    FILE *f=fopen(DIAG_FILE,"rb");
    if(f) {
        if(!fgets(lastStage,sizeof(lastStage),f)) snprintf(lastStage,sizeof(lastStage),"--");
        if(!fgets(lastDetail,sizeof(lastDetail),f)) snprintf(lastDetail,sizeof(lastDetail),"--");
        fclose(f);
    }
    lastStage[strcspn(lastStage,"\r\n")]=0;
    lastDetail[strcspn(lastDetail,"\r\n")]=0;
}
static bool isMp4(const std::string &s) {
    if (s.size()<4) return false;
    std::string ext=s.substr(s.size()-4);
    for(char &c:ext) c=(char)tolower((unsigned char)c);
    return ext==".mp4";
}
static std::string joinPath(const std::string &dir,const std::string &name) {
    return dir=="sdmc:/" ? dir+name : dir+"/"+name;
}
static bool loadCurrentDirectory() {
    entries.clear(); selected=0; browserReadError=false;
    DIR *d=opendir(currentDir.c_str());
    if(!d) { browserReadError=true; return false; }
    std::vector<BrowserEntry> folders;
    struct dirent *e;
    while((e=readdir(d))) {
        std::string n=e->d_name;
        if(n=="." || n==".." || n.empty() || n[0]=='.') continue;
        if(e->d_type==DT_REG && !isMp4(n)) continue;
        std::string full=joinPath(currentDir,n);
        bool directory=e->d_type==DT_DIR;
        if(e->d_type!=DT_DIR && e->d_type!=DT_REG) {
            struct stat st;
            if(stat(full.c_str(),&st)!=0) continue;
            directory=S_ISDIR(st.st_mode);
        }
        if(directory) {
            folders.push_back({n,full,true});
        }
        else if(isMp4(n)) entries.push_back({n,full,false});
    }
    closedir(d);
    // Only enumerate this directory. Descendants are opened on demand.
    entries.insert(entries.end(),folders.begin(),folders.end());
    std::sort(entries.begin(),entries.end(),[](const BrowserEntry &a,const BrowserEntry &b) {
        if(a.directory!=b.directory) return a.directory>b.directory;
        return a.name<b.name;
    });
    return true;
}
static void drawTopText(const char *s,float x,float y,float scale,u32 color) {
    C2D_Text t;
    C2D_TextParse(&t,textBuf,s); C2D_TextOptimize(&t);
    C2D_DrawText(&t,C2D_WithColor,x,y,0.0f,scale,scale,color);
}
static void drawBackground() {
    if(shieldSheet) C2D_DrawImageAt(shieldImage,0,20,0,nullptr,0.78125f,0.78125f);
}
static void showIdleTop() {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(topTarget,C2D_Color32(3,12,24,255));
    C2D_SceneBegin(topTarget);
    drawBackground();
    C3D_FrameEnd(0);
}
static void showTop(const char *title,const char *body,const char *footer) {
    C2D_TextBufClear(textBuf);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(topTarget, C2D_Color32(3,12,24,255)); C2D_SceneBegin(topTarget);
    drawBackground();
    C2D_DrawRectSolid(16,22,0,368,196,C2D_Color32(0,0,0,205));
    drawTopText(uiText(title),28,30,0.72f,C2D_Color32(60,220,245,255));
    drawTopText(uiText(body),28,74,0.48f,C2D_Color32(255,255,255,255));
    drawTopText(uiText(footer),28,202,0.45f,C2D_Color32(255,255,255,255));
    C3D_FrameEnd(0);
}
static void waitForMessage(const char *title,const char *body) {
    playerErrorShown=true;
    debugAction=uiText("Probleme de lecture");
    debugProblem=uiText(title);
    char footer[64]; snprintf(footer,sizeof(footer),uiText("Code %s  |  A ou B : retour"),lastStage);
    showTop(title,body,footer);
    while(aptMainLoop()) { hidScanInput(); u32 k=hidKeysDown(); if(k&KEY_SELECT) toggleDebug(); if(k&KEY_START) startExitRequested=true; if(k&(KEY_A|KEY_B|KEY_START)) break; gspWaitForVBlank(); }
}
static void refreshSystemInfo() {
    u8 rawBattery=0;
    cachedBatteryValid=R_SUCCEEDED(MCUHWC_GetBatteryLevel(&rawBattery)) && rawBattery<=100;
    if(cachedBatteryValid) cachedBatteryPercent=(u8)std::min(100,((int)rawBattery+2)/5*5);
    cachedTempValid=R_SUCCEEDED(MCUHWC_ReadRegister(0x0A,&cachedTemp,1));
}
static void showPlaybackBrightness() {
    if(debugVisible) showDebugPanel();
    else { printf("\x1b[28;1H"); printf(uiText("Luminosite : %d/5"),brightness); }
}
static void showPlaybackProgress(int64_t positionSeconds,int64_t durationSeconds) {
    char bar[41];
    int filled=durationSeconds>0?(int)std::min<int64_t>(40,std::max<int64_t>(0,positionSeconds*40/durationSeconds)):0;
    for(int i=0;i<40;i++) bar[i]=i<filled?'=':'-';
    bar[40]='\0';
    int64_t pos=std::max<int64_t>(0,positionSeconds), dur=std::max<int64_t>(0,durationSeconds);
    printf("\x1b[25;1H");
    printf(uiText("Position : %02lld:%02lld / %02lld:%02lld     "),
        (long long)(pos/60),(long long)(pos%60),(long long)(dur/60),(long long)(dur%60));
    printf("\x1b[26;1H[%s]",bar);
}
static void changePlaybackBrightness(int step) {
    int next=std::max(1,std::min(5,brightness+step));
    if(next==brightness) return;
    brightness=next;
    GSPLCD_SetBrightness(GSPLCD_SCREEN_TOP,brightness);
    GSPLCD_SetBrightness(GSPLCD_SCREEN_BOTTOM,brightness);
    showPlaybackBrightness();
}
static void showVideoFrame(int w,int h,const uint8_t *pixels) {
    const int pixelSize=2, tw=256, th=256;
    static int incX[tw+8]={0}, incY[th+8]={0};
    static bool offsetsReady=false;
    if(!offsetsReady) {
        const int patX[4]={4,12,4,44}, patY[8]={2,6,2,22,2,6,2,tw*8-42};
        for(int i=0;i<=tw;i+=4) for(int j=0;j<4;j++) incX[i+j]=patX[j]*pixelSize;
        for(int i=0;i<=th;i+=8) for(int j=0;j<8;j++) incY[i+j]=patY[j]*pixelSize;
        offsetsReady=true;
    }
    const bool measureTextureStages=debugVisible;
    const u64 copyStarted=measureTextureStages?svcGetSystemTick():0;
    uint8_t *textureData=(uint8_t*)videoTex.data;
    int offsetY=0;
    for(int y=0;y<h;y++) {
        int offsetX=0;
        const uint8_t *srcPair=pixels+(size_t)y*w*pixelSize;
        uint8_t *dstRow=textureData+offsetY;
        // renderW is always even (it is rounded down in playFile), so copy
        // complete RGB565 pairs with a constant-size operation in the hot loop.
        int x=0;
        const int *xOffsets=incX;
        for(;x+1<w;x+=2) {
            uint32_t pair;
            memcpy(&pair,srcPair,sizeof(pair));
            *(uint32_t*)(dstRow+offsetX)=pair;
            srcPair+=sizeof(pair);
            offsetX+=*xOffsets++;
        }
        if(x<w) {
            memcpy(dstRow+offsetX,srcPair,pixelSize);
        }
        offsetY+=incY[y];
    }
    offsetY+=incY[h]; offsetY+=incY[h+1];
    if(measureTextureStages) textureCopyTicks+=svcGetSystemTick()-copyStarted;
    // The texture is 256x256, but videoSub samples only its top h rows.
    // Flush whole 8-row tile bands so every sampled texel is visible to the GPU;
    // this avoids cleaning the unused bottom 112 rows on each frame.
    const size_t tileRows=((size_t)h+7u)&~(size_t)7u;
    const size_t usedBytes=std::min((size_t)videoTex.size,(size_t)videoTex.width*tileRows*pixelSize);
    const u64 flushStarted=measureTextureStages?svcGetSystemTick():0;
    GSPGPU_FlushDataCache(videoTex.data,(u32)usedBytes);
    if(measureTextureStages) { textureFlushTicks+=svcGetSystemTick()-flushStarted; measuredTextureFrames++; }
    videoSub.left=0; videoSub.top=1; videoSub.width=w; videoSub.height=h;
    videoSub.right=(float)w/256.0f; videoSub.bottom=1.0f-(float)h/256.0f;
    videoImage.tex=&videoTex; videoImage.subtex=&videoSub;
    float scale=std::min(400.0f/w,240.0f/h), dw=w*scale, dh=h*scale;
    float x=(400-dw)*0.5f, y=(240-dh)*0.5f;
    const u64 syncStarted=measureTextureStages?svcGetSystemTick():0;
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    if(measureTextureStages) frameSyncTicks+=svcGetSystemTick()-syncStarted;
    const u64 drawStarted=measureTextureStages?svcGetSystemTick():0;
    C2D_TargetClear(topTarget,C2D_Color32(0,0,0,255)); C2D_SceneBegin(topTarget);
    C2D_DrawImageAt(videoImage,x,y,0,nullptr,scale,scale);
    C3D_FrameEnd(0);
    if(measureTextureStages) { drawSubmitTicks+=svcGetSystemTick()-drawStarted; measuredDrawFrames++; }
}
static bool playFile(const std::string &path) {
    playerErrorShown=false;
    videoDecoderThreads=1;
    videoWorkerPriority=0x34;
    videoEcoMode=false;
    videoStable15Mode=false;
    debugCurrentFile=path;
    debugAction="Ouverture du MP4";
    debugProblem.clear();
    debugStats=DebugStats{};
    textureCopyTicks=textureFlushTicks=0; measuredTextureFrames=0;
    audioCacheFlushTicks=0; measuredAudioCacheFlushes=0;
    audioCacheFlushFallbacks=0;
    frameSyncTicks=drawSubmitTicks=0; measuredDrawFrames=0;
    lastActionAt=0;
    snprintf(lastDetail,sizeof(lastDetail),"--");
    AVFormatContext *fmt=nullptr; AVCodecContext *vctx=nullptr,*actx=nullptr;
    AVIOContext *customIO=nullptr;
    FILE *customFile=nullptr;
    std::string url;
    AVFrame *frame=av_frame_alloc(); AVPacket *pkt=av_packet_alloc();
    SwsContext *sws=nullptr; SwrContext *swr=nullptr;
    uint8_t *rgb=(uint8_t*)malloc(MAX_W*MAX_H*2);
    const AVCodec *vc=nullptr,*ac=nullptr; int64_t inLayout=0;
    bool ok=false, stop=false, paused=false, seekPending=false, markedFirstRead=false, markedVideoSend=false, markedVideoRecv=false, markedAudioSend=false, markedAudioRecv=false, markedScale=false, markedDraw=false, markedAudio=false;
    int pendingSeekDirection=0;
    u64 lastSeekAt=0, lastStatsAt=0, lastProgressAt=0;
    int64_t videoLateMs=0;
    unsigned decodedFrames=0, shownFrames=0;
    unsigned measuredReadPackets=0, measuredVideoFrames=0, measuredScaledFrames=0, measuredRenderedFrames=0;
    unsigned cadenceSkippedFrames=0, lateSkippedFrames=0, audioPrioritySkippedFrames=0;
    unsigned audioBufferGrowths=0;
    double cadenceAccumulator=0.0;
    double cadencePerTimestampTick=0.0;
    int64_t lastCadencePts=AV_NOPTS_VALUE;
    bool haveCadenceFrame=false;
    u64 readTicks=0, videoDecodeTicks=0, scaleTicks=0, renderTicks=0;
    u32 previousCpuTimeLimit=30;
    bool cpuLimitRaised=false;
    double sourceFps=0.0;
    AVRational sourceRate={0,1};
    AVRational cadenceTimeBase={0,1};
    int vi=-1,ai=-1,sourceW=0,sourceH=0,renderW=0,renderH=0,fferr=0,videoOpenResult=0,swsSourceW=0,swsSourceH=0,swsSourceFmt=-1; double fit=1.0,lateEnterMs=60.0,lateExitMs=20.0; int64_t currentPts=0, firstPts=AV_NOPTS_VALUE, seekTargetPts=0, audioSeekTarget=AV_NOPTS_VALUE, mediaDurationUs=0; u64 wallStart=0, pauseStarted=0;
    if(!frame||!pkt||!rgb) goto cleanup;
    if(R_SUCCEEDED(APT_GetAppCpuTimeLimit(&previousCpuTimeLimit))) {
        if(R_SUCCEEDED(APT_SetAppCpuTimeLimit(80))) {
            cpuLimitRaised=true;
            playbackCpuTimeLimit=80;
        } else playbackCpuTimeLimit=previousCpuTimeLimit;
    } else playbackCpuTimeLimit=0;
    showTop(uiText("Chargement"),uiText("Ouverture du fichier MP4..."),uiText("Patientez"));
    saveStage("S00");
    if(!fileExists(path.c_str())) { saveStage("E01"); waitForMessage(uiText("Fichier inaccessible"),uiText("Impossible d'ouvrir ce MP4 depuis la SD.")); goto cleanup; }
    // Force FFmpeg's file protocol; the 3DS "sdmc:" prefix is a filesystem
    // device name, and a normal URL parser could mistake it for a protocol.
    url="file:"+path;
    fmt=avformat_alloc_context();
    if(!fmt) goto cleanup;
    fmt->probesize=512*1024;
    fmt->max_analyze_duration=AV_TIME_BASE;
    saveStage("S01");
    fferr=avformat_open_input(&fmt,url.c_str(),nullptr,nullptr);
    if(fferr<0 || !fmt) {
        // fopen() already succeeded; custom IO also supports device paths
        // that FFmpeg's URL handler may reject on the 3DS SD card.
        if(fmt) avformat_close_input(&fmt);
        customFile=fopen(path.c_str(),"rb");
        uint8_t *buffer=customFile?(uint8_t*)av_malloc(32*1024):nullptr;
        if(buffer) {
            customIO=avio_alloc_context(buffer,32*1024,0,customFile,readSdFile,nullptr,seekSdFile);
            if(!customIO) av_free(buffer);
        }
        if(customIO) {
            fmt=avformat_alloc_context();
            if(fmt) {
                fmt->pb=customIO;
                fmt->flags|=AVFMT_FLAG_CUSTOM_IO;
                fmt->probesize=512*1024;
                fmt->max_analyze_duration=AV_TIME_BASE;
                saveStage("S1B");
                fferr=avformat_open_input(&fmt,"local.mp4",nullptr,nullptr);
            } else fferr=AVERROR(ENOMEM);
        }
    }
    if(fferr<0 || !fmt) {
        snprintf(lastDetail,sizeof(lastDetail),"Ouverture FFmpeg: %d",fferr);
        saveStage("E02");
        char reason[120];
        if(fferr==AVERROR_INVALIDDATA) snprintf(reason,sizeof(reason),uiText("MP4 incomplet ou non reconnu.\nCode technique : %d"),fferr);
        else if(fferr==AVERROR(ENOMEM)) snprintf(reason,sizeof(reason),uiText("Memoire insuffisante pour ouvrir ce MP4.\nCode technique : %d"),fferr);
        else snprintf(reason,sizeof(reason),uiText("Acces SD ou structure MP4 invalide.\nCode technique : %d"),fferr);
        waitForMessage(uiText("Ouverture impossible"),reason);
        goto cleanup;
    }
    saveStage("S02");
    // MP4's mov_read_header() has already populated the track metadata.
    // Use the track metadata directly to avoid an extra decoding probe
    // before opening the selected decoders.
    if(!fmt->nb_streams || !fmt->streams) { saveStage("E03"); waitForMessage(uiText("Analyse impossible"),uiText("Aucune piste MP4 lisible dans ce fichier.")); goto cleanup; }
    for(unsigned i=0;i<fmt->nb_streams;i++) {
        if(!fmt->streams[i] || !fmt->streams[i]->codecpar) continue;
        AVCodecParameters *p=fmt->streams[i]->codecpar;
        if(p->codec_type==AVMEDIA_TYPE_VIDEO && vi<0) vi=(int)i;
        if(p->codec_type==AVMEDIA_TYPE_AUDIO && ai<0) ai=(int)i;
    }
    if(vi<0) { saveStage("E04"); waitForMessage("Video absente","Ce fichier MP4 ne contient pas de piste video lisible."); goto cleanup; }
    // Some MP4 files omit the movie-level duration while keeping duration on
    // their tracks. Use the longest track as a fallback for progress and seek.
    mediaDurationUs=fmt->duration>0?fmt->duration:0;
    if(mediaDurationUs<=0) {
        for(unsigned i=0;i<fmt->nb_streams;i++) {
            AVStream *stream=fmt->streams[i];
            if(!stream || stream->duration<=0 || stream->time_base.num<=0 || stream->time_base.den<=0) continue;
            int64_t streamDurationUs=av_rescale_q(stream->duration,stream->time_base,AV_TIME_BASE_Q);
            if(streamDurationUs>mediaDurationUs) mediaDurationUs=streamDurationUs;
        }
    }
    sourceRate=fmt->streams[vi]->avg_frame_rate;
    if(sourceRate.den>0) sourceFps=av_q2d(sourceRate);
    if(sourceFps<=0.0 || sourceFps>120.0) {
        sourceRate=fmt->streams[vi]->r_frame_rate;
        sourceFps=sourceRate.den>0?av_q2d(sourceRate):0.0;
    }
    if(sourceFps<0.0 || sourceFps>120.0) sourceFps=0.0;
    debugStats.sourceFps=sourceFps;
    cadenceTimeBase=fmt->streams[vi]->time_base;
    if(cadenceTimeBase.num>0 && cadenceTimeBase.den>0)
        cadencePerTimestampTick=15.0*(double)cadenceTimeBase.num/(double)cadenceTimeBase.den;
    // On the Old 3DS, start fast sources in a regular 15 fps presentation
    // cadence instead of letting the renderer build a backlog first.
    videoStable15Mode=sourceFps>15.5;
    // ECO only disables H.264 loop filtering; it never drops pictures. Enter
    // sooner than the old 140 ms threshold so a growing backlog can recover
    // before several presentation intervals have accumulated.
    lateEnterMs=sourceFps>1.0?std::max(80.0,2000.0/sourceFps):80.0;
    lateExitMs=sourceFps>1.0?std::max(35.0,900.0/sourceFps):35.0;
    if(mediaDurationUs>0) debugStats.durationSeconds=mediaDurationUs/AV_TIME_BASE;
    vc=avcodec_find_decoder(fmt->streams[vi]->codecpar->codec_id);
    if(ai>=0) ac=avcodec_find_decoder(fmt->streams[ai]->codecpar->codec_id);
    if(!vc) { saveStage("E05"); waitForMessage("Codec video indisponible","Ce codec n'est pas disponible dans le lecteur."); goto cleanup; }
    if(!ac || !ndspReady) ai=-1;
    vctx=avcodec_alloc_context3(vc);
    if(!vctx || avcodec_parameters_to_context(vctx,fmt->streams[vi]->codecpar)<0) goto cleanup;
    vctx->thread_count=1;
    vctx->thread_type=0;
    // APT's CPU budget must be raised before starting a worker on the Old 3DS
    // system core. Without it, the earlier two-core path could stall at open.
    // Prefer frame threading for throughput; fall back to slice threads or
    // the single-thread path when the decoder does not support them.
    if(playbackCpuTimeLimit>=80 && (vc->capabilities&AV_CODEC_CAP_FRAME_THREADS)) {
        vctx->thread_type=FF_THREAD_FRAME;
        vctx->thread_count=2;
    } else if(playbackCpuTimeLimit>=80 && (vc->capabilities&AV_CODEC_CAP_SLICE_THREADS)) {
        vctx->thread_type=FF_THREAD_SLICE;
        vctx->thread_count=2;
    }
    if(vctx->thread_count>1) {
        Util_fake_pthread_set_enabled_core(videoDecodeCores);
        s32 mainPriority=0x30;
        if(R_SUCCEEDED(svcGetThreadPriority(&mainPriority,CUR_THREAD_HANDLE)))
            videoWorkerPriority=std::min(0x3F,(int)mainPriority+1);
        Util_fake_pthread_set_worker_priority(videoWorkerPriority);
    }
    vctx->flags2|=AV_CODEC_FLAG2_FAST;
    vctx->skip_loop_filter=AVDISCARD_NONREF;
    sourceW=fmt->streams[vi]->codecpar->width; sourceH=fmt->streams[vi]->codecpar->height;
    snprintf(lastDetail,sizeof(lastDetail),"V:%s %dx%d A:%s",vc->name,sourceW,sourceH,ac?ac->name:"aucun");
    if(sourceW<=0 || sourceH<=0) { saveStage("E06"); waitForMessage("Dimensions invalides","La piste video ne fournit pas de dimensions lisibles."); goto cleanup; }
    if((sourceW>512 || sourceH>288) && vc->max_lowres<1) {
        saveStage("E07");
        waitForMessage("Video trop lourde","La 3DS Old ne peut pas decoder cette resolution. Convertissez le MP4 en 144p ou 288p maximum."); goto cleanup;
    }
    if((sourceW>1024 || sourceH>576) && vc->max_lowres>=2) vctx->lowres=2;
    else if((sourceW>512 || sourceH>288) && vc->max_lowres>=1) vctx->lowres=1;
    saveStage("S03");
    videoOpenResult=avcodec_open2(vctx,vc,nullptr);
    if(videoOpenResult<0 && vctx->thread_count>1) {
        avcodec_free_context(&vctx);
        vctx=avcodec_alloc_context3(vc);
        if(vctx && avcodec_parameters_to_context(vctx,fmt->streams[vi]->codecpar)>=0) {
            videoWorkerPriority=0;
            vctx->thread_count=1;
            vctx->thread_type=0;
            vctx->flags2|=AV_CODEC_FLAG2_FAST;
            vctx->skip_loop_filter=AVDISCARD_NONREF;
            if((sourceW>1024 || sourceH>576) && vc->max_lowres>=2) vctx->lowres=2;
            else if((sourceW>512 || sourceH>288) && vc->max_lowres>=1) vctx->lowres=1;
            videoOpenResult=avcodec_open2(vctx,vc,nullptr);
        } else videoOpenResult=AVERROR(ENOMEM);
    }
    if(videoOpenResult<0 || !vctx || vctx->width<=0 || vctx->height<=0) { saveStage("E08"); waitForMessage("Decodeur impossible","Le codec video ne peut pas demarrer sur cette 3DS."); goto cleanup; }
    videoDecoderThreads=(vctx->active_thread_type && vctx->thread_count>1)?vctx->thread_count:1;
    if(videoDecoderThreads>1)
        snprintf(lastDetail,sizeof(lastDetail),"V:%s %dx%d T:%d P:%02X A:%s",vc->name,sourceW,sourceH,videoDecoderThreads,videoWorkerPriority,ac?ac->name:"aucun");
    else snprintf(lastDetail,sizeof(lastDetail),"V:%s %dx%d T:%d A:%s",vc->name,sourceW,sourceH,videoDecoderThreads,ac?ac->name:"aucun");
    if(ai>=0) {
        saveStage("S0B");
        actx=avcodec_alloc_context3(ac);
        if(actx && avcodec_parameters_to_context(actx,fmt->streams[ai]->codecpar)>=0) {
            actx->thread_count=1;
            if(avcodec_open2(actx,ac,nullptr)<0 || actx->sample_rate<=0 || actx->channels<=0) { avcodec_free_context(&actx); ai=-1; }
        } else { if(actx) avcodec_free_context(&actx); ai=-1; }
    }
    if(actx) {
        inLayout=actx->channel_layout?actx->channel_layout:av_get_default_channel_layout(actx->channels);
        swr=swr_alloc_set_opts(nullptr,AV_CH_LAYOUT_STEREO,AV_SAMPLE_FMT_S16,32728,inLayout,actx->sample_fmt,actx->sample_rate,0,nullptr);
        if(!swr||swr_init(swr)<0) { if(swr) swr_free(&swr); avcodec_free_context(&actx); ai=-1; }
        else reserveAudioPcmBuffers(swr,actx);
    }
    if(!videoTextureReady) {
        if(!C3D_TexInit(&videoTex,256,256,GPU_RGB565)) { saveStage("E10"); waitForMessage("Memoire video faible","Impossible de creer la texture 144p."); goto cleanup; }
        C3D_TexSetFilter(&videoTex,GPU_LINEAR,GPU_LINEAR);
        videoTextureReady=true;
    }
    saveStage("S0D");
    if(ndspReady) { ndspChnReset(0); ndspChnSetFormat(0,NDSP_FORMAT_STEREO_PCM16); ndspChnSetRate(0,32728.0f); ndspChnSetInterp(0,NDSP_INTERP_LINEAR); }
    for(int j=0;j<AUDIO_SLOTS;j++) { audioWave[j].status=NDSP_WBUF_FREE; audioWave[j].data_vaddr=audioPcm[j]; audioWave[j].nsamples=0; }
    ok=true;
    saveStage("S04");
    debugAction="Lecture";
    showPlaybackBrightness();
    lastStatsAt=osGetTime();
    while(aptMainLoop()&&!stop) {
        if(!markedFirstRead) saveStage("S4L");
        hidScanInput();
        u32 down=hidKeysDown();
        if(down&KEY_SELECT) toggleDebug();
        if(down&KEY_START) { startExitRequested=true; stop=true; break; }
        if(down&KEY_B) { stop=true; break; }
        u64 actionNow=osGetTime();
        if((down&KEY_A) && actionNow-lastActionAt>=220) { lastActionAt=actionNow; paused=!paused; debugAction=paused?"Pause":"Lecture"; if(ndspReady) ndspChnSetPaused(0,paused); if(paused) pauseStarted=osGetTime(); else wallStart+=osGetTime()-pauseStarted; if(debugVisible) showDebugPanel(); }
        if(down&KEY_DUP) changePlaybackBrightness(1);
        if(down&KEY_DDOWN) changePlaybackBrightness(-1);
        if(down&(KEY_DLEFT|KEY_DRIGHT)) pendingSeekDirection=(down&KEY_DRIGHT)?1:-1;
        if(pendingSeekDirection && actionNow-lastSeekAt>=250) {
            int direction=pendingSeekDirection;
            pendingSeekDirection=0;
            debugAction=direction>0?uiText("Recherche +5 s"):uiText("Recherche -5 s");
            lastSeekAt=actionNow;
            saveStage("S09");
            AVRational tb=fmt->streams[vi]->time_base;
            if(tb.num<=0 || tb.den<=0) { saveStage("E14"); waitForMessage(uiText("Recherche impossible"),uiText("Horodatage de la video invalide.")); continue; }
            int64_t step=av_rescale_q(5*AV_TIME_BASE,AV_TIME_BASE_Q,tb);
            int64_t base=(currentPts==AV_NOPTS_VALUE)?0:currentPts;
            seekTargetPts=std::max<int64_t>(0,base+direction*step);
            if(mediaDurationUs>0) {
                int64_t durationPts=av_rescale_q(mediaDurationUs,AV_TIME_BASE_Q,fmt->streams[vi]->time_base);
                if(durationPts>0) seekTargetPts=std::min(seekTargetPts,durationPts-1);
            }
            seekPending=true;
            audioSeekTarget=ai>=0?av_rescale_q(seekTargetPts,fmt->streams[vi]->time_base,fmt->streams[ai]->time_base):AV_NOPTS_VALUE;
            showTop(uiText("Chargement"),uiText("Recherche dans la video..."),uiText("Saut de 5 secondes"));
            if(avformat_seek_file(fmt,vi,INT64_MIN,seekTargetPts,seekTargetPts,AVSEEK_FLAG_BACKWARD)<0) {
                seekPending=false; audioSeekTarget=AV_NOPTS_VALUE;
                saveStage("E11");
                waitForMessage(uiText("Recherche impossible"),uiText("Le fichier ne permet pas ce saut. La lecture reprend a sa position actuelle."));
            } else {
                avcodec_flush_buffers(vctx); if(actx) avcodec_flush_buffers(actx); if(ndspReady) ndspChnWaveBufClear(0); if(swr) { swr_close(swr); swr_init(swr); }
                for(int j=0;j<AUDIO_SLOTS;j++) audioWave[j].status=NDSP_WBUF_FREE;
                firstPts=AV_NOPTS_VALUE;
                videoLateMs=0; videoEcoMode=false;
                decodedFrames=0; shownFrames=0; readTicks=0; videoDecodeTicks=0;
                scaleTicks=0; renderTicks=0; lastStatsAt=osGetTime();
                textureCopyTicks=textureFlushTicks=0; measuredTextureFrames=0;
                audioCacheFlushTicks=0; measuredAudioCacheFlushes=0;
                audioCacheFlushFallbacks=0;
                frameSyncTicks=drawSubmitTicks=0; measuredDrawFrames=0;
                audioBufferGrowths=0;
                measuredReadPackets=0; measuredVideoFrames=0; measuredScaledFrames=0; measuredRenderedFrames=0;
                videoStable15Mode=sourceFps>15.5; cadenceAccumulator=0.0;
                lastCadencePts=AV_NOPTS_VALUE; haveCadenceFrame=false;
                cadenceSkippedFrames=0; lateSkippedFrames=0; audioPrioritySkippedFrames=0;
                vctx->skip_frame=AVDISCARD_DEFAULT;
                vctx->skip_loop_filter=AVDISCARD_NONREF;
                paused=false; if(ndspReady) ndspChnSetPaused(0,false);
            }
        }
        if(paused) { gspWaitForVBlank(); continue; }
        if(!markedFirstRead) {
            if(!fmt->pb || !fmt->iformat || !fmt->streams || !pkt) {
                saveStage("E15");
                waitForMessage("Lecture MP4 impossible","Le conteneur MP4 n'est pas pret pour la lecture.");
                stop=true; break;
            }
            saveStage("S4R");
        }
        if(debugVisible) {
            u64 readStarted=svcGetSystemTick();
            fferr=av_read_frame(fmt,pkt);
            readTicks+=svcGetSystemTick()-readStarted;
            measuredReadPackets++;
        } else fferr=av_read_frame(fmt,pkt);
        if(!markedFirstRead) { saveStage("S4P"); markedFirstRead=true; }
        if(fferr<0) {
            if(fferr!=AVERROR_EOF) { saveStage("E12"); waitForMessage("Lecture SD interrompue","Impossible de lire la suite du fichier MP4."); }
            stop=true; break;
        }
        AVCodecContext *ctx=nullptr; int stream=pkt->stream_index;
        if(stream==vi) ctx=vctx; else if(stream==ai) ctx=actx;
        if(ctx==vctx) {
            // Old 3DS has little CPU headroom. Enter the cheaper decode mode
            // only after roughly three to four frame intervals of lag. A
            // two-frame delay must not discard every non-reference picture.
            if(videoLateMs>lateEnterMs) {
                vctx->skip_frame=AVDISCARD_DEFAULT;
                vctx->skip_loop_filter=AVDISCARD_ALL;
                videoEcoMode=true;
            } else if(videoLateMs<lateExitMs) {
                vctx->skip_frame=AVDISCARD_DEFAULT;
                vctx->skip_loop_filter=AVDISCARD_NONREF;
                videoEcoMode=false;
            }
        }
        if(ctx==vctx && !markedVideoSend) { saveStage("S5V"); markedVideoSend=true; }
        if(ctx==actx && ctx && !markedAudioSend) { saveStage("S5A"); markedAudioSend=true; }
        if(ctx) {
            u64 decodeStarted=0;
            int sendResult;
            if(debugVisible && ctx==vctx) {
                decodeStarted=svcGetSystemTick();
                sendResult=avcodec_send_packet(ctx,pkt);
                videoDecodeTicks+=svcGetSystemTick()-decodeStarted;
            } else sendResult=avcodec_send_packet(ctx,pkt);
            if(sendResult>=0) {
                if(ctx==vctx && !markedVideoRecv) { saveStage("S5R"); markedVideoRecv=true; }
                if(ctx==actx && ctx && !markedAudioRecv) { saveStage("S5Q"); markedAudioRecv=true; }
                while(true) {
                    int receiveResult;
                    if(debugVisible && ctx==vctx) {
                        decodeStarted=svcGetSystemTick();
                        receiveResult=avcodec_receive_frame(ctx,frame);
                        videoDecodeTicks+=svcGetSystemTick()-decodeStarted;
                    } else receiveResult=avcodec_receive_frame(ctx,frame);
                    if(receiveResult<0) break;
                    if(ctx==vctx) {
                    currentPts=frame->best_effort_timestamp;
                    if(seekPending && currentPts<seekTargetPts) { av_frame_unref(frame); continue; }
                    decodedFrames++;
                    if(debugVisible) measuredVideoFrames++;
                    if(seekPending) { seekPending=false; firstPts=currentPts; wallStart=osGetTime(); debugAction="Lecture"; }
                    if(firstPts==AV_NOPTS_VALUE) { firstPts=currentPts; wallStart=osGetTime(); }
                    if(frame->width<=0 || frame->height<=0 || frame->format<0) {
                        saveStage("E09"); waitForMessage("Image invalide","Le decodeur n'a pas produit une image utilisable."); stop=true; av_frame_unref(frame); break;
                    }
                    u64 target=0;
                    if(frame->best_effort_timestamp!=AV_NOPTS_VALUE) {
                        int64_t elapsed=av_rescale_q(frame->best_effort_timestamp-firstPts,fmt->streams[vi]->time_base,AV_TIME_BASE_Q)/1000;
                        target=wallStart+(u64)std::max<int64_t>(0,elapsed);
                        videoLateMs=(int64_t)osGetTime()-(int64_t)target;
                    }
                    // Sample fast sources at a regular 15 fps before scaling and
                    // texture transfer. Use PTS deltas instead of assuming the
                    // decoder returns every nominal source frame; that keeps the
                    // cadence correct for VFR files and imperfect decode counts.
                    if(videoStable15Mode && sourceFps>15.5) {
                        bool cadenceSelected=true;
                        if(haveCadenceFrame) {
                            double cadenceAdvance=15.0/sourceFps;
                            if(currentPts!=AV_NOPTS_VALUE && lastCadencePts!=AV_NOPTS_VALUE && cadencePerTimestampTick>0.0) {
                                int64_t deltaPts=currentPts-lastCadencePts;
                                cadenceAdvance=deltaPts>0?(double)deltaPts*cadencePerTimestampTick:0.0;
                            }
                            // A large gap means the decoder itself missed time;
                            // show the newest available frame without carrying
                            // old cadence debt forward into a burst of pictures.
                            if(cadenceAdvance>=1.0) {
                                cadenceAccumulator=0.0;
                                cadenceSelected=true;
                            } else {
                                cadenceAccumulator+=cadenceAdvance;
                                cadenceSelected=cadenceAccumulator>=1.0;
                                if(cadenceSelected) cadenceAccumulator-=1.0;
                            }
                        } else haveCadenceFrame=true;
                        lastCadencePts=currentPts;
                        if(!cadenceSelected) {
                            cadenceSkippedFrames++;
                            av_frame_unref(frame);
                            continue;
                        }
                    }
                    // Legacy pressure drops apply only below the 15 fps mode.
                    // Applying them on top of the fixed cadence rejected a
                    // second set of frames and caused severe low-FPS playback.
                    if(!videoStable15Mode && target && videoLateMs>0) {
                        int queued=0;
                        if(actx) for(int j=0;j<AUDIO_SLOTS;j++)
                            if(audioWave[j].status==NDSP_WBUF_QUEUED || audioWave[j].status==NDSP_WBUF_PLAYING) queued++;
                        const int64_t frameIntervalMs=sourceFps>1.0?(int64_t)(1000.0/sourceFps):42;
                        const bool audioNeedsPackets=actx && queued<2 && videoLateMs>=frameIntervalMs;
                        if(videoLateMs>lateEnterMs) {
                            lateSkippedFrames++;
                            av_frame_unref(frame);
                            continue;
                        }
                        if(audioNeedsPackets) {
                            audioPrioritySkippedFrames++;
                            av_frame_unref(frame);
                            continue;
                        }
                    }
                    if(!sws || swsSourceW!=frame->width || swsSourceH!=frame->height || swsSourceFmt!=frame->format) {
                        saveStage("S0C");
                        if(sws) { sws_freeContext(sws); sws=nullptr; }
                        fit=std::min(1.0,std::min((double)MAX_W/frame->width,(double)MAX_H/frame->height));
                        renderW=std::max(2,((int)(frame->width*fit))&~1);
                        renderH=std::max(1,(int)(frame->height*fit));
                        sws=sws_getContext(frame->width,frame->height,(AVPixelFormat)frame->format,renderW,renderH,AV_PIX_FMT_RGB565LE,SWS_FAST_BILINEAR,nullptr,nullptr,nullptr);
                        if(!sws) { saveStage("E09"); waitForMessage("Image impossible","La conversion de l'image en 144p a echoue."); stop=true; av_frame_unref(frame); break; }
                        swsSourceW=frame->width; swsSourceH=frame->height; swsSourceFmt=frame->format;
                    }
                    uint8_t *dst[4]={rgb,nullptr,nullptr,nullptr}; int lines[4]={renderW*2,0,0,0};
                    if(!markedScale) { saveStage("S06"); markedScale=true; }
                    int scaledRows;
                    if(debugVisible) {
                        u64 scaleStarted=svcGetSystemTick();
                        scaledRows=sws_scale(sws,frame->data,frame->linesize,0,frame->height,dst,lines);
                        scaleTicks+=svcGetSystemTick()-scaleStarted;
                        measuredScaledFrames++;
                    } else scaledRows=sws_scale(sws,frame->data,frame->linesize,0,frame->height,dst,lines);
                    if(scaledRows<=0) {
                        saveStage("E13"); waitForMessage("Conversion impossible","Image non convertible en 144p."); stop=true; av_frame_unref(frame); break;
                    }
                    if(!markedDraw) { saveStage("S07"); markedDraw=true; }
                    if(debugVisible) {
                        u64 renderStarted=svcGetSystemTick();
                        showVideoFrame(renderW,renderH,rgb);
                        renderTicks+=svcGetSystemTick()-renderStarted;
                        measuredRenderedFrames++;
                    } else showVideoFrame(renderW,renderH,rgb);
                    shownFrames++;
                    if(target) {
                        while(aptMainLoop() && osGetTime()<target && !stop) {
                            hidScanInput(); u32 k=hidKeysDown();
                            if(k&KEY_SELECT) toggleDebug();
                            if(k&KEY_START) { startExitRequested=true; stop=true; break; }
                            if(k&KEY_B) { stop=true; break; }
                            if(k&KEY_DUP) changePlaybackBrightness(1);
                            if(k&KEY_DDOWN) changePlaybackBrightness(-1);
                            if(k&(KEY_DLEFT|KEY_DRIGHT)) { pendingSeekDirection=(k&KEY_DRIGHT)?1:-1; break; }
                            if((k&KEY_A) && osGetTime()-lastActionAt>=220) {
                                lastActionAt=osGetTime();
                                paused=!paused; debugAction=paused?"Pause":"Lecture"; if(ndspReady) ndspChnSetPaused(0,paused); if(debugVisible) showDebugPanel();
                                if(paused) {
                                    pauseStarted=osGetTime();
                                    while(aptMainLoop() && paused && !stop) {
                                        hidScanInput(); u32 pk=hidKeysDown();
                                        if(pk&KEY_SELECT) toggleDebug();
                                        if(pk&KEY_START) { startExitRequested=true; stop=true; break; }
                                        if(pk&KEY_B) { stop=true; break; }
                                        if(pk&(KEY_DLEFT|KEY_DRIGHT)) { pendingSeekDirection=(pk&KEY_DRIGHT)?1:-1; break; }
                                        if((pk&KEY_A) && osGetTime()-lastActionAt>=220) { lastActionAt=osGetTime(); paused=false; debugAction="Lecture"; if(ndspReady) ndspChnSetPaused(0,false); if(debugVisible) showDebugPanel(); }
                                        if(pk&KEY_DUP) changePlaybackBrightness(1);
                                        if(pk&KEY_DDOWN) changePlaybackBrightness(-1);
                                        gspWaitForVBlank();
                                    }
                                    if(!paused) wallStart+=osGetTime()-pauseStarted;
                                }
                            }
                            if(pendingSeekDirection) break;
                            if(actx && target-osGetTime()<150) {
                                int queued=0;
                                for(int j=0;j<AUDIO_SLOTS;j++)
                                    if(audioWave[j].status==NDSP_WBUF_QUEUED || audioWave[j].status==NDSP_WBUF_PLAYING) queued++;
                                if(queued<4) break; // Feed the audio queue before waiting for video.
                            }
                            gspWaitForVBlank();
                        }
                    }
                    } else if(actx && ctx==actx) {
                    int slot=-1;
                    bool pauseAfterQueue=false;
                    for(int j=0;j<AUDIO_SLOTS;j++) if(audioWave[j].status==NDSP_WBUF_FREE||audioWave[j].status==NDSP_WBUF_DONE){slot=j;break;}
                    // MP4 audio packets may arrive in a burst. Dropping one
                    // whenever the small NDSP queue filled caused audible gaps.
                    while(slot<0 && aptMainLoop() && !stop && !pendingSeekDirection && !paused) {
                        hidScanInput(); u32 k=hidKeysDown();
                        if(k&KEY_SELECT) toggleDebug();
                        if(k&KEY_START) { startExitRequested=true; stop=true; break; }
                        if(k&KEY_B) { stop=true; break; }
                        if(k&KEY_DUP) changePlaybackBrightness(1);
                        if(k&KEY_DDOWN) changePlaybackBrightness(-1);
                        if(k&(KEY_DLEFT|KEY_DRIGHT)) { pendingSeekDirection=(k&KEY_DRIGHT)?1:-1; break; }
                        if((k&KEY_A) && osGetTime()-lastActionAt>=220) {
                            lastActionAt=osGetTime(); pauseAfterQueue=true;
                        }
                        gspWaitForVBlank();
                        for(int j=0;j<AUDIO_SLOTS;j++) if(audioWave[j].status==NDSP_WBUF_FREE||audioWave[j].status==NDSP_WBUF_DONE){slot=j;break;}
                    }
                    if(slot>=0) {
                        int maxOut=(int)av_rescale_rnd(swr_get_delay(swr,actx->sample_rate)+frame->nb_samples,32728,actx->sample_rate,AV_ROUND_UP);
                        size_t bytes=(size_t)maxOut*2*sizeof(int16_t);
                        bool beforeSeek=audioSeekTarget!=AV_NOPTS_VALUE && frame->best_effort_timestamp!=AV_NOPTS_VALUE && frame->best_effort_timestamp<audioSeekTarget;
                        if(!beforeSeek) {
                            if(bytes>audioCapacity[slot]) {
                                uint8_t *larger=(uint8_t*)linearAlloc(bytes);
                                if(larger) {
                                    if(audioPcm[slot]) linearFree(audioPcm[slot]);
                                    audioPcm[slot]=larger; audioCapacity[slot]=bytes; audioBufferGrowths++;
                                }
                            }
                            if(audioPcm[slot] && bytes<=audioCapacity[slot]) { uint8_t *out[1]={audioPcm[slot]}; if(!markedAudio) { saveStage("S08"); markedAudio=true; } int n=swr_convert(swr,out,maxOut,(const uint8_t**)frame->extended_data,frame->nb_samples); if(n>0) { u64 flushStarted=debugVisible?svcGetSystemTick():0; bool usedFallback=flushAudioPcmCache(audioPcm[slot],(u32)((size_t)n*4)); if(debugVisible) { audioCacheFlushTicks+=svcGetSystemTick()-flushStarted; measuredAudioCacheFlushes++; if(usedFallback) audioCacheFlushFallbacks++; } audioWave[slot].data_vaddr=audioPcm[slot]; audioWave[slot].nsamples=n; audioWave[slot].status=NDSP_WBUF_FREE; ndspChnWaveBufAdd(0,&audioWave[slot]); } }
                        }
                    }
                    if(pauseAfterQueue && !stop && !pendingSeekDirection) {
                        paused=true; debugAction="Pause"; pauseStarted=osGetTime(); ndspChnSetPaused(0,true);
                    }
                    }
                    av_frame_unref(frame);
                    if(stop || pendingSeekDirection) break;
                }
            }
        }
        av_packet_unref(pkt);
        u64 statsNow=osGetTime();
        if(statsNow-lastStatsAt>=2000) {
            if(debugVisible) {
                double seconds=(statsNow-lastStatsAt)/1000.0;
                int queued=0;
                if(actx) for(int j=0;j<AUDIO_SLOTS;j++)
                    if(audioWave[j].status==NDSP_WBUF_QUEUED || audioWave[j].status==NDSP_WBUF_PLAYING) queued++;
                debugStats.decodeFps=decodedFrames/seconds;
                debugStats.imageFps=shownFrames/seconds;
                double readBase=measuredReadPackets?measuredReadPackets:1;
                double videoBase=measuredVideoFrames?measuredVideoFrames:1;
                double scaleBase=measuredScaledFrames?measuredScaledFrames:1;
                double renderBase=measuredRenderedFrames?measuredRenderedFrames:1;
                debugStats.readMsPerPacket=(double)readTicks*1000.0/SYSCLOCK_ARM11/readBase;
                debugStats.videoDecodeMsPerFrame=(double)videoDecodeTicks*1000.0/SYSCLOCK_ARM11/videoBase;
                debugStats.scaleMsPerFrame=(double)scaleTicks*1000.0/SYSCLOCK_ARM11/scaleBase;
                debugStats.renderPipelineMsPerFrame=(double)renderTicks*1000.0/SYSCLOCK_ARM11/renderBase;
                double textureBase=measuredTextureFrames?measuredTextureFrames:1;
                debugStats.textureCopyMsPerFrame=(double)textureCopyTicks*1000.0/SYSCLOCK_ARM11/textureBase;
                debugStats.textureFlushMsPerFrame=(double)textureFlushTicks*1000.0/SYSCLOCK_ARM11/textureBase;
                double audioFlushBase=measuredAudioCacheFlushes?measuredAudioCacheFlushes:1;
                debugStats.audioFlushMsPerBuffer=(double)audioCacheFlushTicks*1000.0/SYSCLOCK_ARM11/audioFlushBase;
                double drawBase=measuredDrawFrames?measuredDrawFrames:1;
                debugStats.frameSyncMsPerFrame=(double)frameSyncTicks*1000.0/SYSCLOCK_ARM11/drawBase;
                debugStats.drawSubmitMsPerFrame=(double)drawSubmitTicks*1000.0/SYSCLOCK_ARM11/drawBase;
                debugStats.audioQueued=queued;
                debugStats.audioSlots=actx?AUDIO_SLOTS:0;
                debugStats.audioBufferGrowths=audioBufferGrowths;
                debugStats.audioFlushFallbacks=audioCacheFlushFallbacks;
                audioBufferGrowths=0;
                debugStats.lateMs=(int)std::max<int64_t>(-9999,std::min<int64_t>(9999,videoLateMs));
                debugStats.cadenceSkipped=cadenceSkippedFrames;
                debugStats.lateSkipped=lateSkippedFrames;
                debugStats.audioPrioritySkipped=audioPrioritySkippedFrames;
                if(currentPts!=AV_NOPTS_VALUE && vi>=0)
                    debugStats.positionSeconds=std::max<int64_t>(0,av_rescale_q(currentPts,fmt->streams[vi]->time_base,AVRational{1,1}));
                updateDebugMetrics();
            }
            decodedFrames=0; shownFrames=0; readTicks=0; videoDecodeTicks=0;
            scaleTicks=0; renderTicks=0; lastStatsAt=statsNow;
            textureCopyTicks=textureFlushTicks=0; measuredTextureFrames=0;
            audioCacheFlushTicks=0; measuredAudioCacheFlushes=0;
            audioCacheFlushFallbacks=0;
            frameSyncTicks=drawSubmitTicks=0; measuredDrawFrames=0;
            measuredReadPackets=0; measuredVideoFrames=0; measuredScaledFrames=0; measuredRenderedFrames=0;
            cadenceSkippedFrames=0; lateSkippedFrames=0; audioPrioritySkippedFrames=0;
        }
        // The progress display has one-second precision; avoid needlessly
        // repainting the lower-screen console twice per second during decode.
        if(statsNow-lastProgressAt>=1000) {
            lastProgressAt=statsNow;
            if(currentPts!=AV_NOPTS_VALUE && vi>=0) {
                int64_t positionSeconds=av_rescale_q(currentPts,fmt->streams[vi]->time_base,AVRational{1,1});
                int64_t durationSeconds=mediaDurationUs>0?mediaDurationUs/AV_TIME_BASE:0;
                showPlaybackProgress(positionSeconds,durationSeconds);
            }
        }
    }
    if(ndspReady) { ndspChnWaveBufClear(0); ndspChnSetPaused(0,false); }
cleanup:
    if(ndspReady) { ndspChnWaveBufClear(0); ndspChnReset(0); }
    if(!ok && !playerErrorShown) { saveStage("E99"); waitForMessage("Lecture impossible","Memoire faible, fichier endommage ou profil non gere."); }
    if(swr) swr_free(&swr); if(sws) sws_freeContext(sws);
    if(vctx) avcodec_free_context(&vctx); if(actx) avcodec_free_context(&actx);
    if(cpuLimitRaised) APT_SetAppCpuTimeLimit(previousCpuTimeLimit);
    playbackCpuTimeLimit=previousCpuTimeLimit;
    if(fmt) avformat_close_input(&fmt);
    if(customIO) { av_freep(&customIO->buffer); avio_context_free(&customIO); }
    if(customFile) fclose(customFile);
    if(pkt) av_packet_free(&pkt); if(frame) av_frame_free(&frame); free(rgb);
    if(ok && !playerErrorShown) saveStage("OK");
    debugAction="Explorateur SD";
    return ok;
}
static void drawGuide(int page) {
    const char *titles[3]={"MP4 Player Old - Guide 1/3","Guide 2/3 - Commandes","Guide 3/3 - Depannage"};
    const char *bodies[3]={
        "Fichiers MP4 locaux depuis la SD.\nL'image est adaptee a l'ecran.\nLe lecteur utilise les codecs disponibles.\nUn codec trop lourd peut ralentir la 3DS.",
        "A : ouvrir / pause - B : retour / arreter\nGauche / droite : -5s / +5s a vitesse x1\nHaut/bas : luminosite en lecture uniquement\nSTART : quitter - tactile ignore.",
        "Chargement long : patienter quelques secondes.\nEcran noir ou erreur : codec non gere ou\nfichier endommage. Essayer un autre MP4.\nLes grandes videos peuvent etre plus lentes."
    };
    char footer[64]; snprintf(footer,sizeof(footer),uiText(page==2?"A : j'ai compris":"A : page suivante (%d/3)"),page+1);
    showTop(uiText(titles[page]),uiText(bodies[page]),footer);
}
static bool drawLanguagePrompt() {
    showTop(uiText("Choix de langue"),
            uiText(uiLanguage.french?"Votre langue est bien le francais ?\nSeules le francais et English sont disponibles.":"Votre langue est bien English ?\nSeules le francais et English sont disponibles."),
            uiText(uiLanguage.french?"A : oui   B : non / English":"A : oui / Francais   B : non"));
    consoleClear();
    printf("\x1b[36;1mSHIELD MP4 PLAYER\x1b[0m\n\n");
    printf("%s\n\n",uiText("Seules les langues francaise et English sont disponibles."));
    printf("%s\n",uiText("X maintenu au demarrage : choisir la langue"));
    printf("\n%s\n",uiText(uiLanguage.french?"A : oui   B : non / English":"A : oui / Francais   B : non"));
    return true;
}
static bool confirmUiLanguage(bool forcePrompt) {
    if(!uiLanguage.confirmation_needed && !forcePrompt) return true;
    if(!drawLanguagePrompt()) return false;
    while(aptMainLoop()) {
        hidScanInput();
        u32 down=hidKeysDown();
        if(down&KEY_START) return false;
        if(down&KEY_A) {
            uiLanguage.confirmation_needed=false;
            shield_ui_language_save(&uiLanguage);
            return true;
        }
        if(down&KEY_B) {
            uiLanguage.french=!uiLanguage.french;
            uiLanguage.confirmation_needed=false;
            if(!drawLanguagePrompt()) return false;
            shield_ui_language_save(&uiLanguage);
        }
        gspWaitForVBlank();
    }
    return false;
}
static void showBootLogo() {
    showIdleTop();
    const u64 start=osGetTime();
    u64 xHeldSince=0;
    while(aptMainLoop() && osGetTime()-start<5000) {
        hidScanInput();
        if(hidKeysHeld()&KEY_X) {
            if(!xHeldSince) xHeldSince=osGetTime();
            if(osGetTime()-xHeldSince>=1000) languagePromptRequested=true;
        } else xHeldSince=0;
        gspWaitForVBlank();
    }
}
static std::string shortName(const std::string &name,size_t limit) {
    if(name.size()<=limit) return name;
    size_t cut=limit;
    while(cut>0 && ((unsigned char)name[cut]&0xC0)==0x80) --cut;
    return name.substr(0,cut)+"...";
}
static void showBrowser() {
    consoleClear();
    printf("\x1b[36;1mSHIELD MP4 PLAYER v%s (build %d)\x1b[0m\n",APP_RELEASE_VERSION,APP_BUILD_VERSION);
    if(cachedBatteryValid) printf("\x1b[32m%s : %u%%\x1b[0m   %s : ",uiText("Batterie"),cachedBatteryPercent,uiText("Temperature"));
    else printf("\x1b[33m%s : %s\x1b[0m   %s : ",uiText("Batterie"),uiText("n/d"),uiText("Temperature"));
    if(cachedTempValid) printf("%u C\n",cachedTemp); else printf("%s\n",uiText("n/d"));
    if(!ndspReady) printf("\x1b[33m%s\x1b[0m\n",uiText("Audio indisponible (DSP)"));
    std::string location=shortName(currentDir.size()>6?currentDir.substr(6):"/",26);
    printf("\x1b[33m%s %s\x1b[0m\n",uiText("Dossier :"),location.c_str());
    printf(uiText("SELECT : diagnostic | %u elements\n"),(unsigned)entries.size());
    if(browserReadError) printf("\x1b[31m%s\x1b[0m\n",uiText("Dossier inaccessible. B : remonter"));
    else if(entries.empty()) printf("\x1b[90m%s\x1b[0m\n",uiText("Aucun dossier ou MP4 ici."));
    size_t first=(selected/16)*16;
    for(size_t i=first;i<entries.size()&&i<first+16;i++) {
        const BrowserEntry &entry=entries[i];
        printf(i==selected?"\x1b[30;103m> ":"  ");
        printf("%s",entry.directory?"\x1b[36m":"\x1b[37m");
        printf("%s",uiText(entry.directory?"[DOSSIER] ":"[MP4] "));
        std::string label=shortName(entry.name,24);
        printf("%s\x1b[0m\n",label.c_str());
    }
    printf("\n\x1b[32m%s\x1b[0m   \x1b[33m%s\x1b[0m\n%s",uiText("A : ouvrir / lire"),uiText("B : dossier parent"),uiText("Haut/bas : selectionner | Gauche/droite : page\nSTART : quitter | Luminosite en lecture\n"));
}
static void showDebugPanel() {
    consoleClear();
    printf("\x1b[36;1m%s\x1b[0m\n",uiText("SHIELD / DIAGNOSTIC"));
    printf("%s\n\n",uiText("SELECT : retour explorateur"));
    printf(uiText("Action : %s\n"),uiText(debugAction));
    std::string name=debugCurrentFile;
    size_t slash=name.find_last_of("/\\");
    if(slash!=std::string::npos) name=name.substr(slash+1);
    printf(uiText("Video : %s\n"),name.empty()?uiText("aucune"):shortName(name,30).c_str());
    printf(uiText("Code : %s\n"),lastStage);
    printf(uiText("Pistes : %.32s\n"),lastDetail);
    printf(uiText("Probleme : %s\n"),debugProblem.empty()?uiText("aucun"):shortName(debugProblem,29).c_str());
    printf(uiText("Echant. C:%u | pertes R:%u A:%u\n"),debugStats.cadenceSkipped,debugStats.lateSkipped,debugStats.audioPrioritySkipped);
    printf(uiText("Position : %lld / %lld s\n"),(long long)debugStats.positionSeconds,(long long)debugStats.durationSeconds);
    printf(uiText("FPS fichier : %.1f\n"),debugStats.sourceFps);
    printf(uiText("FPS recues  : %.1f\n"),debugStats.decodeFps);
    printf(uiText("FPS envoyees: %.1f\n"),debugStats.imageFps);
    printf(uiText("Mode video : %s\n"),uiText(videoStable15Mode?"cadence stable 15 FPS":(videoEcoMode?"ECO (filtre de boucle reduit)":"normal")));
    printf(uiText("Tampons audio : %d/%d\n"),debugStats.audioQueued,debugStats.audioSlots);
    printf(uiText("Retard image : %d ms\n"),debugStats.lateMs);
    printf(uiText("I/O pkt %.1f | appels codec %.1f ms\n"),debugStats.readMsPerPacket,debugStats.videoDecodeMsPerFrame);
    printf(uiText("Scale %.1f | transf+GPU %.1f ms\n"),debugStats.scaleMsPerFrame,debugStats.renderPipelineMsPerFrame);
    printf(uiText("Copie/flush texture : %.1f / %.1f ms\n"),debugStats.textureCopyMsPerFrame,debugStats.textureFlushMsPerFrame);
    printf(uiText("Flush audio par bloc : %.1f ms\n"),debugStats.audioFlushMsPerBuffer);
    printf(uiText("Agrandissements PCM : %u\n"),debugStats.audioBufferGrowths);
    printf(uiText("Luminosite : %d/5\n"),brightness);
    printf(uiText("Quota CPU app (max) : %u%%\n"),(unsigned)playbackCpuTimeLimit);
    printf(uiText("Sync GPU / draw : %.1f / %.1f ms\n"),debugStats.frameSyncMsPerFrame,debugStats.drawSubmitMsPerFrame);
    printf(uiText("Repli flush DSP : %u\n"),debugStats.audioFlushFallbacks);
    printf(uiText("\nCes mesures changent toutes les 2 s.\n"));
}
static void updateDebugMetrics() {
    if(!debugVisible) return;
    printf("\x1b[4;1H"); printf(uiText("Action : %-30.30s"),uiText(debugAction));
    printf("\x1b[6;1H"); printf(uiText("Code : %-32.32s"),lastStage);
    printf("\x1b[9;1H"); printf(uiText("Echant. C:%u | pertes R:%u A:%u              "),debugStats.cadenceSkipped,debugStats.lateSkipped,debugStats.audioPrioritySkipped);
    printf("\x1b[10;1H"); printf(uiText("Position : %lld / %lld s          "),(long long)debugStats.positionSeconds,(long long)debugStats.durationSeconds);
    printf("\x1b[11;1H"); printf(uiText("FPS fichier : %.1f                    "),debugStats.sourceFps);
    printf("\x1b[12;1H"); printf(uiText("FPS recues  : %.1f                    "),debugStats.decodeFps);
    printf("\x1b[13;1H"); printf(uiText("FPS envoyees: %.1f                    "),debugStats.imageFps);
    printf("\x1b[14;1H"); printf(uiText("Mode video : %-30s"),uiText(videoStable15Mode?"cadence stable 15 FPS":(videoEcoMode?"ECO (filtre de boucle reduit)":"normal")));
    printf("\x1b[15;1H"); printf(uiText("Tampons audio : %d/%d                 "),debugStats.audioQueued,debugStats.audioSlots);
    printf("\x1b[16;1H"); printf(uiText("Retard image : %d ms                 "),debugStats.lateMs);
    printf("\x1b[17;1H"); printf(uiText("I/O pkt %.1f | appels codec %.1f ms        "),debugStats.readMsPerPacket,debugStats.videoDecodeMsPerFrame);
    printf("\x1b[18;1H"); printf(uiText("Scale %.1f | transf+GPU %.1f ms                "),debugStats.scaleMsPerFrame,debugStats.renderPipelineMsPerFrame);
    printf("\x1b[19;1H"); printf(uiText("Copie/flush texture : %.1f / %.1f ms       "),debugStats.textureCopyMsPerFrame,debugStats.textureFlushMsPerFrame);
    printf("\x1b[20;1H"); printf(uiText("Flush audio par bloc : %.1f ms            "),debugStats.audioFlushMsPerBuffer);
    printf("\x1b[21;1H"); printf(uiText("Agrandissements PCM : %u                  "),debugStats.audioBufferGrowths);
    printf("\x1b[22;1H"); printf(uiText("Quota CPU app (max) : %u%%                  "),(unsigned)playbackCpuTimeLimit);
    printf("\x1b[23;1H"); printf(uiText("Sync GPU / draw : %.1f / %.1f ms        "),debugStats.frameSyncMsPerFrame,debugStats.drawSubmitMsPerFrame);
    printf("\x1b[24;1H"); printf(uiText("Repli flush DSP : %u                  "),debugStats.audioFlushFallbacks);
}
static void showLower() {
    if(debugVisible) showDebugPanel(); else showBrowser();
}
int main() {
    av_log_set_level(AV_LOG_QUIET);
    gfxInitDefault(); romfsInit();
    gfxSet3D(false);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE); C2D_Init(C2D_DEFAULT_MAX_OBJECTS); C2D_Prepare();
    topTarget=C2D_CreateScreenTarget(GFX_TOP,GFX_LEFT);
    textBuf=C2D_TextBufNew(4096);
    shieldSheet=C2D_SpriteSheetLoad("romfs:/gfx/shield.t3x");
    if(shieldSheet) shieldImage=C2D_SpriteSheetGetImage(shieldSheet,0);
    videoTextureReady=C3D_TexInit(&videoTex,256,256,GPU_RGB565);
    if(videoTextureReady) C3D_TexSetFilter(&videoTex,GPU_LINEAR,GPU_LINEAR);
    consoleInit(GFX_BOTTOM,nullptr);
    mkdir("sdmc:/3ds",0777); mkdir("sdmc:/3ds/MP4Player",0777);
    shield_ui_language_load(&uiLanguage,LANGUAGE_FILE,false);
    loadStage();
    aptSetHomeAllowed(false);
    // Keep HOME disabled but restore normal sleep handling for the physical
    // POWER button and lid. The app no longer traps a sleep request as a hang.
    aptSetSleepAllowed(true);
    wifiServiceReady = R_SUCCEEDED(nwmExtInit());
    if(wifiServiceReady) NWMEXT_ControlWirelessEnabled(false);
    mcuHwcInit(); gspLcdInit(); ndspReady=R_SUCCEEDED(ndspInit());
    if(ndspReady) { ndspSetOutputMode(NDSP_OUTPUT_STEREO); ndspChnSetFormat(0,NDSP_FORMAT_STEREO_PCM16); ndspChnSetRate(0,32728.0f); ndspChnSetInterp(0,NDSP_INTERP_LINEAR); }
    showBootLogo();
    bool languageConfirmed=confirmUiLanguage(languagePromptRequested);
    if(!languageConfirmed) startExitRequested=true;
    if(languageConfirmed && (languagePromptRequested || !fileExists(GUIDE_FILE))) {
        int page=0; drawGuide(page);
        while(aptMainLoop()) { hidScanInput(); u32 down=hidKeysDown(); if(down&KEY_START) { startExitRequested=true; break; } if(down&KEY_A) { if(page<2) { page++; drawGuide(page); } else { FILE *f=fopen(GUIDE_FILE,"wb"); if(f){fputs("1",f);fclose(f);} break; } } gspWaitForVBlank(); }
    }
    u64 lastInfo=0;
    if(startExitRequested) goto shutdown;
    showTop("Chargement","Ouverture du navigateur SD...","Veuillez patienter");
    loadCurrentDirectory(); refreshSystemInfo(); showLower(); showIdleTop();
    lastInfo=osGetTime();
    while(aptMainLoop()) {
        u64 now=osGetTime();
        hidScanInput(); u32 down=hidKeysDown(), repeat=hidKeysDownRepeat();
        if(down&KEY_START) break;
        if(down&KEY_SELECT) toggleDebug();
        bool browserChanged=false;
        if(!entries.empty()) {
            if(repeat&KEY_DUP) { selected=(selected+entries.size()-1)%entries.size(); browserChanged=true; }
            else if(repeat&KEY_DDOWN) { selected=(selected+1)%entries.size(); browserChanged=true; }
            else if(repeat&KEY_DLEFT) { selected=selected>=16?selected-16:0; browserChanged=true; }
            else if(repeat&KEY_DRIGHT) { selected=std::min(selected+16,entries.size()-1); browserChanged=true; }
        }
        if(down&KEY_B) {
            if(!browserHistory.empty()) {
                BrowserSnapshot &parent=browserHistory.back();
                currentDir=parent.path;
                entries.swap(parent.entries);
                selected=parent.selected;
                browserReadError=false;
                browserHistory.pop_back();
            }
            browserChanged=true;
        }
        if((down&KEY_A) && !entries.empty()) {
            BrowserEntry entry=entries[selected];
            if(entry.directory) {
                showTop("Chargement","Ouverture du dossier...","Veuillez patienter");
                browserHistory.emplace_back();
                browserHistory.back().path=currentDir;
                browserHistory.back().selected=selected;
                browserHistory.back().entries.swap(entries);
                currentDir=entry.path;
                loadCurrentDirectory();
                showIdleTop();
                browserChanged=true;
            } else {
                playFile(entry.path); if(startExitRequested) break; showIdleTop(); browserChanged=true;
            }
        }
        if(now-lastInfo>=20000) { lastInfo=now; refreshSystemInfo(); browserChanged=true; }
        if(browserChanged) showLower();
        gspWaitForVBlank();
    }
shutdown:
    // The player disables Wi-Fi for playback, so restore it before returning
    // to HOME Menu. Using NWMEXT lets NWM rebuild its managed wireless state.
    if(!wifiServiceReady) wifiServiceReady = R_SUCCEEDED(nwmExtInit());
    if(wifiServiceReady) {
        Result wifiRestoreResult = NWMEXT_ControlWirelessEnabled(true);
        nwmExtExit();
        wifiServiceReady = false;
        if(R_FAILED(wifiRestoreResult)) svcSetWifiEnabled(true);
    } else {
        svcSetWifiEnabled(true);
    }
    if(videoTextureReady) C3D_TexDelete(&videoTex);
    for(int i=0;i<AUDIO_SLOTS;i++) { if(ndspReady && (audioWave[i].status==NDSP_WBUF_PLAYING||audioWave[i].status==NDSP_WBUF_QUEUED)) ndspChnWaveBufClear(0); linearFree(audioPcm[i]); audioPcm[i]=nullptr; audioCapacity[i]=0; }
    if(shieldSheet) C2D_SpriteSheetFree(shieldSheet);
    C2D_TextBufDelete(textBuf); C2D_Fini(); C3D_Fini(); if(ndspReady) ndspExit(); gspLcdExit(); mcuHwcExit(); romfsExit(); gfxExit();
    return 0;
}
