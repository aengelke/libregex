
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <setjmp.h>
#include "regex.h"

typedef intptr_t RegexNodeIndex;

#define SYMBOL_BEGIN ('^')
#define SYMBOL_END ('$')
#define SYMBOL_BRANCH ('|')
#define SYMBOL_ANY ('.')
#define SYMBOL_ESCAPE ('\\')
#define SYMBOL_ZEROMORE ('*')
#define SYMBOL_ONEMORE ('+')
#define SYMBOL_ZEROONE ('?')

enum RegexNodeType {
    REGEX_NODE_BEGIN,
    REGEX_NODE_END,
    REGEX_NODE_CHAR,
    REGEX_NODE_EXPR,
    REGEX_NODE_EXPR_NOCAPTURE,
    REGEX_NODE_CLASS,
    REGEX_NODE_CLASS_INVERSE,
    REGEX_NODE_ANY, // dot
    REGEX_NODE_GREEDY,
    REGEX_NODE_RANGE,
    REGEX_NODE_OR,
};

typedef enum RegexNodeType RegexNodeType;

struct RegexNode {
    RegexNodeType type;
    intptr_t next;
    intptr_t left;
    intptr_t right;
};

typedef struct RegexNode RegexNode;

struct RegexBuilder {
    size_t nodesUsed;
    size_t nodesAllocated;
    RegexNode* nodes;
    size_t matchCount;
    const char* regex;
    const char* p;
    jmp_buf errorJump;
};

typedef struct RegexBuilder RegexBuilder;

struct Regex {
    size_t length;
    size_t matchCount;
    RegexNode nodes[];
};


#define NODE(builder,index) ((index)==-1?NULL:(builder)->nodes+(index))


static RegexNodeIndex regex_compile_list(RegexBuilder* builder);

static
__attribute__((noreturn))
void
regex_error(RegexBuilder* builder, const char* message)
{
    printf("%s\n", message);
    longjmp(builder->errorJump, 1);
}

// static
// void
// regex_dump(RegexBuilder* builder)
// {
//     printf("Regex: %lu nodes (%lu allocated)\n", builder->nodesUsed, builder->nodesAllocated);

//     for (size_t i = 0; i < builder->nodesUsed; i++)
//     {
//         RegexNode* node = builder->nodes + i;
//         printf("- Node %d: next %ld, %ld - %ld\n", node->type, node->next, node->left, node->right);
//     }
// }

static
RegexNodeIndex
regex_alloc_node(RegexBuilder* builder, RegexNodeType type)
{
    if (builder->nodesUsed >= builder->nodesAllocated)
    {
        builder->nodesAllocated += 10;
        builder->nodes = realloc(builder->nodes, sizeof(RegexNode) * builder->nodesAllocated);

        if (builder->nodes == NULL)
            regex_error(builder, "out of memory");
    }

    RegexNodeIndex newNode = builder->nodesUsed++;

    NODE(builder,newNode)->type = type;
    NODE(builder,newNode)->next = -1;
    NODE(builder,newNode)->left = -1;
    NODE(builder,newNode)->right = -1;

    return newNode;
}

static
char
regex_char_test(RegexBuilder* builder)
{
    return *builder->p;
}

static
char
regex_char_fetch(RegexBuilder* builder)
{
    if (*builder->p == '\0')
        regex_error(builder, "end of regex");

    char value = *builder->p;
    builder->p++;
    return value;
}

static
bool
regex_char_test_and_fetch(RegexBuilder* builder, char match)
{
    if (regex_char_test(builder) == match)
    {
        regex_char_fetch(builder);
        return true;
    }

    return false;
}

static
RegexNodeIndex
regex_node_value(RegexBuilder* builder, RegexNodeType type, intptr_t value)
{
    RegexNodeIndex node = regex_alloc_node(builder, type);
    NODE(builder,node)->left = value;
    return node;
}

