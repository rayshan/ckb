#include "device.h"
#include "devnode.h"
#include "input.h"
#include "led.h"
#include "notify.h"

extern int features_mask;

// Timespec utility function
void timespec_add(struct timespec* timespec, long nanoseconds){
    nanoseconds += timespec->tv_nsec;
    timespec->tv_sec += nanoseconds / 1000000000;
    timespec->tv_nsec = nanoseconds % 1000000000;
}

// Not supported on OSX...
#ifdef OS_MAC
#define pthread_mutex_timedlock(mutex, timespec) pthread_mutex_lock(mutex)
#endif

void quit(){
    // Wait at most 1s for mutex locks. Better to crash than to freeze shutting down.
    struct timespec timeout = { 1, 0 };
    pthread_mutex_timedlock(&kblistmutex, &timeout);
    for(int i = 1; i < DEV_MAX; i++){
        // Before closing, set all keyboards back to HID input mode so that the stock driver can still talk to them
        if(IS_CONNECTED(keyboard + i)){
            pthread_mutex_timedlock(&keyboard[i].mutex, &timeout);
            // Stop the uinput device now to ensure no keys get stuck
            inputclose(keyboard + i);
            revertusb(keyboard + i);
            closeusb(keyboard + i);
        }
    }
    pthread_mutex_timedlock(&keyboard[0].mutex, &timeout);
    closeusb(keyboard);
    usbdeinit();
    pthread_mutex_unlock(&kblistmutex);
}

void sighandler2(int type){
    printf("\nIgnoring signal %d (already shutting down)\n", type);
}

void sighandler(int type){
    signal(SIGTERM, sighandler2);
    signal(SIGINT, sighandler2);
    signal(SIGQUIT, sighandler2);
    printf("\nCaught signal %d\n", type);
    quit();
    exit(0);
}

pthread_t sigthread;

void* sigmain(void* context){
    // Allow signals in this thread
    sigset_t signals;
    sigfillset(&signals);
    sigdelset(&signals, SIGTERM);
    sigdelset(&signals, SIGINT);
    sigdelset(&signals, SIGQUIT);
    // Set up signal handlers for quitting the service.
    pthread_sigmask(SIG_SETMASK, &signals, 0);
    signal(SIGTERM, sighandler);
    signal(SIGINT, sighandler);
    signal(SIGQUIT, sighandler);
    while(1){
        sleep(-1);
    }
    return 0;
}

void localecase(char* dst, size_t length, const char* src){
    char* ldst = dst + length;
    char s;
    while((s = *src++)){
        if(s == '_')
            s = '-';
        else
            s = tolower(s);
        *dst++ = s;
        if(dst == ldst){
            dst--;
            break;
        }
    }
    *dst = 0;
}

