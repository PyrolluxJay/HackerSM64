#pragma once

#include <ultra64.h>

#include "types.h"

#include "asm.h"


#define ASSERTF_BUFFER_SIZE 255


extern const char* __n64Assert_Condition;
extern const char* __n64Assert_Filename;
extern int         __n64Assert_LineNum;
extern const char* __n64Assert_Message;


extern int __assert_address;


extern void __n64Assert(char* condition, char* fileName, u32 lineNum, char* message);
extern void __n64Assertf(char* condition, char* fileName, u32 lineNum, char* message, ...) __attribute__((format(printf, 4, 5)));


/**
 * Set the assert address to the current location.
 */
#define SET_ASSERT_ADDRESS() __assert_address = _asm_getaddr()


/**
 * Will always cause a crash with your message of choice.
 */
#define ERROR(__message__) do {                                                             \
    SET_ASSERT_ADDRESS();                                                                   \
    __n64Assert(NULL, __FILE__, __LINE__, __message__"\n");                                 \
} while (0)

#define ERRORF(__message__, ...) do {                                                       \
    SET_ASSERT_ADDRESS();                                                                   \
    __n64Assertf(NULL, __FILE__, __LINE__, __message__"\n", ##__VA_ARGS__);                 \
} while (0)


/**
 * Will always cause a crash if cond is not true (handle with care).
 */
#define ASSERT(__condition__, __message__) do {                                             \
    if (!(__condition__)) {                                                                 \
        SET_ASSERT_ADDRESS();                                                               \
        __n64Assert(#__condition__, __FILE__, __LINE__, __message__"\n");                   \
    }                                                                                       \
} while (0)
#define ASSERTF(__condition__, __message__, ...) do {                                       \
    if (!(__condition__)) {                                                                 \
        SET_ASSERT_ADDRESS();                                                               \
        __n64Assertf(#__condition__, __FILE__, __LINE__, __message__"\n", ##__VA_ARGS__);   \
    }                                                                                       \
} while (0)

/**
 * Will cause a crash if condition is not true, and DEBUG_ASSERTIONS is defined (allows for quick removal of littered asserts).
 */
#ifdef DEBUG_ASSERTIONS
    #define DEBUG_ERROR(__message__)                        ERROR(__message__)
    #define DEBUG_ERRORF(__message__, ...)                  ERRORF(__message__, ##__VA_ARGS__)
    #define DEBUG_ASSERT(__condition__, __message__)        ASSERT(__condition__, __message__)
    #define DEBUG_ASSERTF(__condition__, __message__, ...)  ASSERTF(__condition__, __message__, ##__VA_ARGS__)
#else
    #define DEBUG_ERROR(__message__)
    #define DEBUG_ERRORF(__message__, ...)
    #define DEBUG_ASSERT(__condition__, message)
    #define DEBUG_ASSERTF(__condition__, message, ...)
#endif

// Case sensitivity:
#define error           ERROR
#define errorf          ERRORF
#define assert          ASSERT
#define assertf         ASSERTF
#define debug_error     DEBUG_ERROR
#define debug_errorf    DEBUG_ERRORF
#define debug_assert    DEBUG_ASSERT
#define debug_assertf   DEBUG_ASSERTF

// Backwards compatibility:
#define aggress         assert
#define aggressf        assertf