static
RegexNodeIndex
regex_compile_char(RegexBuilder* builder)
{
    RegexNodeIndex node;

    if (regex_char_test_and_fetch(builder, SYMBOL_ESCAPE))
    {
        char value = regex_char_fetch(builder);
        switch (value)
        {
            case 'n': node = regex_node_value(builder, REGEX_NODE_CHAR, '\n'); break;
            case 't': node = regex_node_value(builder, REGEX_NODE_CHAR, '\t'); break;
            case 'r': node = regex_node_value(builder, REGEX_NODE_CHAR, '\r'); break;
            case 'v': node = regex_node_value(builder, REGEX_NODE_CHAR, '\v'); break;
            default:
                node = regex_node_value(builder, REGEX_NODE_CHAR, value);
                break;
        }
    }
    else
        node = regex_node_value(builder, REGEX_NODE_CHAR, regex_char_fetch(builder));

    return node;
}

static
RegexNodeIndex
regex_compile_element(RegexBuilder* builder)
{
    RegexNodeIndex ret = -1;
    RegexNodeIndex chain = -1;
    char currentChar = '\0';

    switch (regex_char_test(builder))
    {
        case SYMBOL_END:
            regex_char_fetch(builder);
            ret = regex_alloc_node(builder, REGEX_NODE_END);
            break;
        case SYMBOL_ANY:
            regex_char_fetch(builder);
            ret = regex_alloc_node(builder, REGEX_NODE_ANY);
            break;
        case '[':
            regex_char_fetch(builder);
            if (regex_char_test_and_fetch(builder, '^'))
                ret = regex_alloc_node(builder, REGEX_NODE_CLASS_INVERSE);
            else
                ret = regex_alloc_node(builder, REGEX_NODE_CLASS);

            if (regex_char_test(builder) == ']')
                regex_error(builder, "empty class");

            while (regex_char_test(builder) != ']')
            {
                RegexNodeIndex node;

                if (currentChar != '\0' && regex_char_test_and_fetch(builder, '-'))
                {
                    char endOfRange = regex_char_fetch(builder);
                    if (endOfRange == ']')
                    {
                        builder->p--;
                        node = regex_node_value(builder, REGEX_NODE_CHAR, '-');
                        if (chain != -1)
                            NODE(builder,node)->next = chain;
                        chain = node;
                        break;
                    }

                    if (currentChar > endOfRange)
                        regex_error(builder, "invalid range");

                    node = chain;
                    chain = NODE(builder,chain)->next;

                    NODE(builder,node)->type = REGEX_NODE_RANGE;
                    NODE(builder,node)->right = endOfRange;

                    currentChar = '\0';
                }
                else
                {
                    currentChar = regex_char_fetch(builder);
                    node = regex_node_value(builder, REGEX_NODE_CHAR, currentChar);
                }

                if (chain != -1)
                    NODE(builder,node)->next = chain;

                chain = node;
            }

            NODE(builder,ret)->left = chain;

            if (regex_char_fetch(builder) != ']')
                regex_error(builder, "open class");
            break;
        case '(':
            regex_char_fetch(builder);
            if (regex_char_test_and_fetch(builder, '?'))
            {
                if (regex_char_fetch(builder) != ':')
                    regex_error(builder, "invalid group");

                ret = regex_alloc_node(builder, REGEX_NODE_EXPR_NOCAPTURE);
            }
            else
            {
                builder->matchCount++;
                ret = regex_alloc_node(builder, REGEX_NODE_EXPR);
            }

            NODE(builder,ret)->left = regex_compile_list(builder);

            if (regex_char_fetch(builder) != ')')
                regex_error(builder, "open expr");

            break;
        default:
            ret = regex_compile_char(builder);
            break;
    }

    bool isGreedy;
    uint16_t min = 0;
    uint16_t max = 0;
    switch (regex_char_test(builder))
    {
        case SYMBOL_ZEROMORE:
            regex_char_fetch(builder);
            min = 0;
            max = 0xffff;
            isGreedy = true;
            break;
        case SYMBOL_ONEMORE:
            regex_char_fetch(builder);
            min = 1;
            max = 0xffff;
            isGreedy = true;
            break;
        case SYMBOL_ZEROONE:
            regex_char_fetch(builder);
            min = 0;
            max = 1;
            isGreedy = true;
            break;
        // case '{':
        //     {
        //         regex_char_fetch(builder);

        //     }
        default:
            isGreedy = false;
    }

    if (isGreedy)
    {
        ret = regex_node_value(builder, REGEX_NODE_GREEDY, ret);
        NODE(builder,ret)->right = (min << 16) | max;
    }

    char nextChar = regex_char_test(builder);
    if (nextChar != SYMBOL_BRANCH &&
        nextChar != SYMBOL_ZEROMORE &&
        nextChar != SYMBOL_ONEMORE &&
        nextChar != SYMBOL_ZEROONE &&
        nextChar != ')' &&
        nextChar != '\0')
    {
        RegexNodeIndex next = regex_compile_element(builder);
        NODE(builder,ret)->next = next;
    }

    return ret;
}

