#include "lib_json.h"
#include "string.h"

static void skip_ws(const char **p, const char *end) {
    while (*p < end) {
        char c = **p;
        if (c==' '||c=='\n'||c=='\r'||c=='\t') (*p)++;
        else break;
    }
}

static int expect_char(const char **p, const char *end, char ch) {
    skip_ws(p,end);
    if (*p >= end || **p != ch) return -1;
    (*p)++;
    return 0;
}

static int parse_string(const char **p, const char *end, char *out, int outcap) {
    skip_ws(p,end);
    if (*p >= end || **p != '"') return -1;
    (*p)++;
    int n = 0;
    while (*p < end) {
        char c = **p;
        (*p)++;
        if (c == '"') break;
        if (c == '\\') {
            if (*p >= end) return -1;
            char e = **p;
            (*p)++;
            switch (e) {
                case '"': c='"'; break;
                case '\\': c='\\'; break;
                case 'n': c='\n'; break;
                case 'r': c='\r'; break;
                case 't': c='\t'; break;
                default: return -1; // keep it strict
            }
        }
        if (n < outcap-1) out[n++] = c;
    }
    out[n] = 0;
    return 0;
}

static int key_eq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a++; char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

int json_manifest_parse(const char *json, size_t len, pakman_pkg_t *out, int max_out) {
    if (!json || !out || max_out <= 0) return -1;
    const char *p = json;
    const char *end = json + len;

    if (expect_char(&p,end,'[') != 0) return -1;

    int count = 0;
    skip_ws(&p,end);
    if (p < end && *p == ']') return 0;

    while (p < end) {
        if (count >= max_out) return -1;
        pakman_pkg_t *pkg = &out[count];
        memset(pkg, 0, sizeof(*pkg));

        if (expect_char(&p,end,'{') != 0) return -1;

        while (p < end) {
            skip_ws(&p,end);
            if (p < end && *p == '}') { p++; break; }

            char key[64];
            if (parse_string(&p,end,key,sizeof(key)) != 0) return -1;
            if (expect_char(&p,end,':') != 0) return -1;

            if (key_eq(key,"name")) {
                if (parse_string(&p,end,pkg->name,sizeof(pkg->name)) != 0) return -1;
            } else if (key_eq(key,"version")) {
                if (parse_string(&p,end,pkg->version,sizeof(pkg->version)) != 0) return -1;
            } else if (key_eq(key,"installPath") || key_eq(key,"installpath")) {
                if (parse_string(&p,end,pkg->install_path,sizeof(pkg->install_path)) != 0) return -1;
            } else {
                // Skip unknown value (string only for now)
                char tmp[256];
                if (parse_string(&p,end,tmp,sizeof(tmp)) != 0) return -1;
            }

            skip_ws(&p,end);
            if (p < end && *p == ',') { p++; continue; }
            if (p < end && *p == '}') { p++; break; }
        }

        count++;
        skip_ws(&p,end);
        if (p < end && *p == ',') { p++; continue; }
        if (p < end && *p == ']') { p++; break; }
    }

    return count;
}

static int append_str(char *out, size_t cap, size_t *pos, const char *s) {
    while (*s) {
        if (*pos + 1 >= cap) return -1;
        out[(*pos)++] = *s++;
    }
    out[*pos] = 0;
    return 0;
}

static int append_escaped(char *out, size_t cap, size_t *pos, const char *s) {
    while (*s) {
        char c = *s++;
        if (c == '"' || c == '\\') {
            if (*pos + 2 >= cap) return -1;
            out[(*pos)++]='\\';
            out[(*pos)++]=c;
        } else if (c == '\n') {
            if (*pos + 2 >= cap) return -1;
            out[(*pos)++]='\\';
            out[(*pos)++]='n';
        } else {
            if (*pos + 1 >= cap) return -1;
            out[(*pos)++]=c;
        }
    }
    out[*pos]=0;
    return 0;
}

int json_manifest_write(const pakman_pkg_t *pkgs, int count, char *outbuf, size_t outcap) {
    if (!outbuf || outcap < 4) return -1;
    size_t pos = 0;
    outbuf[0]=0;

    if (append_str(outbuf,outcap,&pos,"[\n") != 0) return -1;
    for (int i=0;i<count;i++) {
        const pakman_pkg_t *p = &pkgs[i];
        if (append_str(outbuf,outcap,&pos,"  {\"name\":\"")!=0) return -1;
        if (append_escaped(outbuf,outcap,&pos,p->name)!=0) return -1;
        if (append_str(outbuf,outcap,&pos,"\",\"version\":\"")!=0) return -1;
        if (append_escaped(outbuf,outcap,&pos,p->version)!=0) return -1;
        if (append_str(outbuf,outcap,&pos,"\",\"installPath\":\"")!=0) return -1;
        if (append_escaped(outbuf,outcap,&pos,p->install_path)!=0) return -1;
        if (append_str(outbuf,outcap,&pos,"\"}")!=0) return -1;
        if (i != count-1) {
            if (append_str(outbuf,outcap,&pos,",\n")!=0) return -1;
        } else {
            if (append_str(outbuf,outcap,&pos,"\n")!=0) return -1;
        }
    }
    if (append_str(outbuf,outcap,&pos,"]\n")!=0) return -1;
    return (int)pos;
}