int main(int argc, char** argv){
    printf("ckb Corsair Keyboard RGB driver %s\n", CKB_VERSION_STR);

    // Check PID, quit if already running
    char pidpath[strlen(devpath) + 6];
    snprintf(pidpath, sizeof(pidpath), "%s0/pid", devpath);
    FILE* pidfile = fopen(pidpath, "r");
    if(pidfile){
        pid_t pid;
        fscanf(pidfile, "%d", &pid);
        fclose(pidfile);
        if(pid > 0){
            // kill -s 0 checks if the PID is active but doesn't send a signal
            if(!kill(pid, 0)){
                printf("ckb-daemon is already running (PID %d). Try killing the existing process first.\n(If this is an error, delete %s and try again)\n", pid, pidpath);
                return 0;
            }
        }
    }

    // Read parameters
    int forceroot = 1;
    for(int i = 1; i < argc; i++){
        char* argument = argv[i];
        char layout[10];
        unsigned newfps, newgid;
        if(sscanf(argument, "--fps=%u", &newfps) == 1){
            // Set FPS
            setfps(newfps);
        } else if(sscanf(argument, "--layout=%9s", layout) == 1){
            // Set keyboard layout
            keymap_system = getkeymap(layout);
            if(keymap_system)
                printf("Setting default layout: %s\n", layout);
        } else if(sscanf(argument, "--gid=%u", &newgid) == 1){
            // Set dev node GID
            gid = newgid;
            printf("Setting /dev node gid: %u\n", newgid);
        } else if(!strcmp(argument, "--nobind")){
            // Disable key notifications and rebinding
            features_mask &= ~FEAT_BIND & ~FEAT_NOTIFY;
            printf("Key binding and key notifications are disabled\n");
        } else if(!strcmp(argument, "--nonotify")){
            // Disable key notifications
            features_mask &= ~FEAT_NOTIFY;
            printf("Key notifications are disabled\n");
        } else if(!strcmp(argument, "--nonroot")){
            // Allow running as a non-root user
            forceroot = 0;
        }
    }

    // Check UID
    if(getuid() != 0){
        if(forceroot){
            printf("Fatal: ckb-daemon must be run as root. Try `sudo %s`\n", argv[0]);
            exit(0);
        } else
            printf("Warning: not running as root, allowing anyway per command-line parameter...\n");
    }

    // Set FPS if not done already
    if(!fps)
        setfps(30);

    // If the keymap wasn't set via command-line, get it from the system locale
    if(!keymap_system){
        setlocale(LC_ALL, "");
        const char* loc = setlocale(LC_CTYPE, 0);
        char locale[strlen(loc) + 1];
        localecase(locale, sizeof(locale), loc);
        if(strstr(locale, "de-")){
            // Check for DE layout
            keymap_system = keymap_de;
            printf("Setting default layout: de\n");
        } else if(strstr(locale, "es-")){
            // Check for ES layout
            keymap_system = keymap_es;
            printf("Setting default layout: es\n");
        } else if(strstr(locale, "fr-")){
            // Check for FR layout
            keymap_system = keymap_fr;
            printf("Setting default layout: fr\n");
        } else if(strstr(locale, "sv-")){
            // Check for SE layout
            keymap_system = keymap_se;
            printf("Setting default layout: se\n");
        } else if(strstr(locale, "en-us")
                  || strstr(locale, "en-au")
                  || strstr(locale, "en-ca")
                  || strstr(locale, "en-hk")
                  || strstr(locale, "en-in")
                  || strstr(locale, "en-nz")
                  || strstr(locale, "en-ph")
                  || strstr(locale, "en-sg")
                  || strstr(locale, "en-za")){
            // Check for US layout
            keymap_system = keymap_us;
            printf("Setting default layout: us\n");
        } else {
            // Default to GB
            keymap_system = keymap_gb;
            printf("Setting default layout: gb\n");
        }
    }

    // Make root keyboard
    umask(0);
    memset(keyboard, 0, sizeof(keyboard));
    pthread_mutex_init(&keyboard[0].mutex, 0);
    keyboard[0].model = -1;
    keyboard[0].features = FEAT_NOTIFY & features_mask;
    if(!makedevpath(keyboard))
        printf("Root controller ready at %s0\n", devpath);

    // Don't let any spawned threads handle signals
    sigset_t signals;
    sigfillset(&signals);
    pthread_sigmask(SIG_SETMASK, &signals, 0);

    // Start the USB system
    if(usbinit()){
        quit();
        return -1;
    }

    // Start the signal handling thread
    pthread_create(&sigthread, 0, sigmain, 0);

    int v120 = 0;
    struct timespec time, nexttime;
    while(1){
        clock_gettime(CLOCK_MONOTONIC, &time);
        pthread_mutex_lock(&kblistmutex);
        // Process commands for root controller
        if(keyboard[0].infifo){
            const char* line;
            if(readlines(keyboard[0].infifo, &line))
                readcmd(keyboard, line);
        }
        // Run the USB queue. Messages must be queued because sending multiple messages at the same time can cause the interface to freeze
        for(int i = 0; i < DEV_MAX; i++){
            if(IS_CONNECTED(keyboard + i)){
                pthread_mutex_lock(&keyboard[i].mutex);
                if(usbdequeue(keyboard + i) == 0
                        && usb_tryreset(keyboard + i)){
                    // If it failed and couldn't be reset, close the keyboard
                    closeusb(keyboard + i);
                } else {
                    if(keyboard[i].queuecount == 0){
                        // Process FIFOs
                        for(int i = 0; i < DEV_MAX; i++){
                            if(keyboard[i].infifo){
                                const char* line;
                                if(readlines(keyboard[i].infifo, &line))
                                    readcmd(keyboard + i, line);
                                if(keyboard[i].fwversion >= 0x0120)
                                    v120 = 1;
                            }
                        }
                        // Update indicator LEDs for this keyboard. These are polled rather than processed during events because they don't update
                        // immediately and may be changed externally by the OS.
                        updateindicators(keyboard + i, 0);
                    }
                    pthread_mutex_unlock(&keyboard[i].mutex);
                }
            }
        }
        pthread_mutex_unlock(&kblistmutex);
        // Sleep for long enough to achieve the desired frame rate (5 packets per frame).
        memcpy(&nexttime, &time, sizeof(time));
        timespec_add(&nexttime, 1000000000 / fps / (v120 ? 12 : 5));
        // Don't ever sleep for less than 100µs. It can lock the keyboard. Restart the sleep if it gets interrupted.
        clock_gettime(CLOCK_MONOTONIC, &time);
        timespec_add(&time, 100000);
        if(timespec_gt(nexttime, time))
            while(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &nexttime, 0) == EINTR);
        else
            while(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &time, 0) == EINTR);
    }
    quit();
    return 0;
}
