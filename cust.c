/*
  universal_terminal_full_gemini.c
  Universal terminal (full mapping set + Gemini fallback)
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
    #define STRTOK(str, delim, saveptr) strtok((str), (delim))
#else
    #define STRTOK(str, delim, saveptr) strtok_r((str), (delim), (saveptr))
#endif

// History
static char *history[MAX_HISTORY];
static int hist_count = 0;

static void add_history(const char *cmd){
    if(!cmd || strlen(cmd)==0) return;
    if(hist_count < MAX_HISTORY){
        history[hist_count++] = strdup(cmd);
    } else {
        free(history[0]);
        memmove(history, history+1, sizeof(char*)*(MAX_HISTORY-1));
        history[MAX_HISTORY-1] = strdup(cmd);
    }
}

static void print_history(){
    int start = hist_count>100? hist_count-100 : 0;
    for(int i=start;i<hist_count;i++){
        printf("%d  %s\n", i+1, history[i]);
    }
}

// trim
static void trim(char *s){
    if(!s) return;
    int i = 0;
    while(s[i] && isspace((unsigned char)s[i])) i++;
    if(i) memmove(s, s+i, strlen(s+i)+1);
    int len = strlen(s);
    while(len>0 && isspace((unsigned char)s[len-1])) s[--len]=0;
}

// Gemini Api configuration
static void call_gemini_api(const char *cmd) {
    char curl_cmd[4096];
    char buffer[4096];
    FILE *fp;

    // Replace YOUR_API_KEY with your actual Gemini API key
    snprintf(curl_cmd, sizeof(curl_cmd),
        "curl -s -X POST https://api.gemini.com/v1/commands "
        "-H \"Content-Type: application/json\" "
        "-H \"Authorization: Bearer YOUR_API_KEY\" "
        "-d '{\"command\": \"%s\"}'", cmd);

    // Open pipe to read output from curl
    if ((fp = popen(curl_cmd, "r")) == NULL) {
        printf("[Gemini API] Failed to execute curl\n");
        return;
    }

    printf("[Gemini API] Response:\n");
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        printf("%s", buffer); // Print each line of response
    }
    pclose(fp);
    printf("\n");
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

// Build command mapping
static char *map_command(const char *input, int source_is_windows, int host_is_windows){
    if(source_is_windows == host_is_windows) return strdup(input);
    char first[MAX_TOK], rest[MAX_LINE];
    split_first(input, first, rest);
    char first_lc[MAX_TOK]; lc_copy(first, first_lc);
    char mapped[MAX_LINE*2]; mapped[0]=0;
    #define SETM(fmt,...) do{ snprintf(mapped, sizeof(mapped), fmt, ##__VA_ARGS__); } while(0)
    #define APPREST() do{ if(strlen(rest)) { if(strlen(mapped)) strncat(mapped, " ", sizeof(mapped)-strlen(mapped)-1); strncat(mapped, rest, sizeof(mapped)-strlen(mapped)-1); } } while(0)

    // Linux -> Windows mapping (simplified)
    if(!source_is_windows && host_is_windows){
        if(strcmp(first_lc,"ls")==0){ SETM("dir"); APPREST(); return strdup(mapped);}
        if(strcmp(first_lc,"pwd")==0){ SETM("cd"); return strdup(mapped);}
        if(strcmp(first_lc,"rm")==0){ SETM("del"); APPREST(); return strdup(mapped);}
        if(strcmp(first_lc,"mkdir")==0){ SETM("mkdir"); APPREST(); return strdup(mapped);}
        if(strcmp(first_lc,"cp")==0){ SETM("copy"); APPREST(); return strdup(mapped);}
        if(strcmp(first_lc,"mv")==0){ SETM("move"); APPREST(); return strdup(mapped);}
        if(strcmp(first_lc,"cat")==0){ SETM("type"); APPREST(); return strdup(mapped);}
        if(strcmp(first_lc,"clear")==0){ SETM("cls"); return strdup(mapped);}
    }

    // Windows -> Linux mapping
    if(source_is_windows && !host_is_windows){
        if(strcmp(first_lc,"dir")==0){ SETM("ls"); APPREST(); return strdup(mapped);}
        if(strcmp(first_lc,"del")==0){ SETM("rm"); APPREST(); return strdup(mapped);}
        if(strcmp(first_lc,"copy")==0){ SETM("cp"); APPREST(); return strdup(mapped);}
        if(strcmp(first_lc,"move")==0){ SETM("mv"); APPREST(); return strdup(mapped);}
        if(strcmp(first_lc,"type")==0){ SETM("cat"); APPREST(); return strdup(mapped);}
        if(strcmp(first_lc,"cls")==0){ SETM("clear"); return strdup(mapped);}
    }

    return strdup(input);
}

// Expand !! and !n
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

// Built-in commands
static int handle_builtin_pipeline(const char *cmd) {
    char first[MAX_TOK], rest[MAX_LINE];
    split_first(cmd, first, rest);
    char first_lc[MAX_TOK]; lc_copy(first, first_lc);

    if(strcmp(first_lc,"help")==0){
        printf("Universal Terminal — Help\n");
        return 1;
    }
    if(strcmp(first_lc,"exit")==0 || strcmp(first_lc,"quit")==0) exit(0);
    if(strcmp(first_lc,"history")==0){ print_history(); return 1;}
    if(strcmp(first_lc,"clear")==0){ if(HOST_IS_WINDOWS) system("cls"); else system("clear"); return 1;}
    return 0;
}

// Pipeline translation
static char *translate_pipeline(const char *line, int source_is_windows, int host_is_windows) {
    char buf[MAX_LINE*2]; buf[0]=0;
    char *copy = strdup(line); if(!copy) return strdup("");
    char *saveptr=NULL, *token; int first=1;

    token = STRTOK(copy, "|", &saveptr);
    while(token){
        trim(token);
        if(strlen(token)>0){
            if(handle_builtin_pipeline(token)){ token=STRTOK(NULL,"|",&saveptr); continue;}
            char *mapped = map_command(token, source_is_windows, host_is_windows);
            if(mapped){
                if(!first) strncat(buf," | ",sizeof(buf)-strlen(buf)-1);
                strncat(buf,mapped,sizeof(buf)-strlen(buf)-1);
                free(mapped); first=0;
            }
        }
        token=STRTOK(NULL,"|",&saveptr);
    }

    free(copy);
    return strdup(buf);
}

// Gemini API fallback (mock example using curl)
static void call_gemini_api(const char *cmd){
    printf("[Gemini API] Command not recognized: %s\n", cmd);
    printf("[Gemini API] Response: This is a placeholder response from Gemini API for your command.\n");
    // Real implementation would use curl to call API and display JSON response
}

int main(){
    printf("Universal Terminal + Gemini fallback\n");
#if HOST_IS_WINDOWS
    printf("Host: Windows\n");
#else
    printf("Host: Unix-like\n");
#endif

    int source_is_windows=-1; char choice[16];
    while(source_is_windows==-1){
        printf("Choose input dialect:\n1) Windows\n2) Linux\nEnter 1 or 2: ");
        if(!fgets(choice,sizeof(choice),stdin)){ continue;}
        trim(choice);
        if(choice[0]=='1') source_is_windows=1;
        else if(choice[0]=='2') source_is_windows=0;
    }

    char line[MAX_LINE];
    while(1){
        if(source_is_windows) printf("cmd> "); else printf("bash> ");
        if(!fgets(line,sizeof(line),stdin)){ printf("\n"); break;}
        trim(line); if(strlen(line)==0) continue;

        if(line[0]=='!'){
            char *expanded = expand_bang(line);
            if(expanded && strlen(expanded)){ strncpy(line,expanded,sizeof(line)-1); line[sizeof(line)-1]=0;}
            free(expanded);
        }

        add_history(line);

        char *translated = translate_pipeline(line, source_is_windows, HOST_IS_WINDOWS);
        if(!translated) translated=strdup(line);

        if(strncmp(translated,"rem ",4)==0 || strncmp(translated,"true",4)==0){
            printf("[Note] %s\n", translated); free(translated); continue;
        }

        printf("[Translated] %s\n", translated);

        // Execute or fallback
        int rc = system(translated);
        if(rc==-1){
            call_gemini_api(line);
        }

        free(translated);
    }

    for(int i=0;i<hist_count;i++) free(history[i]);
    printf("Goodbye.\n");
    return 0;
}
