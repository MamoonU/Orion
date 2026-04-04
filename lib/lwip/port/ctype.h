// port/ctype.h - minimal ctype shim for the lwIP port

#ifndef LWIP_PORT_CTYPE_H
#define LWIP_PORT_CTYPE_H

static inline int isalnum (int c) {                                                 // alphanumeric check
    return (c>='0'&&c<='9')||(c>='a'&&c<='z')||(c>='A'&&c<='Z');
}
static inline int isalpha (int c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z'); }    // alpha check
static inline int isdigit (int c) { return c>='0'&&c<='9'; }                        // numeric check
static inline int isupper (int c) { return c>='A'&&c<='Z'; }                        // upper case check
static inline int islower (int c) { return c>='a'&&c<='z'; }                        // lower case check
static inline int isspace (int c) {                                                 // space, \n, \r, \t ...etc check
    return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v';
}
static inline int isxdigit(int c) {
    return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');
}
static inline int tolower (int c) { return isupper(c) ? c+32 : c; }                 // case conversion -> lower
static inline int toupper (int c) { return islower(c) ? c-32 : c; }                 // case conversion -> upper

#endif