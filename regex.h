
#ifndef REGEX_H
#define REGEX_H

#include <stdbool.h>
#include <stdint.h>

struct Regex;
typedef struct Regex Regex;

Regex* regex_compile(const char* regexStr);
const char* regex_match_node(Regex* regex, intptr_t nodeIndex, const char* str, const char* bol, const char* eol, intptr_t next);

#define regex_match_length(regex,text,textLength) (regex_match_node((regex), 0, (text), (text), (text)+(textLength), -1)==(text)+(textLength))
#define regex_match(regex,text) regex_match_length((regex),(text),strlen((text)))

#endif
