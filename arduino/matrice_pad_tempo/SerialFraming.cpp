#include "SerialFraming.h"
#include <string.h>
#include <ctype.h>

char* findSep(char *s) {
    return strstr(s, "||");
}

void trimInPlace(char *s) {
    int len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    int start = 0;
    while (s[start] && isspace((unsigned char)s[start])) start++;
    if (start > 0) memmove(s, s + start, len - start + 1);
}

void copyField(char *dst, int dstCapacity, const char *src, int len) {
    if (len > dstCapacity - 1) len = dstCapacity - 1;
    if (len < 0) len = 0;
    strncpy(dst, src, len);
    dst[len] = '\0';
}

void splitTitleIntoLines(const char *title, char *out1, char *out2, int outCapacity, int maxLen) {
    int titleLen = (int)strlen(title);
    if (titleLen <= maxLen) {
        copyField(out1, outCapacity, title, titleLen);
        out2[0] = '\0';
        return;
    }
    int splitPos = -1;
    for (int i = maxLen; i >= 0; i--) {
        if (title[i] == ' ') {
            splitPos = i;
            break;
        }
    }
    if (splitPos == -1) {
        copyField(out1, outCapacity, title, maxLen - 3);
        strcat(out1, "...");
        out2[0] = '\0';
    } else {
        copyField(out1, outCapacity, title, splitPos);

        const char *rest = title + splitPos + 1;
        int restLen = (int)strlen(rest);
        if (restLen > maxLen) {
            copyField(out2, outCapacity, rest, maxLen - 3);
            strcat(out2, "...");
        } else {
            copyField(out2, outCapacity, rest, restLen);
        }
    }
}
