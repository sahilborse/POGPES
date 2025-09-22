/*
  universal_terminal_full.c
  Universal terminal (full mapping set requested)
  - Supports user dialect choice: Windows (cmd) or Linux (bash)
  - Detects host OS at compile time
  - Translates many common commands (with parameters) from source dialect to host dialect
  - Keeps history and supports !! and !<num>
  - Uses system() to execute translated commands
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#define HOST_IS_WINDOWS 1
#else
#define HOST_IS_WINDOWS 0
#endif

#define MAX_LINE 8192
#define MAX_TOK 256
#define MAX_HISTORY 1000
// Cross-platform strtok alias
#if defined(_WIN32) || defined(_WIN64)
    // On Windows, fallback to strtok (not thread-safe, but fine here)
    #define STRTOK(str, delim, saveptr) strtok((str), (delim))
#else
    // On Linux/Unix, use thread-safe strtok_r
    #define STRTOK(str, delim, saveptr) strtok_r((str), (delim), (saveptr))
#endif


// History buffer
static char *history[MAX_HISTORY];
static int hist_count = 0;

static void add_history(const char *cmd){
    if(!cmd || strlen(cmd)==0) return;
    if(hist_count < MAX_HISTORY){
        history[hist_count++] = strdup(cmd);
    } else {
        // rotate: free oldest
        free(history[0]);
        memmove(history, history+1, sizeof(char*)*(MAX_HISTORY-1));
        history[MAX_HISTORY-1] = strdup(cmd);
    }
}

static void print_history(){
    int start = hist_count>100? hist_count-100 : 0;
    for(int i=start;i<hist_count;i++){
        printf("%d  %s\n", i+1, history[i]);
    }
}

// trim
static void trim(char *s){
    if(!s) return;
    int i = 0;
    // trim leading
    while(s[i] && isspace((unsigned char)s[i])) i++;
    if(i) memmove(s, s+i, strlen(s+i)+1);
    // trim trailing
    int len = strlen(s);
    while(len>0 && isspace((unsigned char)s[len-1])) s[--len]=0;
}

// lowercase copy
static void lc_copy(const char *in, char *out){
    while(*in) { *out = tolower((unsigned char)*in); in++; out++; }
    *out = 0;
}

// split first token and rest
static void split_first(const char *in, char *first, char *rest){
    first[0]=0; rest[0]=0;
    const char *p = in;
    while(*p && isspace((unsigned char)*p)) p++;
    if(!*p) return;
    // handle quoted token
    if(*p=='\'' || *p=='"'){
        char q = *p++;
        const char *start = p;
        while(*p && *p!=q) p++;
        strncpy(first, start, p-start);
        first[p-start]=0;
        if(*p) p++;
    } else {
        const char *start = p;
        while(*p && !isspace((unsigned char)*p)) p++;
        strncpy(first, start, p-start);
        first[p-start]=0;
    }
    while(*p && isspace((unsigned char)*p)) p++;
    strncpy(rest, p, MAX_LINE-1);
    rest[MAX_LINE-1]=0;
}

// Replace substring (first occurrence). Returns 1 if replaced.
static int replace_first(char *s, const char *old, const char *new){
    char *pos = strstr(s, old);
    if(!pos) return 0;
    char buf[MAX_LINE];
    int prefix_len = pos - s;
    snprintf(buf, sizeof(buf), "%.*s%s%s", prefix_len, s, new, pos + strlen(old));
    strncpy(s, buf, MAX_LINE-1);
    s[MAX_LINE-1]=0;
    return 1;
}

// Build command mapping. source_is_windows: dialect user types. host_is_windows: current platform.
static char *map_command(const char *input, int source_is_windows, int host_is_windows){
    // If same dialect as host, return copy
    if(source_is_windows == host_is_windows) return strdup(input);

    // We'll attempt best-effort map: change first token and common flags/subpatterns.
    char first[MAX_TOK], rest[MAX_LINE];
    split_first(input, first, rest);
    char first_lc[MAX_TOK];
    lc_copy(first, first_lc);

    char mapped[MAX_LINE*2];
    mapped[0]=0;

    // Helper macros to set mapped easily
    #define SETM(fmt,...) do{ snprintf(mapped, sizeof(mapped), fmt, ##__VA_ARGS__); } while(0)
    #define APPREST() do{ if(strlen(rest)) { if(strlen(mapped)) strncat(mapped, " ", sizeof(mapped)-strlen(mapped)-1); strncat(mapped, rest, sizeof(mapped)-strlen(mapped)-1); } } while(0)

    // Linux -> Windows mappings
    if(!source_is_windows && host_is_windows){
        // Most common
        if(strcmp(first_lc,"pwd")==0){ SETM("cd"); return strdup(mapped); }
        if(strcmp(first_lc,"ls")==0){
            // handle flags in rest: -l, -a
            if(strstr(rest,"-l") && strstr(rest,"-a")) { SETM("dir /a /q"); APPREST(); return strdup(mapped); }
            if(strstr(rest,"-l")) { SETM("dir"); APPREST(); return strdup(mapped); }
            if(strstr(rest,"-a")) { SETM("dir /a"); APPREST(); return strdup(mapped); }
            SETM("dir"); APPREST(); return strdup(mapped);
        }
        if(strcmp(first_lc,"mkdir")==0){ SETM("mkdir"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"rmdir")==0){ SETM("rmdir"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"rm")==0){
            // rm -r <dir>
            if(strstr(rest,"-r") || strstr(rest,"-rf")){ // rmdir /s /q
                // remove -r from rest to get target
                char t[MAX_LINE]; strncpy(t, rest, sizeof(t));
                replace_first(t, "-r", ""); replace_first(t, "-rf", "");
                SETM("rmdir /s /q"); if(strlen(t)){ strncat(mapped," ", sizeof(mapped)-strlen(mapped)-1); strncat(mapped,t, sizeof(mapped)-strlen(mapped)-1);} return strdup(mapped);
            } else {
                SETM("del"); APPREST(); return strdup(mapped);
            }
        }
        if(strcmp(first_lc,"touch")==0){
            // type nul > file
            if(strlen(rest)){
                char t[MAX_LINE]; strncpy(t, rest, sizeof(t));
                trim(t);
                SETM("type nul >"); strncat(mapped, " ", sizeof(mapped)-strlen(mapped)-1); strncat(mapped, t, sizeof(mapped)-strlen(mapped)-1); return strdup(mapped);
            } else { return strdup("rem touch: missing filename"); }
        }
        if(strcmp(first_lc,"cp")==0){ SETM("copy"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"mv")==0){ SETM("move"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"cat")==0){ SETM("type"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"less")==0 || strcmp(first_lc,"more")==0){ SETM("more"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"head")==0){
            // head -n N file -> powershell Get-Content file -TotalCount N
            char sec[MAX_TOK]; split_first(rest, sec, sec); // not perfect
            char *nptr = strstr(rest, "-n");
            if(nptr){
                int N;
                if(sscanf(nptr, "-n %d", &N)==1){
                    char file[MAX_LINE]; // attempt to extract file (last token)
                    // simple heuristic: last whitespace separated token
                    char temp[MAX_LINE]; strncpy(temp, rest, sizeof(temp));
                    trim(temp);
                    char *last = strrchr(temp, ' ');
                    if(last) strcpy(file, last+1); else strcpy(file, temp);
                    snprintf(mapped, sizeof(mapped), "powershell -Command \"Get-Content %s -TotalCount %d\"", file, N);
                    return strdup(mapped);
                }
            }
            // fallback: more +N not reliable; use head via PowerShell reading first lines
            if(strlen(rest)){ snprintf(mapped,sizeof(mapped),"powershell -Command \"Get-Content %s -TotalCount 10\"", rest); return strdup(mapped); }
            SETM("more"); return strdup(mapped);
        }
        if(strcmp(first_lc,"tail")==0){
            // tail -f -> powershell Get-Content -Wait; tail -n -> Get-Content -Tail
            if(strstr(rest,"-f") || strstr(rest,"-F")){
                // extract filename
                char temp[MAX_LINE]; strncpy(temp, rest, sizeof(temp));
                replace_first(temp, "-f", ""); replace_first(temp, "-F", "");
                trim(temp);
                snprintf(mapped,sizeof(mapped),"powershell -Command \"Get-Content %s -Wait\"", temp);
                return strdup(mapped);
            }
            char *nptr = strstr(rest, "-n");
            if(nptr){
                int N;
                if(sscanf(nptr, "-n %d", &N)==1){
                    char temp[MAX_LINE]; strncpy(temp, rest, sizeof(temp));
                    // remove -n ... to get filename
                    char *fn = NULL;
                    char *tok = strtok(temp, " ");
                    while(tok){ fn = tok; tok = strtok(NULL," "); }
                    if(fn){ snprintf(mapped,sizeof(mapped),"powershell -Command \"Get-Content %s -Tail %d\"", fn, N); return strdup(mapped); }
                }
            }
            SETM("powershell -Command \"Get-Content "); APPREST(); strncat(mapped, " -Tail 10\"", sizeof(mapped)-strlen(mapped)-1); return strdup(mapped);
        }
        if(strcmp(first_lc,"chmod")==0){ SETM("rem chmod not supported on Windows; use icacls or powershell Set-Acl"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"chown")==0){ SETM("rem chown not supported on Windows; use icacls"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"whoami")==0){ SETM("whoami"); return strdup(mapped); }
        if(strcmp(first_lc,"uname")==0){ SETM("systeminfo"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"hostname")==0){ SETM("hostname"); return strdup(mapped); }
        if(strcmp(first_lc,"date")==0){ SETM("date /t"); return strdup(mapped); }
        if(strcmp(first_lc,"uptime")==0){ SETM("net statistics workstation"); return strdup(mapped); }
        if(strcmp(first_lc,"df")==0){
            // df -h -> wmic logicaldisk get size,freespace,caption (legacy)
            SETM("wmic logicaldisk get caption,freespace,size"); return strdup(mapped);
        }
        if(strcmp(first_lc,"du")==0){
            // du -sh dir -> powershell Get-ChildItem dir -Recurse | Measure-Object -Property Length -Sum
            char target[MAX_LINE]; strncpy(target, rest, sizeof(target)); trim(target);
            if(strlen(target)){
                snprintf(mapped,sizeof(mapped),"powershell -Command \"(Get-ChildItem -Recurse %s | Measure-Object -Property Length -Sum).Sum\"", target);
                return strdup(mapped);
            } else { SETM("rem du needs directory"); return strdup(mapped); }
        }
        if(strcmp(first_lc,"free")==0){ SETM("systeminfo | findstr /C:\"Total Physical Memory\" /C:\"Available\""); return strdup(mapped); }
        if(strcmp(first_lc,"top")==0 || strcmp(first_lc,"htop")==0){
            SETM("tasklist"); return strdup(mapped);
        }
        if(strcmp(first_lc,"ps")==0){
            if(strstr(rest,"aux")){ SETM("tasklist"); return strdup(mapped); }
            SETM("tasklist"); APPREST(); return strdup(mapped);
        }
        if(strcmp(first_lc,"kill")==0){
            // kill -9 pid -> taskkill /PID pid /F
            if(strstr(rest,"-9")){
                char t[MAX_LINE]; strncpy(t, rest, sizeof(t));
                replace_first(t,"-9","");
                trim(t);
                snprintf(mapped,sizeof(mapped),"taskkill /PID %s /F", t);
                return strdup(mapped);
            } else {
                char t[MAX_LINE]; strncpy(t, rest, sizeof(t)); trim(t);
                snprintf(mapped,sizeof(mapped),"taskkill /PID %s", t);
                return strdup(mapped);
            }
        }
        if(strcmp(first_lc,"jobs")==0 || strcmp(first_lc,"fg")==0 || strcmp(first_lc,"bg")==0){
            SETM("rem job control not supported on Windows; use powershell background jobs or task manager"); APPREST(); return strdup(mapped);
        }
        if(strcmp(first_lc,"ping")==0){ SETM("ping"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"curl")==0){ SETM("curl"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"wget")==0){
            SETM("curl -O"); APPREST(); return strdup(mapped);
        }
        if(strcmp(first_lc,"ifconfig")==0 || (strcmp(first_lc,"ip")==0 && strstr(rest,"addr"))){
            SETM("ipconfig /all"); return strdup(mapped);
        }
        if(strcmp(first_lc,"netstat")==0){
            // netstat -tulnp -> netstat -ano
            SETM("netstat -ano"); APPREST(); return strdup(mapped);
        }
        if(strcmp(first_lc,"ssh")==0){ SETM("ssh"); APPREST(); return strdup(mapped); } // Windows 10+ may have ssh
        if(strcmp(first_lc,"scp")==0){ SETM("scp"); APPREST(); return strdup(mapped); } // requires installed scp
        // package managers: apt/dnf/pacman -> not supported
        if(strcmp(first_lc,"sudo")==0){
            // remove sudo on Windows; try to run via powershell start-process -Verb runAs for elevation is complex; we'll strip it
            char t[MAX_LINE]; strncpy(t, rest, sizeof(t)); trim(t);
            if(strlen(t)) return strdup(t);
            else return strdup("rem sudo with no command");
        }
        if(strcmp(first_lc,"apt")==0 || strcmp(first_lc,"dnf")==0 || strcmp(first_lc,"pacman")==0){
            SETM("rem Package manager commands are not supported on Windows; consider using WSL or equivalent"); APPREST(); return strdup(mapped);
        }
        if(strcmp(first_lc,"adduser")==0 || strcmp(first_lc,"passwd")==0 || strcmp(first_lc,"su")==0){
            SETM("rem User management must be done via Control Panel or net user on Windows"); APPREST(); return strdup(mapped);
        }
        if(strcmp(first_lc,"who")==0 || strcmp(first_lc,"id")==0 || strcmp(first_lc,"groups")==0){
            SETM("whoami"); APPREST(); return strdup(mapped);
        }
        if(strcmp(first_lc,"tar")==0){
            // many forms: tar -czvf file.tar.gz dir/ -> use tar if Windows has tar.exe or use powershell Compress-Archive
            if(strstr(rest,"-czvf") || strstr(rest,"-czf")){
                // find archive name and dir
                // fallback to using tar if available
                SETM("tar"); APPREST(); return strdup(mapped);
            }
            SETM("tar"); APPREST(); return strdup(mapped);
        }
        if(strcmp(first_lc,"zip")==0 || strcmp(first_lc,"unzip")==0){
            // Windows: use powershell Compress-Archive or Expand-Archive
            if(strcmp(first_lc,"zip")==0){
                SETM("powershell -Command \"Compress-Archive -Path"); if(strlen(rest)){ strncat(mapped, " ", sizeof(mapped)-strlen(mapped)-1); strncat(mapped, rest, sizeof(mapped)-strlen(mapped)-1); } strncat(mapped, "\"", sizeof(mapped)-strlen(mapped)-1); return strdup(mapped);
            } else {
                SETM("powershell -Command \"Expand-Archive -Path"); if(strlen(rest)){ strncat(mapped, " ", sizeof(mapped)-strlen(mapped)-1); strncat(mapped, rest, sizeof(mapped)-strlen(mapped)-1); } strncat(mapped, "\"", sizeof(mapped)-strlen(mapped)-1); return strdup(mapped);
            }
        }
        if(strcmp(first_lc,"history")==0){ SETM("rem history shown by this terminal"); return strdup(mapped); }
        if(strcmp(first_lc,"clear")==0){ SETM("cls"); return strdup(mapped); }
        if(strcmp(first_lc,"!!")==0){ // handled outside
            return strdup("!!");
        }
        if(first_lc[0]=='!'){ // !n handled outside
            return strdup(input);
        }

        // default fallback: try to run via bash on Windows if available (WSL) else run raw
        // We'll attempt to run original in PowerShell by wrapping: bash -lc "input"
        // But since host_is_windows, we will try to run via bash -c if WSL present:
        char trybash[MAX_LINE*2];
        snprintf(trybash, sizeof(trybash), "bash -lc \"%s\"", input);
        return strdup(trybash);
    }

    // Windows -> Linux mappings
    if(source_is_windows && !host_is_windows){
        if(strcmp(first_lc,"dir")==0){
            // map flags /a etc roughly
            SETM("ls"); APPREST(); return strdup(mapped);
        }
        if(strcmp(first_lc,"type")==0){ SETM("cat"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"copy")==0){ SETM("cp"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"move")==0){ SETM("mv"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"del")==0 || strcmp(first_lc,"erase")==0){ SETM("rm"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"rmdir")==0){ SETM("rm -r"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"mkdir")==0){ SETM("mkdir"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"cls")==0){ SETM("clear"); return strdup(mapped); }
        if(strcmp(first_lc,"whoami")==0){ SETM("whoami"); return strdup(mapped); }
        if(strcmp(first_lc,"systeminfo")==0){ SETM("uname -a"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"hostname")==0){ SETM("hostname"); return strdup(mapped); }
        if(strcmp(first_lc,"date")==0){ SETM("date"); return strdup(mapped); }
        if(strcmp(first_lc,"netstat")==0){ SETM("netstat -tulnp"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"tasklist")==0){ SETM("ps aux"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"taskkill")==0){
            // taskkill /PID pid /F -> kill -9 pid
            char t[MAX_LINE]; strncpy(t, rest, sizeof(t)); trim(t);
            // try to find PID
            char pid[MAX_TOK] = {0};
            if(strstr(t,"/PID")){
                char *p = strstr(t,"/PID")+4;
                while(*p && isspace((unsigned char)*p)) p++;
                int i=0;
                while(*p && !isspace((unsigned char)*p) && i<MAX_TOK-1) pid[i++]=*p++;
                pid[i]=0;
                snprintf(mapped,sizeof(mapped),"kill -9 %s", pid); return strdup(mapped);
            } else {
                SETM("rem cannot map taskkill: check args"); APPREST(); return strdup(mapped);
            }
        }
        if(strcmp(first_lc,"ipconfig")==0){ SETM("ifconfig"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"ping")==0){ SETM("ping"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"curl")==0){ SETM("curl"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"ssh")==0){ SETM("ssh"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"scp")==0){ SETM("scp"); APPREST(); return strdup(mapped); }
        if(strcmp(first_lc,"powershell")==0){ // pass through but remove 'powershell -Command'
            SETM(rest); return strdup(mapped);
        }
        if(strcmp(first_lc,"wmic")==0){
            SETM("df -h"); APPREST(); return strdup(mapped);
        }
        if(strcmp(first_lc,"cls")==0){ SETM("clear"); return strdup(mapped); }
        if(strcmp(first_lc,"tar")==0 || strcmp(first_lc,"Compress-Archive")==0){
            SETM("tar"); APPREST(); return strdup(mapped);
        }
        if(strcmp(first_lc,"rem")==0){
            // comment - do nothing
            SETM("true"); return strdup(mapped);
        }
        if(strcmp(first_lc,"history")==0){ SETM("history"); return strdup(mapped); } // history is handled in this terminal
        if(strcmp(first_lc,"start")==0){
            SETM("xdg-open"); APPREST(); return strdup(mapped);
        }
        // fallback: return original
        return strdup(input);
    }

    // default fallback
    return strdup(input);
}

// Expand !! and !n using history; returns malloc'd string
static char *expand_bang(const char *cmd){
    if(strcmp(cmd,"!!")==0){
        if(hist_count==0) return strdup("");
        return strdup(history[hist_count-1]);
    }
    if(cmd[0]=='!' && isdigit((unsigned char)cmd[1])){
        int n = atoi(cmd+1);
        if(n <= 0 || n > hist_count) return strdup("");
        return strdup(history[n-1]);
    }
    return strdup(cmd);
}

// New function: handle built-in commands that can appear in a pipeline
static int handle_builtin_pipeline(const char *cmd) {
    char first[MAX_TOK], rest[MAX_LINE];
    split_first(cmd, first, rest);
    char first_lc[MAX_TOK]; lc_copy(first, first_lc);

    if(strcmp(first_lc,"help")==0){
        printf("Universal Terminal — Help\n");
        printf("-------------------------\n");
        printf("Built-in commands:\n");
        printf("  exit, quit       : Exit the terminal\n");
        printf("  history          : Show last 100 commands\n");
        printf("  clear            : Clear the screen\n");
        printf("  !!               : Repeat last command\n");
        printf("  !<num>           : Repeat command number <num> from history\n");
        printf("  help             : Show this help message\n");
        printf("\nCommand translation:\n");
        printf("  You can type commands in your chosen dialect (Windows CMD or Linux Bash)\n");
        printf("  Common commands like ls, dir, cp, move, rm, del, cat, etc., are mapped to the host OS\n");
        printf("  Piped commands (using |) are supported and translated\n");
        return 1; // indicates it was handled
    }
    if(strcmp(first_lc,"exit")==0 || strcmp(first_lc,"quit")==0){
        exit(0);
    }
    if(strcmp(first_lc,"history")==0){
        print_history();
        return 1;
    }
    if(strcmp(first_lc,"clear")==0){
        if(HOST_IS_WINDOWS) system("cls"); else system("clear");
        return 1;
    }

    return 0; // not a handled built-in
}

// New function: handle multiple commands separated by |
static char *translate_pipeline(const char *line, int source_is_windows, int host_is_windows) {
    char buf[MAX_LINE * 2];
    buf[0] = 0;

    char *copy = strdup(line);
    if (!copy) return strdup("");

    char *saveptr = NULL;
    char *token;
    int first = 1;

    token = STRTOK(copy, "|", &saveptr);
    while (token) {
        trim(token);
        if (strlen(token) > 0) {
            if(handle_builtin_pipeline(token)) {
                // skip adding to mapped buffer, already handled
                token = STRTOK(NULL, "|", &saveptr);
                continue;
            }
            char *mapped = map_command(token, source_is_windows, host_is_windows);
            if (mapped) {
                if (!first) strncat(buf, " | ", sizeof(buf) - strlen(buf) - 1);
                strncat(buf, mapped, sizeof(buf) - strlen(buf) - 1);
                free(mapped);
                first = 0;
            }
        }
        token = STRTOK(NULL, "|", &saveptr);
    }

    free(copy);
    return strdup(buf);
}


int main(){
    printf("Universal Terminal — Full mapping\n");
    printf("--------------------------------\n");
#if HOST_IS_WINDOWS
    printf("Host detected: Windows (compile-time)\n");
#else
    printf("Host detected: Unix-like (Linux/macOS) (compile-time)\n");
#endif

   int source_is_windows = -1; // invalid initial value
    char choice[16];

    while (source_is_windows == -1) {
        printf("Choose input dialect (the style YOU will type):\n");
        printf("  1) Windows (cmd)\n");
        printf("  2) Linux (bash)\n    Enter 1 or 2: ");

        if (!fgets(choice, sizeof(choice), stdin)) {
            fprintf(stderr, "No input detected. Try again.\n");
            continue;
        }

        trim(choice);

        if (choice[0] == '1') {
            source_is_windows = 1;
        } else if (choice[0] == '2') {
            source_is_windows = 0;
        } else {
            printf("Invalid choice. Please enter 1 or 2.\n\n");
        }
    }


    printf("Type commands in the chosen dialect. Type 'exit' to quit. 'history' shows recent commands.\n");

    char line[MAX_LINE];
    while(1){
        if(source_is_windows) printf("cmd> ");
        else printf("bash> ");
        if(!fgets(line, sizeof(line), stdin)){
            printf("\n");
            break;
        }
        trim(line);
        if(strlen(line)==0) continue;
        // handle help command
        if(strcmp(line,"help")==0){
            printf("Universal Terminal — Help\n");
            printf("-------------------------\n");
            printf("Built-in commands:\n");
            printf("  exit, quit       : Exit the terminal\n");
            printf("  history          : Show last 100 commands\n");
            printf("  clear            : Clear the screen\n");
            printf("  !!               : Repeat last command\n");
            printf("  !<num>           : Repeat command number <num> from history\n");
            printf("  help             : Show this help message\n");
            printf("\nCommand translation:\n");
            printf("  You can type commands in your chosen dialect (Windows CMD or Linux Bash)\n");
            printf("  Common commands like ls, dir, cp, move, rm, del, cat, etc., are mapped to the host OS\n");
            printf("  Piped commands (using |) are supported and translated\n");
            add_history(line);
            continue;
        }

        // Handle literal textual control commands if user types "CTRL + C" etc.
        if(strcmp(line,"CTRL + C")==0 || strcmp(line,"CTRL+C")==0){
            printf("[Note] To send an interrupt to a running process, press Ctrl-C on your keyboard while it's running.\n");
            continue;
        }
        if(strcmp(line,"CTRL + D")==0 || strcmp(line,"CTRL+D")==0){
            printf("[Note] Ctrl-D sends EOF in UNIX shells (pressing it here won't exit the terminal session). Use 'exit' to quit.\n");
            continue;
        }
        if(strcmp(line,"CTRL + Z")==0 || strcmp(line,"CTRL+Z")==0){
            printf("[Note] Ctrl-Z suspends a process in UNIX; job control not fully supported across OS translations.\n");
            continue;
        }

        // Expand history references
       if(line[0]=='!'){
            char *expanded = expand_bang(line);
            if(expanded && strlen(expanded)){
                printf("[Expanded] %s\n", expanded);
                strncpy(line, expanded, sizeof(line)-1);
                line[sizeof(line)-1]=0;
            } else {
                printf("No such history entry.\n");
                free(expanded);
                continue;
            }
            free(expanded);
        }


        // handle builtins: exit, history, clear, etc
        char cmd_copy[MAX_LINE]; strncpy(cmd_copy, line, sizeof(cmd_copy));
        char first[MAX_TOK], rest[MAX_LINE];
        split_first(cmd_copy, first, rest);
        char first_lc[MAX_TOK]; lc_copy(first, first_lc);

        if(strcmp(first_lc,"exit")==0 || strcmp(first_lc,"quit")==0) break;
        if(strcmp(first_lc,"history")==0){
            print_history();
            add_history(line);
            continue;
        }
        if(strcmp(first_lc,"clear")==0){
            if(HOST_IS_WINDOWS) system("cls"); else system("clear");
            add_history(line);
            continue;
        }

        // add to history before expansion of !!? Add after expansion done. We already expanded !n earlier.

        add_history(line);

        // Translate
        char *translated = translate_pipeline(line, source_is_windows, HOST_IS_WINDOWS);
        if(!translated){
            translated = strdup(line);
        }

        // If translation yields empty or just a note, print and skip running system if it starts with "rem" or "true" or "echo"?
        if(strncmp(translated,"rem ",4)==0 || strncmp(translated,"true",4)==0 || strncmp(translated,"echo ",5)==0){
            printf("[Translated note] %s\n", translated);
            free(translated);
            continue;
        }

        printf("[Translated ->] %s\n", translated);

        // Execute translated command
        // int rc = system(translated);
        // if(rc == -1){
        //     printf("Failed to run command on host shell.\n");
        // }
        
        // Execute translated command
        if(strlen(translated) > 0){
            printf("[Translated ->] %s\n", translated);

            #if HOST_IS_WINDOWS
                    if (strchr(translated, '|') != NULL) {
                        // Split into separate commands if pipe is found
                        char copy[MAX_LINE * 3];
                        strncpy(copy, translated, sizeof(copy));
                        copy[sizeof(copy)-1] = '\0';

                        char *token = strtok(copy, "|");
                        while (token != NULL) {
                            // Trim leading spaces
                            while (*token == ' ') token++;

                            char cmdline[MAX_LINE * 3];
                            snprintf(cmdline, sizeof(cmdline), "cmd /C \"%s\"", token);
                            printf("[Running ->] %s\n", cmdline);

                            int rc = system(cmdline);
                            if (rc == -1) {
                                printf("Failed to run command on host shell.\n");
                            }

                            token = strtok(NULL, "|");
                        }
                    } else {
                        char cmdline[MAX_LINE * 3];
                        snprintf(cmdline, sizeof(cmdline), "cmd /C \"%s\"", translated);
                        int rc = system(cmdline);
                        if (rc == -1) {
                            printf("Failed to run command on host shell.\n");
                        }
                    }
            #else
                // Linux/Unix: /bin/sh already understands pipes
                int rc = system(translated);
                if (rc == -1) {
                    printf("Failed to run command on host shell.\n");
                }
            #endif

        }


        // free(translated);
    }

    // cleanup history
    for(int i=0;i<hist_count;i++) free(history[i]);

    printf("Goodbye.\n");
    return 0;
}