static
RegexNodeIndex
regex_compile_list(RegexBuilder* builder)
{
    RegexNodeIndex ret = -1;

    if (*builder->p == '\0')
        return ret;

    if (regex_char_test_and_fetch(builder, SYMBOL_BEGIN))
        ret = regex_alloc_node(builder, REGEX_NODE_BEGIN);

    RegexNodeIndex element = regex_compile_element(builder);

    if (ret != -1)
        NODE(builder,ret)->next = element;
    else
        ret = element;

    if (regex_char_test_and_fetch(builder, SYMBOL_BRANCH))
    {
        ret = regex_node_value(builder, REGEX_NODE_OR, ret);
        NODE(builder,ret)->right = regex_compile_list(builder);
    }

    return ret;
}

Regex*
regex_compile(const char* regexStr)
{
    RegexBuilder builder = {
        .nodesUsed = 0,
        .nodesAllocated = 0,
        .nodes = malloc(0),
        .matchCount = 0,
        .regex = regexStr,
        .p = regexStr,
    };

    Regex* regex = NULL;

    if (setjmp(builder.errorJump) == 0)
    {
        RegexNodeIndex entry = regex_alloc_node(&builder, REGEX_NODE_EXPR_NOCAPTURE);

        NODE(&builder,entry)->left = regex_compile_list(&builder);

        regex = (Regex*) malloc(sizeof(Regex) + builder.nodesUsed * sizeof(RegexNode));
        regex->length = sizeof(Regex) + builder.nodesUsed * sizeof(RegexNode);
        regex->matchCount = builder.matchCount;
        memcpy(regex->nodes, builder.nodes, builder.nodesUsed * sizeof(RegexNode));

        // regex_dump(&builder);
    }

    free(builder.nodes);

    return regex;
}

