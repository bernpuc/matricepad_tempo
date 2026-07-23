#pragma once

// Null-terminated, no-heap string helpers for parsing "||"-delimited serial
// packets in place ("song||artist||volume||muted||paused||...").

// Finds the next "||" separator in s, or nullptr if none remains.
char* findSep(char *s);

// Trims leading/trailing whitespace from s in place.
void trimInPlace(char *s);

// Copies at most (dstCapacity - 1) chars from src into dst, always
// null-terminating. No heap allocation.
void copyField(char *dst, int dstCapacity, const char *src, int len);

// Splits title into out1/out2 (each with capacity outCapacity), word-wrapping
// at the last space at or before maxLen chars. Used by the 3-line display
// layout to fit long song titles across two static rows.
void splitTitleIntoLines(const char *title, char *out1, char *out2, int outCapacity, int maxLen);
