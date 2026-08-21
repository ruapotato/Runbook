/* string.h — a shim, so the lifted language files can include what they expect.
 *
 * There is no libc on this machine. The four files that make up the language
 * (value.c, lex.c, compile.c, vm.c) are compiled here UNCHANGED, and they
 * include the headers any host C file would; providing empty ones that route
 * to nom.h is what lets them stay unchanged.
 *
 * Everything they actually call -- strlen, memcpy, strcmp, snprintf's job --
 * is in nom.h, and this file exists so the #include line above it resolves.
 */
#ifndef GUEST_SHIM_STRING_H
#define GUEST_SHIM_STRING_H
#include "nom.h"
#endif