const char*
regex_match_node(Regex* regex, RegexNodeIndex nodeIndex, const char* str, const char* bol, const char* eol, RegexNodeIndex next)
{
    if (nodeIndex == -1)
        return NULL;

    RegexNode* node = NODE(regex, nodeIndex);

    switch (node->type)
    {
        case REGEX_NODE_ANY:
            return str + 1;
        case REGEX_NODE_CHAR:
            if (*str != node->left)
                return NULL;
            return str + 1;
        case REGEX_NODE_BEGIN:
            return str == bol ? str : NULL;
        case REGEX_NODE_END:
            return str == eol ? str : NULL;
        case REGEX_NODE_GREEDY:
            {
                uint16_t min = node->right >> 16;
                uint16_t max = node->right & 0xffff;
                size_t count = 0;

                const char* remaining = str;
                const char* handled = str;

                // RegexNodeIndex greedyStop = node->next != -1 ? node->next : next;

                while (count < max)
                {
                    if (!(remaining = regex_match_node(regex, node->left, remaining, bol, eol, -1)))
                        break;

                    count++;
                    handled = remaining;

                    // if (greedyStop != -1)
                    // {
                    //     RegexNode* stopNode = NODE(regex, greedyStop);
                    //     RegexNode* nextNode = NODE(regex, next);
                    //     if (stopNode->type != REGEX_NODE_GREEDY || (stopNode->right >> 16) != 0)
                    //     {
                    //         RegexNodeIndex nextStop = stopNode->next != -1 ? stopNode->next : next != -1 ? nextNode->next : -1;

                    //         const char* stop = regex_match_node(regex, greedyStop, handled, bol, eol, nextStop);
                    //         if (stop)
                    //         {
                    //             if (count >= min && max == 0xffff)
                    //                 break;
                    //             if (count >= min && count <= max)
                    //                 break;
                    //         }
                    //     }
                    // }

                    if (handled >= eol)
                        break;
                }

                if (count >= min && max == 0xffff)
                    return handled;
                if (count >= min && count <= max)
                    return handled;
            }
            return NULL;
        case REGEX_NODE_CLASS:
        case REGEX_NODE_CLASS_INVERSE:
            {
                RegexNodeIndex entries = node->left;
                bool match = false;

                do {
                    RegexNode* entry = NODE(regex, entries);
                    switch (entry->type)
                    {
                        case REGEX_NODE_CHAR:
                            if (*str == entry->left)
                                match = true;
                            break;
                        case REGEX_NODE_RANGE:
                            if (*str >= entry->left && *str <= entry->right)
                                match = true;
                            break;
                        default:
                            return NULL;
                    }
                    entries = entry->next;
                } while (entries != -1 && !match);

                if ((node->type == REGEX_NODE_CLASS) == match)
                    return str + 1;
            }
            return NULL;
        case REGEX_NODE_OR:
            {
                const char* remaining = str;
                RegexNodeIndex temp = node->left;
                while ((remaining = regex_match_node(regex, temp, remaining, bol, eol, -1)))
                {
                    RegexNode* tempNode = NODE(regex, temp);
                    if (tempNode->next != -1)
                        temp = tempNode->next;
                    else
                        return remaining;
                }
                remaining = str;
                temp = node->right;
                while ((remaining = regex_match_node(regex, temp, remaining, bol, eol, -1)))
                {
                    RegexNode* tempNode = NODE(regex, temp);
                    if (tempNode->next != -1)
                        temp = tempNode->next;
                    else
                        return remaining;
                }
            }
            return NULL;
        case REGEX_NODE_EXPR:
        case REGEX_NODE_EXPR_NOCAPTURE:
            {
                RegexNodeIndex currentIndex = node->left;
                const char* remaining = str;

                do {
                    RegexNode* currentNode = NODE(regex, currentIndex);
                    RegexNodeIndex subnext = -1;//currentNode->next != -1 ? currentNode->next : next;

                    if (!(remaining = regex_match_node(regex, currentIndex, remaining, bol, eol, subnext)))
                        return NULL;

                    currentIndex = currentNode->next;
                } while (currentIndex != -1);

                return remaining;
            }
        case REGEX_NODE_RANGE:
        default:
            return NULL;
    }
}

// bool
// regex_match(Regex* regex, const char* text)
// {
//     const char* eol = text + strlen(text);
//     const char* res = regex_match_node(regex, 0, text, text, eol, -1);
//     return res != NULL && res == eol;
// }

// int main()
// {
//     Regex* regex = regex_compile("[a-zA-Z0-9._%+-]+@(?:[a-zA-Z0-9-]+\\.)+[a-zA-Z]");
//     if (regex != NULL)
//     {
//         printf("%d\n", regex_match(regex, "abbbb@c.d"));
//         free(regex);
//     }
//     else
//         return 1;
//     return 0;
// }
