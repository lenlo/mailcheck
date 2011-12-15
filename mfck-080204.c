/*
**  mailutil -- A General Mailbox Utility Tool
**
**  Copyright (c) 2008, Lennart Lovstrand.  All rights reserved.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sysexits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <math.h>
#include <time.h>

#ifdef USE_GC
#  ifdef DEBUG
#    define GC_DEBUG
#  endif
#  include <gc.h>
#  define malloc				GC_MALLOC
#  define realloc				GC_REALLOC
#  define free					GC_FREE
#endif

#define kCheck_MaxWarnCount			5

#define kArray_InitialSize			32
#define kArray_GrowthFactor			1.4

#define kRead_InitialSize			(64*1024)
#define kRead_GrowthFactor			1.5

#define kString_CStringPoolSize			10
#define kString_MaxPrintFLength			1024
#define kString_MaxPrettyLength			32

#define kDefaultPageWidth			80
#define kDefaultPageHeight			24

#define String_Define(NAM, CSTR)			\
    const String NAM = {CSTR, sizeof(CSTR)-1, kString_Const};

typedef enum {false = 0, true = !false} bool;

typedef enum {kString_Shared = 0, kString_Alloced, kString_Mapped,
	      kString_Const} StringType;

typedef void Free(void *);

typedef struct {
    const char *buf;
    int len;
    // Must be last
    StringType type;
} String;

typedef struct {
    //String *body;		// not owned by the parser
    const char *start;
    String rest;
} Parser;

typedef struct _Header {
    String *key;
    String *value;
    String *line;		// Original complete header line(s)
    struct _Header *next;
} Header;

typedef struct {
    Header *root;
    struct _Message *msg;
} Headers;

typedef enum {
    kDFSB_None		= 0x00,
    kDFSB_CLXUID	= 0x01,
    kDFSB_Status	= 0x02,
    kDFSB_Newline	= 0x04,
} DovecotFromSpaceBugType;

typedef struct _Message {
    int num;
    struct _Mailbox *mbox;
    String *tag;
    String *data;
    String *envSender;
    struct tm envDate;
    Headers *headers;
    String *body;
    String *cachedID;		// Cached from headers; do not free
    bool deleted;
    bool dirty;
    DovecotFromSpaceBugType dovecotFromSpaceBug;
    struct _Message *next;
} Message;

typedef struct _Mailbox {
    String *source;
    String *name;
    String *data;
    Message *root;
    int count;
    bool dirty;
} Mailbox;

typedef struct {
    void **items;
    int count;
    int size;
    Free *liberator;
} Array;

typedef struct {
    FILE *file;
    String *name;
    bool ignoreErrors;
} Stream;

#define New(T)			((T *) calloc(1, sizeof(T)))

/*
**  String Constants
*/

// Genral Strings
String_Define(Str_Emtpy, "");

// Header Keys
String_Define(Str_Bcc, "bcc");
String_Define(Str_Cc, "cc");
String_Define(Str_ContentLength, "Content-Length");
String_Define(Str_Date, "Date");
String_Define(Str_From, "From");
String_Define(Str_FromSpace, "From ");
String_Define(Str_MessageID, "Message-ID");
String_Define(Str_NLFromSpace, "\nFrom ");
String_Define(Str_NL2FromSpace, "\n\nFrom ");
String_Define(Str_Received, "Received");
String_Define(Str_ReturnPath, "Return-Path");
String_Define(Str_Sender, "Sender");
String_Define(Str_Status, "Status");
String_Define(Str_Subject, "Subject");
String_Define(Str_To, "To");
String_Define(Str_XDate, "X-Date");
String_Define(Str_XFrom, "X-From");
String_Define(Str_XIMAP, "X-IMAP");
String_Define(Str_XIMAPBase, "X-IMAPBase");
String_Define(Str_XUID, "X-UID");

// Other Strings
String_Define(Str_All, "all");
String_Define(Str_Check, "check");
String_Define(Str_Repair, "repair");
String_Define(Str_Unique, "unique");

String_Define(Str_EnvelopeDate, "envelope date");
String_Define(Str_EnvelopeSender, "envelope sender");

/*
**  Global Variables
*/

bool gAddContentLength = false;
bool gAutoWrite = false;
bool gBackup = false;
bool gCheck = false;
#ifdef DEBUG
bool gDebug = false;
#endif
bool gDryRun = false;
bool gInteractive = false;
bool gMap = true;
bool gPicky = false;
bool gQuiet = false;
bool gRepair = false;
bool gUnique = false;
bool gVerbose = false;

int gWarnings = 0;
int gMessageCounter = 0;
String *gPager = NULL;
Stream *gStdOut;
int gPageWidth = kDefaultPageWidth;
int gPageHeight = kDefaultPageHeight;

/*
**  Math Functions
*/

inline int iMin(int a, int b)	{return a < b ? a : b;}
inline int iMax(int a, int b)	{return a > b ? a : b;}

/*
**  Char Functions
*/

inline bool Char_IsNewline(int ch)	{return ch == '\r' || ch == '\n';}

const char *Char_QuotedCString(char ch)
{
    static char buf[10];

    if (ch < ' ' || ch > '~') {
	switch (ch) {
	  case '\t': return "'\\t'";
	  case '\n': return "'\\n'";
	  case '\r': return "'\\r'";
	  default:
	    sprintf(buf, "'\\%03o'", ch & 0xff);
	    return buf;
	}
    } else if (ch == '\'') {
	return "'\\''";

    } else {
	sprintf(buf, "'%c'", ch);
	return buf;
    }
}

// Scale the size to the nearest (larger) "Ki" (1024) unit.
// 0 = > 0K, 1 => 1K, 2 => 1K, ..., 1023 => 1K, 1024 => 1K, 1025 => 2K
//
size_t NormalizeSize(size_t size, char *pSuffix)
{
    double fsize = size;
    char *suffix = "KMGT";

    fsize /= 1024;

    while (fsize > 999 && suffix[1] != '\0') {
	fsize /= 1024;
	suffix++;
    }

    if (pSuffix != NULL)
	*pSuffix = *suffix;

    return ceil(fsize);
}

/*
**  Error & Notification Functions
**
**  Note that calling Error will exit the program.
*/

void Note(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    if (!gQuiet) {
	fprintf(stdout, "[");
	vfprintf(stdout, fmt, args);
	fprintf(stdout, "]\n");
    }
    va_end(args);
}

void Warn(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    if (!gQuiet) {
	fprintf(stdout, "%%");
	vfprintf(stdout, fmt, args);
	fprintf(stdout, "\n");
    }
    va_end(args);

    gWarnings++;
}

void Error(int err, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    fprintf(stderr, "?");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);

    if (err != EX_OK)
	exit(err);
}

/*
**  Memory Functions
**
**  Note that all memory allocations are guaranteed to succeed
**  or else abort the running program with an error.
**
**  Also note that it's OK to (re)alloc and free a NULL memory
**  pointer.  (The right thing will happen.)
**/

void xfree(void *mem)
{
    if (mem != NULL)
	free(mem);
}

void *xalloc(void *mem, size_t size)
{
    if (mem == NULL)
	mem = malloc(size);
    else
	mem = realloc(mem, size);

    if (mem == NULL && size == 0) {
	// Sigh, some mallocs don't like it when you ask them for zero bytes
	xfree(mem);
	mem = malloc(1);
    }

    if (mem == NULL)
	Error(EX_UNAVAILABLE, "Out of memory when trying to allocate %u bytes",
	      size);

    return mem;
}

/*
**  String Functions
**
**  Strings are arrays of chars with a specific length.  Note that unlike
**  C-strings, they aren't necessarily null terminated.  To get a (temporary)
**  null terminated string, use String_CString or one of its siblings.
**
**  The String's underlying chars can be shared (never freed by String_Free),
**  allocated (freed by Sting_Free), mapped (unmapped by String_Free),
**  or by part of compiled in constant memory (never freed by String_Free,
**  nor is the actual String * pointer).  Note that the latter makes it
**  harmless to call String_Free with a compiled-in string constant.
**  (This fact is used among other places in Header_Free when freeing they
**  header's keys, which often will be compiled in Str_ constants.)
**
**  It is very common for strings to share memory with other strings.  No
**  reference counting is performed, so it's up to the caller to make sure
**  that the memory owning strings doesn't get freed as long as there are
**  refereces still in use to its chars.
*/

inline const char *String_Chars(const String *str)
{
    return str == NULL ? NULL : str->buf;
}

inline int String_Length(const String *str)
{
    return str == NULL ? 0 : str->len;
}

inline const char *String_End(const String *str)
{
    return str == NULL ? NULL : str->buf + str->len;
}

inline const String *String_Safe(String *str)
{
    return str != NULL ? str : &Str_Emtpy;
}

inline void String_SetChars(String *str, const char *buf)
{
    str->buf = buf;
}

inline void String_SetLength(String *str, int len)
{
    str->len = len;
}

inline void String_Set(String *str, const char *buf, int len)
{
    str->buf = buf;
    str->len = len;
}

inline String *String_New(StringType type, const char *chars, int length)
{
    String *str = New(String);
    String_Set(str, chars, length);
    return str;
}

inline String *String_Sub(const String *str, int start, int end)
{
    return String_New(kString_Shared, String_Chars(str) + start, end - start);
}

String *String_Clone(const String *str)
{
    if (str == NULL)
	return NULL;

    return String_New(kString_Shared, String_Chars(str), String_Length(str));
}

String *String_Alloc(int length)
{
    return String_New(kString_Alloced, xalloc(NULL, length), length);
}

String *String_FromCString(const char *chars, bool copy)
{
    if (chars == NULL)
	return NULL;

    int len = strlen(chars);

    if (copy) {
	String *str = String_Alloc(len);
	memcpy((char *) String_Chars(str), chars, len);
	return str;
    }

    return String_New(kString_Shared, chars, len);
}

void String_Free(String *str)
{
    if (str != NULL) {
	switch (str->type) {
	  case kString_Shared:
	    xfree(str);
	    break;

	  case kString_Alloced:
	    xfree((void *) str->buf);
	    xfree(str);
	    break;

	  case kString_Mapped:
	    munmap((void *) str->buf, String_Length(str));
	    xfree(str);
	    break;

	  case kString_Const:
	    /* Nothing to free -- it's all static memory */
	    break;
	}
    }
}

void String_FreeP(String **pStr)
{
    if (pStr != NULL) {
	String_Free(*pStr);
	*pStr = NULL;
    }
}

// Change the string by adjusting its start and length
//
inline void String_Adjust(String *str, int offset)
{
    str->buf += offset;
    str->len -= offset;
}

inline bool String_IsEqual(const String *a, const String *b, bool sameCase)
{
    if (String_Length(a) != String_Length(b))
	return false;

    return sameCase ?
	strncmp(String_Chars(a), String_Chars(b), String_Length(b)) == 0 :
	strncasecmp(String_Chars(a), String_Chars(b), String_Length(b)) == 0;
}

inline bool String_HasPrefix(const String *str, const String *sub,
			     bool sameCase)
{
    if (String_Length(str) < String_Length(sub))
	return false;

    return sameCase ?
	strncmp(String_Chars(str), String_Chars(sub), String_Length(sub)) == 0 :
	strncasecmp(String_Chars(str), String_Chars(sub),
		    String_Length(sub)) == 0;
}

inline int String_Compare(const String *a, const String *b, bool sameCase)
{
    const char *ap, *bp;
    int i, minlen = iMin(String_Length(a), String_Length(b));

    for (i = 0, ap = String_Chars(a), bp = String_Chars(b);
	 i < minlen; i++, ap++, bp++) {
	int cmp = sameCase ? *ap - *bp : tolower(*ap) - tolower(*bp);

	if (cmp != 0)
	    return cmp;
    }

    return String_Length(a) - String_Length(b);
}

inline int String_CompareCI(const String *a, const String *b)
{
    return String_Compare(a, b, false);
}

inline int String_CompareCS(const String *a, const String *b)
{
    return String_Compare(a, b, true);
}

int _String_FindTwoChars(const String *str, char ach, char bch)
{
    const char *p = String_Chars(str);
    const char *end = p + String_Length(str);

    while (p < end) {
	if (*p == ach || *p == bch)
	    return p - String_Chars(str);
	p++;
    }

    return -1;
}

int _String_FindLastTwoChars(const String *str, char ach, char bch)
{
    const char *begin = String_Chars(str);
    const char *p = begin + String_Length(str);

    while (begin < p) {
	p--;
	if (*p == ach || *p == bch)
	    return p - begin;
    }

    return -1;
}

int String_FindChar(const String *str, char ch, bool sameCase)
{
    if (!sameCase) {
	char lch = tolower(ch);
	char uch = toupper(ch);

	if (lch != uch)
	    return _String_FindTwoChars(str, lch, uch);
    }

    const char *p = memchr(String_Chars(str), ch, String_Length(str));

    if (p == NULL)
	return -1;

    return p - String_Chars(str);
}

int String_FindLastChar(const String *str, char ch, bool sameCase)
{
    if (!sameCase) {
	char lch = tolower(ch);
	char uch = toupper(ch);

	if (lch != uch) {
	    return _String_FindLastTwoChars(str, lch, uch);
	}
    }

    // I wish there was a memrchr()...

    const char *begin = String_Chars(str);
    const char *p = begin + String_Length(str);

    while (begin < p--) {
	if (*p == ch)
	    return p - begin;
    }

    return -1;
}

int String_FindString(const String *str, const String *sub, bool sameCase)
{
    // Degenerate case: The empty string is a substring of all other strings
    //
    if (String_Length(sub) == 0)
	return 0;

    String tmp = *str;
    char firstChar = *String_Chars(sub);
    int offset = 0;
    int pos;

    while ((pos = String_FindChar(&tmp, firstChar, sameCase)) != -1) {
	String_Adjust(&tmp, pos);
	offset += pos;

	if (String_HasPrefix(&tmp, sub, sameCase))
	    return offset;

	// Skip past found char
	offset++;
	String_Adjust(&tmp, 1);
    }

    return -1;
}

inline int String_FindNewline(const String *str)
{
    return _String_FindTwoChars(str, '\r', '\n');
}

void String_TrimSpaces(String *str)
{
    const char *chars = String_Chars(str);
    int b, e, len = String_Length(str);

    for (b = 0; b < len && isspace(chars[b]); b++);
    for (e = len - 1; e >= b && isspace(chars[e]); e--);

    if (str->type == kString_Alloced) {
	// Oops, can't just reset pointer
	memcpy((char *) chars, chars + b, e - b + 1);
	String_SetLength(str, e - b + 1);
    } else {
	String_Set(str, chars + b, e - b + 1);
    }
}

// Return a temporary, null-terminated const char *string that is using
// shared storage (will be overwritten on every nth call).
char **String_NextCStringPoolSlot(void)
{
    static char *pool[kString_CStringPoolSize] = {0};
    static int counter = -1;

    counter = (counter + 1) % kString_CStringPoolSize;

    return &pool[counter];
}

const char *String_CString(const String *str)
{
    if (str == NULL)
	return NULL;

    char **pdup = String_NextCStringPoolSlot();
    int len = String_Length(str);

    *pdup = xalloc(*pdup, len + 1);
    memcpy(*pdup, String_Chars(str), len);
    (*pdup)[len] = '\0';

    return *pdup;
}

const char *String_QuotedCString(const String *str, int maxLength)
{
    if (str == NULL)
	return "(null)";

    char **pdup = String_NextCStringPoolSlot();    
    const char *chars = String_Chars(str);
    int i, len = String_Length(str);
    char *b;
    int extra = 2 + 3 + 1; // Need room for quotes, ellipsis, and '\0'

    // Trim the data we're going to be looking at if it's too long
    //
    if (maxLength >= 0 && maxLength < len)
	len = maxLength;

    // Non-printing characters need extra space for \x or \nnn format
    //
    for (i = 0; i < len; i++) {
	if (chars[i] < ' ' || chars[i] > '~' || chars[i] == '"') {
	    extra += 3;
	}
    }

    b = *pdup = xalloc(*pdup, len + extra);

    *b++ = '"';
    for (i = 0; i < len; i++) {
	char ch = chars[i];

	if (ch < ' ' || ch > '~' || ch == '"') {
	    *b++ = '\\';
	    switch (ch) {
	      case '\n': *b++ = 'n'; break;
	      case '\r': *b++ = 'r'; break;
	      case '\t': *b++ = 't'; break;
	      case '"':  *b++ = '"'; break;
	      default:
		b += sprintf(b, "%03o", ch & 0xff);
		break;
	    }
	} else {
	    *b++ = ch;
	}
    }
    *b++ = '"';

    if (len < String_Length(str)) {
	strcpy(b, "...");
	b += 3;
    }
    *b = '\0';

    return *pdup;
}

// Return a "pretty" C string that is can be printed and read
// as a single entity without confusion.  It will be the "raw"
// string for short, simple, printable, single-word strings
// and quoted for the rest.
// 
const char *String_PrettyCString(const String *str)
{
    const char *chars = String_Chars(str);
    int i, len = String_Length(str);

    if (len == 0 || len > kString_MaxPrettyLength)
	return String_QuotedCString(str, kString_MaxPrettyLength);

    for (i = 0; i < len; i++) {
	if (chars[i] <= ' ' || chars[i] > '~')
	    return String_QuotedCString(str, kString_MaxPrettyLength);
    }

    return String_CString(str);
}

String *String_PrintF(const char *fmt, ...)
{
    char buf[kString_MaxPrintFLength];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf)-1, fmt, args);
    va_end(args);
    // Ensure null termination (vsnprintf won't necessarily add null)
    buf[sizeof(buf)-1] = '\0';

    int len = strlen(buf);
    char *dup = xalloc(NULL, len);
    memcpy(dup, buf, len);

    return String_New(kString_Alloced, dup, len);
}

String *String_ByteSize(size_t size)
{
    char suffix;

    size = NormalizeSize(size, &suffix);

    return String_PrintF("%d%cB", size, suffix);
}

/*
**  Array Functions
**
**  Arrays are dynamic and will grow automatically as more items are added
**  to them.  They store opaque void * pointers.  If given a liberator
**  function, the items will be freed when replaced or when the array is
**  reset / freed itself; otherwise, potentially freeing the items when they
**  become unused will be the callers responsibility.
*/

Array *Array_New(int initialSize, Free *liberator)
{
    Array *array = New(Array);
    if (initialSize > 0) {
	array->size = initialSize;
	array->items = xalloc(NULL, array->size * sizeof(void *));
    }
    array->liberator = liberator;

    return array;
}

void *Array_Items(const Array *array)
{
    return array->items;
}

int Array_Count(const Array *array)
{
    return array->count;
}

inline void _Array_CheckBounds(const Array *array, int ix)
{
    if (ix < 0 || ix >= Array_Count(array)) {
	Error(EX_UNAVAILABLE, "Out of bounds reference to array %#x at %d",
	      (unsigned) array, ix);
    }
}

inline void *Array_GetAt(const Array *array, int ix)
{
    _Array_CheckBounds(array, ix);
    return array->items[ix];
}

void _Array_EnsureSpace(Array *array, int space)
{
    if (array->count + space > array->size) {
	// Not enough space, grow it until there is
	//
	while (array->count + space > array->size) {
	    if (array->items == NULL)
		array->size = kArray_InitialSize;
	    else
		array->size *= kArray_GrowthFactor;
	}

	array->items = xalloc(array->items, array->size * sizeof(void *));
    }
}

void Array_InsertAt(Array *array, int ix, void *item)
{
    if (ix != Array_Count(array))
	_Array_CheckBounds(array, ix); 

    _Array_EnsureSpace(array, 1);
    memmove(&array->items[ix + 1], &array->items[ix],
	    (Array_Count(array) - ix) * sizeof(void *));
    array->items[ix] = item;
    array->count++;
}

void Array_ReplaceAt(Array *array, int ix, void *item)
{
    _Array_CheckBounds(array, ix);
    if (array->liberator != NULL && array->items[ix] != NULL)
	(*array->liberator)(array->items[ix]);
    array->items[ix] = item;
}

void Array_DeleteAt(Array *array, int ix)
{
    _Array_CheckBounds(array, ix);
    if (array->liberator != NULL && array->items[ix] != NULL)
	(*array->liberator)(array->items[ix]);

    memmove(&array->items[ix], &array->items[ix + 1],
	    (Array_Count(array) - ix - 1) * sizeof(void *));
    array->count--;
}

void Array_Append(Array *array, const void *item)
{
    _Array_EnsureSpace(array, 1);
    array->items[array->count++] = (void *) item;
}

void Array_Reset(Array *array)
{
    if (array->liberator != NULL) {
	int i;
	for (i = 0; i < Array_Count(array); i++) {
	    (*array->liberator)(Array_GetAt(array, i));
	}
    }

    xfree(array->items);
    array->items = NULL;
    array->count = 0;
    array->size = 0;
}

void Array_Free(Array *array)
{
    Array_Reset(array);
    xfree(array);
}

/**
 **  Parser Functions
 **
 **  Parsers move a virtual pointer through an underlying String,
 **  returning a (shared) strings pointing to the chars "consumed"
 **  by each call.  They also have an internal "rest" String that
 **  can be queried to find out how much is left of the original
 **  String.
 **/

void Parser_Set(Parser *par, const String *str)
{
    if (str == NULL) {
	par->start = NULL;
	String_Set(&par->rest, NULL, 0);
    } else {
	par->start = String_Chars(str);
	par->rest = *str;
    }
}

int Parser_Position(Parser *par)
{
    return String_Chars(&par->rest) - par->start;
}

bool Parser_AtEnd(Parser *par)
{
    return String_Length(&par->rest) == 0;
}

bool Parser_MoveTo(Parser *par, int pos)
{
    int len = String_Length(&par->rest);
    int totLen = Parser_Position(par) + len;

    if (pos < 0 || pos > totLen)
	return false;

    String_Set(&par->rest, par->start + pos, totLen - pos);

    return true;
}

bool Parser_Move(Parser *par, int count)
{
    return Parser_MoveTo(par, Parser_Position(par) + count);
}

int Parse_Peek(Parser *par)
{
    if (String_Length(&par->rest) == 0)
	return EOF;
    return *String_Chars(&par->rest);
}

bool Parse_Char(Parser *par, char *pch)
{
    if (String_Length(&par->rest) == 0) {
	if (pch != NULL)
	    *pch = '\0';
	return false;
    }

    if (pch != NULL)
	*pch = *String_Chars(&par->rest);

    String_Set(&par->rest, String_Chars(&par->rest) + 1,
	       String_Length(&par->rest) - 1);

    return true;
}

void Parse_StringStart(Parser *par, String **pResult)
{
    *pResult = New(String);
    String_SetChars(*pResult, String_Chars(&par->rest));
}

void Parse_StringEnd(Parser *par, String *str)
{
    String_SetLength(str, String_Chars(&par->rest) - String_Chars(str));    
}

bool Parse_ConstChar(Parser *par, char ex, bool sameCase, String **pResult)
{
    if (String_Length(&par->rest) == 0)
	return false;

    char ch = *String_Chars(&par->rest);

    if (ch == ex || (!sameCase && tolower(ch) == tolower(ex))) {
	if (pResult != NULL)
	    *pResult = String_Sub(&par->rest, 0, 1);

	Parser_Move(par, 1);
	return true;
    }

    return false;
}

bool Parse_ConstString(Parser *par, const String *expect, bool sameCase,
		       String **pResult)
{
    if (!String_HasPrefix(&par->rest, expect, sameCase))
	return false;

    if (pResult != NULL)
	*pResult = String_Sub(&par->rest, 0, String_Length(expect));

    Parser_Move(par, String_Length(expect));
    return true;
}

bool Parse_Spaces(Parser *par, String **pResult)
{
    const char *p = String_Chars(&par->rest);
    const char *end = p + String_Length(&par->rest);

    for (; p < end && (*p == '\t' || *p == ' '); p++);

    int len = p - String_Chars(&par->rest);

    if (len == 0)
	return false;

    if (pResult != NULL)
	*pResult = String_New(kString_Shared, String_Chars(&par->rest), len);

    Parser_Move(par, len);

    return true;
}

bool Parse_Newline(Parser *par, String **pResult)
{
    const char *p = String_Chars(&par->rest);
    const char *end = p + String_Length(&par->rest);

    if (p < end && *p == '\r')
	p++;
    if (p < end && *p == '\n')
	p++;

    int len = p - String_Chars(&par->rest);

    if (len == 0)
	return false;

    Parser_Move(par, len);
    return true;
}

bool Parse_UntilNewline(Parser *par, String **pResult)
{
    int pos = String_FindNewline(&par->rest);

    if (pos == -1) {
	if (pResult != NULL)
	    *pResult = NULL;
	return false;
    }

    if (pResult != NULL)
	*pResult = String_New(kString_Shared, String_Chars(&par->rest), pos);

    Parser_Move(par, pos);

    return true;
}

bool Parse_UntilChar(Parser *par, char ch, bool sameCase, String **pResult)
{
    int pos = String_FindChar(&par->rest, ch, sameCase);

    if (pos == -1) {
	if (pResult != NULL)
	    *pResult = NULL;
	return false;
    }

    if (pResult != NULL) {
	*pResult = New(String);
	String_Set(*pResult, String_Chars(&par->rest), pos);
    }

    Parser_Move(par, pos);

    return true;
}

bool Parse_UntilSpace(Parser *par, String **pResult)
{
    return Parse_UntilChar(par, ' ', true, pResult);
}

bool Parse_UntilString(Parser *par, const String *expect, bool sameCase,
		       String **pResult)
{
    int pos = String_FindString(&par->rest, expect, sameCase);

    if (pos == -1) {
	if (pResult != NULL)
	    *pResult = NULL;
	return false;
    }

    if (pResult != NULL) {
	*pResult = New(String);
	String_Set(*pResult, String_Chars(&par->rest), pos);
    }

    Parser_Move(par, pos);

    return true;
}

bool Parse_UntilCString(Parser *par, const char *expect, bool sameCase,
		       String **pResult)
{
    String tmp = {expect, strlen(expect)};
    return Parse_UntilString(par, &tmp, sameCase, pResult);
}

bool Parse_UntilEnd(Parser *par, String **pResult)
{
    if (pResult != NULL) {
	*pResult = New(String);
	**pResult = par->rest;
    }

    Parser_Move(par, String_Length(&par->rest));

    return true;
}

// Parse until the next newline (or the end of the data)
// and return all the text up to, but not including the newline.
// (But if a newline was found, go eat it too.)
//
bool Parse_Line(Parser *par, String **pLine)
{
    if (Parse_UntilNewline(par, pLine))
	return Parse_Newline(par, NULL);

    return Parse_UntilEnd(par, pLine);
}

bool Parse_Integer(Parser *par, int *pResult)
{
    const char *p = String_Chars(&par->rest);
    const char *end = p + String_Length(&par->rest);
    int num = 0;

    while (p < end && isdigit(*p)) {
	num = (num * 10) + *p++ - '0';
    }

    int count = p - String_Chars(&par->rest);

    if (count == 0)
	return false;

    Parser_Move(par, count);

    if (pResult != NULL)
	*pResult = num;

    return true;
}

int String_ToInteger(String *str, int defValue)
{
    Parser tmp;
    int num;

    if (str == NULL)
	return defValue;

    Parser_Set(&tmp, str);
    if (Parse_Integer(&tmp, &num))
	return num;

    return defValue;
}

/*
**  Stream Functions
**
**  Streams are basically a combination of a FILE * and a filename.
**  All write operations are guaranteed to succeed (or abort the program)
**  unless ignoreErrors is set.
*/

void Stream_SetIgnoreErrors(Stream *stream, bool flag)
{
    stream->ignoreErrors = flag;
}

Stream *Stream_New(FILE *file, String *name, bool cloneName)
{
    Stream *stream = New(Stream);

    stream->file = file;
    stream->name = cloneName ? String_Clone(name) : name;

    return stream;
}

Stream *Stream_Open(String *path, bool write)
{
    FILE *file;

    if (path == NULL) {
	file = write ? stdout : stdin;
	path = String_FromCString(write ? "(stdout)" : "(stdin)", false);

    } else {
	const char *cPath = String_CString(path);
	file = fopen(cPath, write ? "w" : "r");

	if (file == NULL)
	    Error(write ? EX_CANTCREAT : EX_NOINPUT, "Can't open file %s: %s",
		  cPath, strerror(errno));

	path = String_Clone(path);
    }

    return Stream_New(file, path, false);
}

Stream *Stream_OpenTemp(String *path, bool write)
{
    int len = String_Length(path);
    char template[len + 1 + 6 + 1];

    memcpy(template, String_Chars(path), len);
    strcpy(template + len, "-XXXXXX");

    int fd = mkstemp(template);
    if (fd == -1)
	Error(EX_CANTCREAT, "Can't create temporary file %s", template);

    return Stream_New(fdopen(fd, write ? "w" : "r"),
		      String_FromCString(template, true), false);
}

void Stream_Close(Stream *stream)
{
    if (fclose(stream->file) != 0) {
	Error(EX_IOERR, "%s: %s", strerror(errno),
	      String_CString(stream->name));
    }

    stream->file = NULL;
}

void Stream_Free(Stream *stream, bool close)
{
    if (close)
	Stream_Close(stream);
    String_Free(stream->name);
    stream->name = NULL;
}

bool Stream_ReadContents(Stream *input, String **pContents)
{
    int fd = fileno(input->file);
    struct stat sbuf;
    char *data = NULL;
    StringType type = kString_Shared;
    size_t size = -1;

    // See if we can find out how big it's going to be
    //
    if (fstat(fd, &sbuf) == 0)
	size = sbuf.st_size;

    // First try mapping the file
    //
    if (gMap && size > 0) {
	data = mmap(NULL, size, PROT_READ, MAP_FILE | MAP_PRIVATE, fd, 0);
	type = kString_Mapped;

	if (data == (void *) -1) {
	    Warn("Could not mmap file %s: %s",
		 String_CString(input->name), strerror(errno));
	    data = NULL;
	}
    }

    if (data == NULL) {
	int offset = 0;
	int count;
	
	if (size == -1)
	    size = kRead_InitialSize;

	data = xalloc(NULL, size);
	type = kString_Alloced;

	while ((count = read(fd, data + offset, size - offset)) > 0) {
	    offset += count;
	    if (offset == size) {
		size *= kRead_GrowthFactor;
		data = xalloc(data, size);
	    }
	}

	size = offset;
	data = xalloc(data, size);

	if (count < 0) {
	    xfree(data);
	    data = NULL;
	}
    }

    if (data == NULL)
	return false;

    *pContents = String_New(type, data, size);

    return true;
}

void Stream_WriteChar(Stream *output, char ch)
{
    if (putc(ch, output->file) == EOF && !output->ignoreErrors)
	Error(EX_IOERR, "Could not write 1 byte to %s: %s",
	      String_CString(output->name), strerror(errno));
}

void Stream_WriteChars(Stream *output, const char *chars, int len)
{
    if (len > 0 && fwrite(chars, len, 1, output->file) != 1 &&
	!output->ignoreErrors)
	Error(EX_IOERR, "Could not write %d byte%s to %s: %s",
	      len, len == 1 ? "" : "s", 
	      String_CString(output->name), strerror(errno));
}

void Stream_WriteNewline(Stream *output)
{
    Stream_WriteChar(output, '\n');
}

void Stream_WriteString(Stream *output, const String *str)
{
    Stream_WriteChars(output, String_Chars(str), String_Length(str));
}

void Stream_PrintF(Stream *output, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vfprintf(output->file, fmt, args);
    va_end(args);
}

/**
 **  String <-> Array Functions
 **/

String *Array_Join(Array *array, const String *delim)
{
    int i, count = Array_Count(array);
    int newLength = 0;

    if (count == 0)
	return NULL;

    for (i = 0; i < count; i++) {
	String *str = Array_GetAt(array, i);
	newLength += String_Length(str);
    }

    int delimLen = String_Length(delim);
    String *newString = String_Alloc(newLength + count * delimLen);
    char *newChars = (char *) String_Chars(newString);

    for (i = 0; i < count; i++) {
	String *str = Array_GetAt(array, i);
	int len = String_Length(str);

	if (i > 0 && delimLen > 0) {
	    memcpy(newChars, String_Chars(delim), delimLen);
	    newChars += delimLen;
	}
	memcpy(newChars, String_Chars(str), len);
	newChars += len;
    }

    return newString;
}

Array *String_Split(const String *str, char separator, bool trim)
{
    Array *words = Array_New(0, (Free *) String_Free);
    Parser parser;
    String *word;
    
    Parser_Set(&parser, str);

    for (;;) {

	if (trim)
	    Parse_Spaces(&parser, NULL);

	if (!Parse_UntilChar(&parser, separator, false, &word))
	    break;

	if (trim)
	    String_TrimSpaces(word);

	Array_Append(words, word);
    }

    if (!Parser_AtEnd(&parser) && Parse_UntilEnd(&parser, &word)) {
	if (trim)
	    String_TrimSpaces(word);
	Array_Append(words, word);
    }

    return words;
}

/**
 **  Time Support
 **/

// Weekdays
//
String_Define(Str_Sun, "Sun");
String_Define(Str_Mon, "Mon");
String_Define(Str_Tue, "Tue");
String_Define(Str_Wed, "Wed");
String_Define(Str_Thu, "Thu");
String_Define(Str_Fri, "Fri");
String_Define(Str_Sat, "Sat");

const String *kWeekdays[] = {
    &Str_Sun, &Str_Mon, &Str_Tue, &Str_Wed, &Str_Thu, &Str_Fri, &Str_Sat,
    NULL
};

// Months
//
String_Define(Str_Jan, "Jan");
String_Define(Str_Feb, "Feb");
String_Define(Str_Mar, "Mar");
String_Define(Str_Apr, "Apr");
String_Define(Str_May, "May");
String_Define(Str_Jun, "Jun");
String_Define(Str_Jul, "Jul");
String_Define(Str_Aug, "Aug");
String_Define(Str_Sep, "Sep");
String_Define(Str_Oct, "Oct");
String_Define(Str_Nov, "Nov");
String_Define(Str_Dec, "Dec");

const String *kMonths[] = {
    &Str_Jan, &Str_Feb, &Str_Mar, &Str_Apr, &Str_May, &Str_Jun,
    &Str_Jul, &Str_Aug, &Str_Sep, &Str_Oct, &Str_Nov, &Str_Dec,
    NULL
};

// Returns NULL if none was found
//
static int ParseKeyword(Parser *par, const String **keywords)
{
    const String **sp;

    for (sp = keywords; *sp != NULL; sp++) {
	if (Parse_ConstString(par, *sp, true, NULL)) {
	    return sp - keywords;
	}
    }

    return -1;
}

// Returns -1 if no valid two digit number
//
static int ParseTwoDigits(Parser *par, bool leadingSpaceOK)
{
    char c1, c2;

    if (!Parse_Char(par, &c1) || !Parse_Char(par, &c2))
	return -1;

    if (c1 == ' ')
	c1 = '0';

    if (!isdigit(c1) || !isdigit(c2))
	return -1;

    return (c1 - '0') * 10 + (c2 - '0');
}

static bool ParseCTimeHelper(Parser *par, struct tm *pTime)
{
    // Parse "www dd mmm hh:mm:ss yyyy"

    int wday = ParseKeyword(par, kWeekdays);

    if (!Parse_ConstChar(par, ' ', true, NULL))
	return false;

    int day = ParseTwoDigits(par, true);
    if (day == -1)
	return false;

    int mon = ParseKeyword(par, kMonths);

    if (!Parse_ConstChar(par, ' ', true, NULL))
	return false;

    int hour = ParseTwoDigits(par, false);
    if (hour == -1)
	return false;

    if (!Parse_ConstChar(par, ':', true, NULL))
	return false;
    
    int min = ParseTwoDigits(par, false);
    if (min == -1)
	return false;

    if (!Parse_ConstChar(par, ':', true, NULL))
	return false;
    
    int sec = ParseTwoDigits(par, false);
    if (sec == -1)
	return false;

    if (!Parse_ConstChar(par, ' ', true, NULL))
	return false;

    int y1 = ParseTwoDigits(par, false);
    int y2 = ParseTwoDigits(par, false);

    if (y1 == -1 || y2 == -1)
	return false;

    int year = y1 * 100 + y2;

    if (pTime != NULL) {
	pTime->tm_sec = sec;
	pTime->tm_min = min;
	pTime->tm_hour = hour;
	pTime->tm_mday = day;
	pTime->tm_mon = mon;
	pTime->tm_year = year;
	pTime->tm_wday = wday;
    }

    return true;
}

bool Parse_CTime(Parser *par, struct tm *pTime)
{
    int pos = Parser_Position(par);
    bool result = ParseCTimeHelper(par, pTime);
    if (!result)
	Parser_MoveTo(par, pos);
    return result;
}

String *String_RFC822Date(struct tm *tm, bool withTimeZone)
{
    if (withTimeZone)
	return String_PrintF("%s, %2d %s %4d %02d:%02d:%02d",
			     String_CString(kWeekdays[tm->tm_wday]),
			     tm->tm_mday, String_CString(kMonths[tm->tm_mon]),
			     tm->tm_year, tm->tm_hour, tm->tm_min, tm->tm_sec);
    else
	return String_PrintF("%s, %2d %s %4d %02d:%02d:%02d %c%02d%02d",
			     String_CString(kWeekdays[tm->tm_wday]),
			     tm->tm_mday, String_CString(kMonths[tm->tm_mon]),
			     tm->tm_year, tm->tm_hour, tm->tm_min, tm->tm_sec,
			     tm->tm_gmtoff > 0 ? '+' : '-',
			     abs(tm->tm_gmtoff / 3600),
			     abs(tm->tm_gmtoff / 60 % 60));
}

void Stream_WriteCTime(Stream *output, struct tm *tm)
{
    // www mmm dd hh:mm:ss yyyy

    Stream_PrintF(output, "%s %s %02d %02d:%02d:%02d %4d\n",
		  String_CString(kWeekdays[tm->tm_wday]),
		  String_CString(kMonths[tm->tm_mon]), tm->tm_mday,
		  tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year);
}


/**
 **  Dirty Functions
 **
 **  (Must be declared first.)
 **/

void Mailbox_SetDirty(Mailbox *mbox, bool flag)
{
    mbox->dirty = flag;
}

void Message_SetDirty(Message *msg, bool flag)
{
    msg->dirty = flag;
    if (flag)
	Mailbox_SetDirty(msg->mbox, flag);
}

/**
 **  Header Functions
 **
 **  Headers construct implicit list of {key, value} pairs of Strings.
 **  Note that these strings are owned by the Headers and that calling
 **  Header_Set et al WILL TRANSFER OWNERSHIP of both keys and values
 **  (i.e. they will be freed with String_Free when the header is freed).
 **/

void Header_Free(Header *header, bool all)
{
    Header *next;

    for (; header != NULL; header = next) {
	next = header->next;

	String_Free(header->key);
	String_Free(header->value);
	String_Free(header->line);
	xfree(header);

	if (!all)
	    break;
    }
}

void Headers_Free(Headers *headers)
{
    Header_Free(headers->root, true);
    xfree(headers);
}

Header *Header_Find(Headers *headers, const String *key)
{
    Header *head;

    for (head = headers->root; head != NULL; head = head->next) {
	if (String_IsEqual(head->key, key, false))
	    return head;
    }

    return NULL;
}

Header *Header_FindLast(Headers *headers, const String *key)
{
    Header *head, *last = NULL;

    for (head = headers->root; head != NULL; head = head->next) {
	if (String_IsEqual(head->key, key, false))
	    last = head;
    }

    return last;
}

String *Header_Get(Headers *headers, const String *key)
{
    Header *head = Header_Find(headers, key);

    if (head == NULL)
	return NULL;

    return head->value;
}

String *Header_GetLastValue(Headers *headers, const String *key)
{
    Header *last = Header_FindLast(headers, key);

    if (last == NULL)
	return NULL;

    return last->value;
}

Header ** _Header_FindP(Header **pHead, const String *key)
{
    for (; *pHead != NULL; pHead = &(*pHead)->next) {
	if (String_IsEqual((*pHead)->key, key, false)) {
	    break;
	}
    }

    return pHead;
}

void Header_Set(Headers *headers, const String *key, String *value)
{
    Header **pHead = _Header_FindP(&headers->root, key);
    
    if (*pHead != NULL) {
	String_Free((*pHead)->value);
	(*pHead)->value = value;
	String_Free((*pHead)->line);
	(*pHead)->line = NULL;

    } else {
	*pHead = New(Header);
	(*pHead)->key = (String *) key;		// XXX: Clone?
	(*pHead)->value = value;
    }

    Message_SetDirty(headers->msg, true);
}

void Header_Append(Headers *headers, String *key, String *value)
{
    Header **pHead;

    for (pHead = &headers->root; *pHead != NULL; pHead = &(*pHead)->next);

    *pHead = New(Header);
    (*pHead)->key = key;
    (*pHead)->value = value;

    Message_SetDirty(headers->msg, true);
}

void Header_Delete(Headers *headers, const String *key, bool all)
{
    Header **pHead = &headers->root;

    while (*(pHead = _Header_FindP(pHead, key)) != NULL) {
	Header *next = (*pHead)->next;
	Header_Free(*pHead, false);
	*pHead = next;

	Message_SetDirty(headers->msg, true);

	if (!all)
	    break;
    }
}

bool Parse_Header(Parser *par, Header **phead)
{
    Header *head;
    int warnCount;
    char ch;

    // Must start with alpha-numeric char
    //
    if (!isalnum(Parse_Peek(par)))
	return false;

    head = New(Header);

    // Parse header name
    //
    Parse_StringStart(par, &head->line);
    Parse_StringStart(par, &head->key);
    warnCount = 0;
    while (Parse_Char(par, &ch) && ch != ':') {
	if (gCheck && !isalnum(ch) && ch != '-' && ch != '_') {
	    if (++warnCount < kCheck_MaxWarnCount)
		Warn("Illegal character %s in message headers%s {@%d}",
		     Char_QuotedCString(ch), Parser_Position(par),
		     warnCount == kCheck_MaxWarnCount ? " (and more)" : "");
	}
    }
    Parse_StringEnd(par, head->key);
    // Backup over the colon
    head->key->len--;
    String_TrimSpaces(head->key);

    // Parse header value
    //
    Parse_Spaces(par, NULL);
    Parse_StringStart(par, &head->value);
    for (;;) {
	Parse_UntilNewline(par, NULL);
	Parse_StringEnd(par, head->value);

	// Skip over newline
	//
	Parse_Newline(par, NULL);

	// Stop parsing unless the next line begins with whitespace
	//
	ch = Parse_Peek(par);
	if (ch != ' ' && ch != '\t')
	    break;
    }
    String_TrimSpaces(head->value);
    Parse_StringEnd(par, head->line);

    *phead = head;

    return true;
}

// Parse headers (until & including empty line)
bool Parse_Headers(Parser *par, Message *msg, Headers **pHeaders)
{
    Headers *headers = New(Headers);
    Header **pHead = &headers->root;

    headers->msg = msg;

    while (!Parse_Newline(par, NULL)) {
	if (!Parse_Header(par, pHead)) {
	    Warn("Header parsing ended prematurely for message %s",
		 String_CString(msg->tag));
	    // Should fail here, but it's arguably better to keep what we got
	    break;
	}
	pHead = &(*pHead)->next;
    }

    *pHeaders = headers;

    return true;
}

void Stream_WriteHeaders(Stream *output, Headers *headers)
{
    Header *head;

    for (head = headers->root; head != NULL; head = head->next) {
	if (head->line != NULL) {
	    // Already have a preformatted (orignal) header line
	    Stream_WriteString(output, head->line);
	} else {
	    Stream_WriteString(output, head->key);
	    Stream_WriteChars(output, ": ", 2);
	    Stream_WriteString(output, head->value);
	    Stream_WriteNewline(output);
	}
    }
}

/**
 **  Message Functions
 **/

Message *Message_New(Mailbox *mbox, int num)
{
    Message *msg = New(Message);

    msg->mbox = mbox;
    msg->num = num;

    return msg;
}

void Message_Free(Message *msg, bool all)
{
    Message *next;

    for (; msg != NULL; msg = next) {
	next = msg->next;

	String_Free(msg->tag);
	String_Free(msg->data);
	String_Free(msg->envSender);
	Headers_Free(msg->headers);
	String_Free(msg->body);
	// Don't free msg->cachedID -- it is "owned" by the headers
	xfree(msg);

	if (!all)
	    break;
    }
}

int Message_Count(Message *msg)
{
    int count = 0;

    for (count = 0; msg != NULL; msg = msg->next, count++);

    return count;
}

int Message_BodyLength(Message *msg)
{
    return String_Length(msg->body);
}

String *Message_Body(Message *msg)
{
    // XXX: Automatically retrieve message body if not already cached
    return msg->body;
}

void Message_SetBody(Message *msg, String *body)
{
    msg->body = body;
    Message_SetDirty(msg, true);
}

bool Message_IsDeleted(Message *msg)
{
    return msg->deleted;
}

void Message_SetDeleted(Message *msg, bool flag)
{
    if (msg->deleted != flag) {
	msg->deleted = flag;
	Message_SetDirty(msg, true);
    }
}

bool Parse_FromSpaceLine(Parser *par, String **pEnvSender, struct tm *pEnvTime)
{
    int pos = Parser_Position(par);

    // Got "From "?
    //
    if (!Parse_ConstString(par, &Str_FromSpace, true, NULL))
	return false;

    // There shouldn't be more than one space, but just in case...
    //
    (void) Parse_Spaces(par, NULL);

    // Parse envelope sender
    //
    if (!Parse_UntilSpace(par, pEnvSender)) {
	Parser_MoveTo(par, pos);
	return false;
    }

    // There still shouldn't be more than one space, but just in case...
    //
    (void) Parse_Spaces(par, NULL);

    // Parse envelope date in ctime format
    //
    if (!Parse_CTime(par, pEnvTime) ||
	!Parse_Newline(par, NULL)) {
	String_FreeP(pEnvSender);
	Parser_MoveTo(par, pos);
	return false;
    }

    return true;
}

int ProcessDovecotFromSpaceBugBody(Parser *par, int endPos,
				   DovecotFromSpaceBugType bug,
				   Array *bodyParts)
{
    int xHeadSpace = 0;
    String *part;


    if (bodyParts != NULL)
	Parse_StringStart(par, &part);

    // Dovecot isn't very picky about what's preceeding a legal "From " line,
    // so just look for a single newline instead of two.
    //
    while (Parse_UntilString(par, &Str_NLFromSpace, true, NULL) &&
	   Parser_Position(par) < endPos) {
	if (Parse_Newline(par, NULL) && Parse_FromSpaceLine(par, NULL, NULL)) {
	    // Got one!  Go scan the headers...
	    //
	    while (!Parser_AtEnd(par)) {
		int pos = Parser_Position(par);

		if (Parse_Newline(par, NULL)) {
		    // This is the terminating newline, but maybe
		    // we should include it too?
		    //
		    if ((bug & kDFSB_Newline) != 0) {
			int nllen = Parser_Position(par) - pos;
			xHeadSpace += nllen;

			// Collecting body parts too?
			//
			if (bodyParts != NULL) {
			    Parser_Move(par, -nllen);
			    Parse_StringEnd(par, part);
			    Parser_Move(par, nllen);
			    Array_Append(bodyParts, part);
			    Parse_StringStart(par, &part);
			}
		    }

		    // Go back before the newline so that we can start
		    // looking for a new "\nFrom " immediately again!
		    // (Dovecot thinks that the newline that terminates
		    // the headers could also be the newline that preceeds
		    // the next "From " line, so we need to deal with this.)
		    //
		    Parser_MoveTo(par, pos);
		    break;
		}

		// Looking for Content-Length, X-UID, and Status
		//
		if ((((bug & kDFSB_CLXUID) != 0 &&
		      Parse_ConstString(par, &Str_ContentLength, false, NULL)) ||
		     ((bug & kDFSB_CLXUID) != 0 && 
		      Parse_ConstString(par, &Str_XUID, false, NULL)) ||
		     ((bug & kDFSB_Status) != 0 &&
		      Parse_ConstString(par, &Str_Status, false, NULL))) &&
		    Parse_ConstChar(par, ':', true, NULL) &&
		    Parse_Line(par, NULL)) {
		    // Account for this header
		    //
		    int hlen = Parser_Position(par) - pos;
		    xHeadSpace += hlen;

		    // Collecting body parts too?
		    //
		    if (bodyParts != NULL) {
			Parser_Move(par, -hlen);
			Parse_StringEnd(par, part);
			Parser_Move(par, hlen);
			Array_Append(bodyParts, part);
			Parse_StringStart(par, &part);
		    }

		} else {
		    Parse_Line(par, NULL);
		}
	    }
	}
    }

    if (bodyParts) {
	Parse_UntilEnd(par, NULL);
	Parse_StringEnd(par, part);
	Array_Append(bodyParts, part);
    }

    return xHeadSpace;
}

// Call this function when the Content-Length doesn't seem to be correct
// for the current message being parsed.  We'll look to see if the message
// body contains a "From " line and if so, if someone has inserted an X-UID
// and a (new) Content-Length header afterwards. If so, we discount those and
// check the current message's Content-Length again.  If it matches, we know
// that we have our culprit and create a new body part + move the parser
// forward and return true.  If not, we leave the parser where it is and
// return false.
//
bool TryWorkaroundDovecotFromSpaceBug(Parser *par, Message *msg, int cllen)
{
    // Try these patterns in order
    const DovecotFromSpaceBugType bugTypes[] = {
	kDFSB_CLXUID | kDFSB_Status,
	kDFSB_CLXUID,
	kDFSB_CLXUID | kDFSB_Status | kDFSB_Newline,
	kDFSB_CLXUID | kDFSB_Newline,
	kDFSB_None,
    };
    const DovecotFromSpaceBugType *bugp;
    int savedPos = Parser_Position(par);
    int xHeadSpace = 0;

    // Is there a "From " line between the start of the message body and
    // the current position?
    //
    // Check body twice if necessary, once including space for the Status
    // header, once without it.
    //
    for (bugp = bugTypes; *bugp != kDFSB_None; bugp++) {
	// Go back an extra step to include the newline between the headers
	// and the body of the message (so that we can trap a "From " line
	// if it's the first line of the body).
	//
	Parser_Move(par, -cllen - 1);
	xHeadSpace = ProcessDovecotFromSpaceBugBody(par, savedPos, *bugp, NULL);
	Parser_MoveTo(par, savedPos);

	// Did we find anything, and if so, did it make the Content-Length
	// valid?
	//
	if (xHeadSpace > 0 && Parser_Move(par, xHeadSpace)) {
	    int ch = Parse_Peek(par);

	    // Look for "\nFrom "...
	    //
	    if (ch == 'F') {
		// Hmm, no newline, but maybe we've arrived right at
		// the next message's "From " line instead?  Check to
		// see if a newline preceeds us.  If so, continue as
		// if nothing had happened.
		//
		if (cllen > 0) {
		    Parser_Move(par, -1);
		    ch = Parse_Peek(par);
		    if (ch != '\n') {
			// No good
			Parser_Move(par, 1);
		    }
		}
	    }

	    int pos = Parser_Position(par);

	    if (Parser_AtEnd(par) ||
		(Parse_Newline(par, NULL) && 
		 Parse_FromSpaceLine(par, NULL, NULL))) {
		// Yup!  (God damn it)  
		// Move to the new end and remember this for the future...
		//
		Parser_MoveTo(par, pos);
		msg->dovecotFromSpaceBug = *bugp;
		return true;
	    }

	    Parser_MoveTo(par, savedPos);
	}

    }

    return false;
}

void RepairDovecotFromSpaceBugBody(Message *msg)
{
    Parser parser;
    Array *bodyParts = Array_New(0, (Free *) String_Free);

    Parser_Set(&parser, Message_Body(msg));
    ProcessDovecotFromSpaceBugBody(&parser, String_Length(&parser.rest),
				   msg->dovecotFromSpaceBug, bodyParts);
    String_Free(msg->body);
    msg->body = Array_Join(bodyParts, NULL);
    Array_Free(bodyParts);
    msg->dovecotFromSpaceBug = kDFSB_None;
}

void MoveToEndOfMessage(Parser *par, Message *msg)
{
    int bodyPos = Parser_Position(par);
    String *clstr = Header_Get(msg->headers, &Str_ContentLength);
    int cllen;

    if (clstr != NULL && (cllen = String_ToInteger(clstr, -1)) >= 0) {
	// Great, we have a Content-Length.  Make sure it's good
	// and proper before using it, though.
	//
	// There should be a newline *immediately* after this message
	// followed by EOF or the next message's "From " line, but
	// we'll be a bit flexible and allow for 0-2 newlines to
	// compensate for other mailers' indiscretions.
	//
	int bodyPos = Parser_Position(par);

	if (Parser_Move(par, cllen)) {
	    int endPos = Parser_Position(par);
	    int ch = Parse_Peek(par);

	    if (ch == 'F') {
		// Maybe we get too far?  Go back one char and recheck.
		Parser_Move(par, -1);
		ch = Parse_Peek(par);
	    }

	    // We want either EOF, "\n" EOF, or "\nFrom"
	    //
	    if (Parser_AtEnd(par) ||
		(Parse_Newline(par, NULL) &&
		 (Parser_AtEnd(par) ||
		  Parse_ConstString(par, &Str_FromSpace, true, NULL)))) {
		// Great!  Move back to where we were.
		//
		Parser_MoveTo(par, endPos);
		return;

	    } else if (TryWorkaroundDovecotFromSpaceBug(par, msg, cllen)) {
		// Didn't find the "From " line were we expected it,
		// but we did find a case of the Dovecot bug that splits
		// up messages, adding extranoues headers.
		//
		Warn("Working around Dovecot bug for incorrectly "
		     "split message %s", String_CString(msg->tag));
		return;

	    } else {
		// Couldn't find a proper "From " line were we expected
		// it.  Start scanning at the beginning of the message
		// and make note of the last "From " lines we find before
		// the (alleged) end of the message + the first one
		// after.  Then see which one gives the least error.
		//
		Parser_MoveTo(par, bodyPos);

		int lastPos = -1;
		int fromPos = -1;

		while (Parse_UntilString(par, &Str_NL2FromSpace, true, NULL)) {
		    // Remember the pos between the newlines -- that's
		    // the proper (tentative) end of the message.
		    //
		    fromPos = Parser_Position(par) + 1;
		    if (fromPos > endPos)
			break;

		    // Advance past one newline
		    Parser_Move(par, 1);

		    lastPos = fromPos;
		}

		// OK, let's see what we got...  Choose whatever pos
		// is closest to the declared end.
		//
		if (fromPos == -1) {
		    // Never found *any* "From " line -- go to end
		    //
		    Parse_UntilEnd(par, NULL);
		    fromPos = Parser_Position(par);

		} else {
		    if (fromPos != lastPos &&
			endPos - lastPos < fromPos - endPos)
			fromPos = lastPos;
		}

		// Don't bother warning about +/-1 -- that's just a matter
		// of newline strictness.
		//
		if (gPicky || abs(cllen - (fromPos - bodyPos)) > 1)
		    Warn("Incorrect Content-Length: %d for message %s; "
			 "using %d",
			 cllen, String_CString(msg->tag), fromPos - bodyPos);

		Parser_MoveTo(par, fromPos);
		return;
	    }
	}
    }

    // Invalid or missing Content-Length, search for "\n\nFrom " or
    // the end of the data minus one newline instead.
    //
    if (Parse_UntilString(par, &Str_NL2FromSpace, true, NULL)) {
	// Advance past one of the newlines
	//
	Parser_Move(par, 1);

    } else {
	// Go to the end of the mailbox minus one newline
	//
	Parse_UntilEnd(par, NULL);
	Parser_Move(par, -1);
	// Looking at newline?  If not, go to real end (shouldn't happen)
	if (!Char_IsNewline(Parse_Peek(par)))
	    Parser_Move(par, 1);
    }

    if (clstr != NULL) {
	Warn("Invalid Content-Length: %s for message %s; using %d",
	     String_PrettyCString(clstr), String_CString(msg->tag),
	     Parser_Position(par) - bodyPos);
    }
}

bool Parse_Message(Parser *par, Mailbox *mbox, Message **pMsg)
{
    int savedPos = Parser_Position(par);

    // A message must begin with a "From " line or a header,
    // both of which start with alpha-numeric characters.
    //
    if (!isalnum(Parse_Peek(par)))
	return false;

    Message *msg = New(Message);

    msg->mbox = mbox;
    msg->num = ++mbox->count;
    msg->tag = String_PrintF("#%d {@%d}", msg->num, Parser_Position(par));

    Parse_StringStart(par, &msg->data);

    // Allow (expect) for a "From " pseudo-header to start the message
    //
    (void) Parse_FromSpaceLine(par, &msg->envSender, &msg->envDate);

    // Parse headers (until & including empty line)
    //
    if (!Parse_Headers(par, msg, &msg->headers)) {
	Warn("Could not parse headers");
	Parser_MoveTo(par, savedPos);
	return false;
    }

    // Parse body
    //
    Parse_StringStart(par, &msg->body);

    // Find the end of the message
    //
    MoveToEndOfMessage(par, msg);

    Parse_StringEnd(par, msg->body);
    Parse_StringEnd(par, msg->data);

    *pMsg = msg;

    return true;
}

void Stream_WriteMessage(Stream *output, Message *msg)
{
    if (msg->envSender != NULL) {
	Stream_WriteString(output, &Str_FromSpace);
	Stream_WriteString(output, msg->envSender);
	Stream_WriteChar(output, ' ');
	Stream_WriteCTime(output, &msg->envDate);
	Stream_WriteNewline(output);
    }
    Stream_WriteHeaders(output, msg->headers);
    Stream_WriteNewline(output);
    Stream_WriteString(output, Message_Body(msg));
}

/**
 **  Mailbox Functions
 **/

void Mailbox_Free(Mailbox *mbox)
{
    Message_Free(mbox->root, true);
    String_Free(mbox->data);
    String_Free(mbox->name);
    String_Free(mbox->source);
    xfree(mbox);
}

String *Mailbox_Source(Mailbox *mbox)
{
    return mbox->source;
}

String *Mailbox_Name(Mailbox *mbox)
{
    if (mbox->name == NULL && mbox->source != NULL) {
	int pos = String_FindLastChar(mbox->source, '/', true);

	if (pos++ == -1)
	    pos = 0;

	mbox->name =
	    String_Sub(mbox->source, pos, String_Length(mbox->source));
    }

    return mbox->name;
}

int Mailbox_Count(Mailbox *mbox)
{
    return mbox->count;
}

bool Mailbox_IsDirty(Mailbox *mbox)
{
    return mbox->dirty;
}

Message *Mailbox_Root(Mailbox *mbox)
{
    return mbox->root;
}

bool Parse_Mailbox(Parser *par, Mailbox **pMbox)
{
    Mailbox *mbox = New(Mailbox);
    Message **pMsg = &mbox->root;

    if (gVerbose)
	Note("Parsing mailbox %s", String_CString(Mailbox_Name(mbox)));

    // Append at the end
    while (*pMsg != NULL)
	pMsg = &(*pMsg)->next;

    while (Parse_Message(par, mbox, pMsg)) {
	Parse_Newline(par, NULL);
	pMsg = &(*pMsg)->next;
    }

    if (!Parser_AtEnd(par))
	Warn("Unparsable garbage after last message (@%d):\n %s",
	     Parser_Position(par), String_QuotedCString(&par->rest, 72));

    *pMbox = mbox;

    return true;
}

Mailbox *Mailbox_Open(String *source)
{
    if (gVerbose)
	Note("Opening %s", String_CString(source));

    // We only support mbox files for now
    //
    Stream *input = Stream_Open(source, false);
    String *data;
    Mailbox *mbox;

    if (!Stream_ReadContents(input, &data)) {
	// Save the errno for our caller
	//
	int err = errno;
	Stream_Free(input, true);
	errno = err;
	return NULL;
    }

    Stream_Free(input, true);

    Parser parser;

    Parser_Set(&parser, data);
    Parse_Mailbox(&parser, &mbox);

    mbox->source = String_Clone(source);
    mbox->data = data;

    return mbox;
}
void Stream_WriteMailbox(Stream *output, Mailbox *mbox, bool sanitize)
{
    // Dovecot and C-Client based IMAP implementations store internal
    // IMAP information in an X-IMAP or X-IMAPbase header that must
    // be in the first message in the mailbox.  If we're deleting this
    // message, we need to move the value to an X-IMAPbase header in the
    // new first message.  (X-IMAP is only used when the first message
    // is a pseudo-message.)
    // 
    if (sanitize) {
	Message *first, *msg;
	String *imap;

	// Find first non-deleted message
	//
	for (first = Mailbox_Root(mbox); first != NULL; first = first->next) {
	    if (!Message_IsDeleted(first))
		break;
	}

	for (msg = Mailbox_Root(mbox); msg != NULL; msg = msg->next) {
	    imap = Header_Get(msg->headers, &Str_XIMAPBase);
	    if (imap == NULL)
		imap = Header_Get(msg->headers, &Str_XIMAP);
	    if (imap != NULL)
		break;
	}

	if (msg != NULL && msg != first) {
	    // Move X-IMAPBase header to first message
	    Header_Set(first->headers, &Str_XIMAPBase, String_Clone(imap));
	    Header_Delete(msg->headers, &Str_XIMAP, false);
	    Header_Delete(msg->headers, &Str_XIMAPBase, false);
	}
    }

    Message *msg;

    for (msg = Mailbox_Root(mbox); msg != NULL; msg = msg->next) {
	if (!Message_IsDeleted(msg)) {
	    Stream_WriteMessage(output, msg);
	    Stream_WriteNewline(output);
	}
    }
}

void Mailbox_Write(Mailbox *mbox, String *destination)
{
    if (gDryRun) {
	Note("Dry run specified -- not writing mailbox %s",
	     String_CString(Mailbox_Name(mbox)));
	return;
    }

    if (gVerbose) {
	if (String_IsEqual(Mailbox_Source(mbox), destination, false))
	    Note("Saving mailbox %s", String_CString(Mailbox_Name(mbox)));
	else
	    Note("Saving mailbox %s to %s",
		 String_CString(Mailbox_Name(mbox)),
		 String_CString(destination));
    }

    // Only mbox file destinations supported for now
    //
    String *file = destination;
    Stream *tmp = Stream_OpenTemp(file, true);

    Stream_WriteMailbox(tmp, mbox, true);
    Stream_Close(tmp);

    const char *cFile = String_CString(destination);

    if (gBackup) {
	int len = String_Length(file);
	char bakPath[len + 1 + 1];

	memcpy(bakPath, cFile, len);
	strcpy(&bakPath[len], "~");

	if (rename(cFile, bakPath) != 0) {
	    Error(EX_CANTCREAT, "Could not rename %s to %s: %s",
		  cFile, bakPath, strerror(errno));
	}
    }

    if (rename(String_CString(tmp->name), cFile) != 0) {
	Error(EX_CANTCREAT, "Could not rename %s to %s: %s",
	      String_CString(tmp->name), cFile, strerror(errno));
    }

    Stream_Free(tmp, false);
}

void Mailbox_Save(Mailbox *mbox, bool force)
{
    if (!Mailbox_IsDirty(mbox) && !force) {
	Note("Leaving mailbox %s unchanged",
	     String_CString(Mailbox_Name(mbox)));

    } else {
	Mailbox_Write(mbox, mbox->source);	
    }
}

/**
 **  Interaction Functions
 **/

// Note: Return pointer to static storage.  Will be overwritten
// on succeeding calls. Must not be freed or saved by caller.
//
bool User_AskLine(const char *prompt, const String **pLine, bool trim)
{
    static char buf[1024];
    String line = {NULL, 0, kString_Const};

    fprintf(stdout, "%s", prompt);
    if (fgets(buf, sizeof(buf), stdin) == NULL)
	return false;

    if (pLine != NULL) {
	int len = strlen(buf);
	if (len > 0 && buf[len-1] == '\n')
	    len--;
	String_Set(&line, buf, len);
	if (trim)
	    String_TrimSpaces(&line);
	*pLine = &line;
    }

    return true;
}

// Ask the user a question that requires a single character answer
// (lowercase only, please)
//
int User_AskChoice(const char *question, const char *choices, char def)
{
    int answer = EOF;

    do {
	const char *p;
	int ch;

	// Present the question and the options
	//
	fprintf(stdout, "%s [", question);
	for (p = choices; *p != '\0'; p++) {
	    putc((*p == def) ? toupper(*p) : *p, stdout);
	}
	fprintf(stdout, "] ");
    
	// Get an answer
	//
	while ((ch = getchar()) != EOF && ch == ' ');
	if (ch == EOF)
	    break;

	// Validate the answer
	//
	if (ch == '\n')
	    break;

	ch = tolower(ch);
	if (strchr(choices, ch) != NULL)
	    answer = ch;

	// Eat rest of line
	while (ch != '\n' && (ch = getchar()) != EOF);
	if (ch == EOF)
	    break;

    } while (answer == EOF);

    return answer != EOF ? answer : def;
}

bool User_AskYesOrNo(const char *question, bool def)
{
    return User_AskChoice(question, "yn", def ? 'y' : 'n') == 'y';
}

/*
**  Application Functions
*/

void WriteQuotedExcerpt(FILE *out, String *str, int pos, int lines,
			const char *prefix)
{
    const char *chars = String_Chars(str);
    int len = String_Length(str);
    const char *b, *e, *p;
    int counter;

    // Extract a context of four lines before & after
    // the "From " line.
    //
    counter = lines / 2;
    for (b = chars + pos; b > chars; b--) {
	if (*b == '\n' && counter-- == 0)
	    break;
    }

    counter = lines - lines / 2;
    for (e = chars + pos; e < chars + len; e++) {
	if (*e == '\n' && counter-- == 0)
	    break;
    }


    for (p = b; p < e; p++) {
	if (p == b || p[-1] == '\n')
	    fprintf(out, "%s", prefix);
	putc(*p, out);
    }
    if (e > b && p[-1] != '\n')
	putc('\n', out);
}


void CheckMailbox(Mailbox *mbox, bool repair)
{
    Message *msg;

    for (msg = Mailbox_Root(mbox); msg != NULL; msg = msg->next) {
	String *value;
	const String *source = NULL;
	int cllen;

	// Content-Length
	//
	value = Header_Get(msg->headers, &Str_ContentLength);
	cllen = String_ToInteger(value, -1);

	int bodyLength = Message_BodyLength(msg);

	if (cllen != bodyLength) {
	    if (repair) {
		// Got the Dovecot "From " bug?
		//
		if (msg->dovecotFromSpaceBug != kDFSB_None) {
		    // Yup, remove bogus headers from the body
		    Note("Repairing Dovecot 'From '-corrupted body of "
			 "message %s", String_CString(msg->tag));

		    RepairDovecotFromSpaceBugBody(msg);
		    // Repairing the message will change it's length
		    bodyLength = Message_BodyLength(msg);

		} else if (value == NULL) {
		    Note("Setting Content-Length: %d for message %s",
			 bodyLength, String_CString(msg->tag));
			 
		} else {
		    Note("Correcting Content-Length: from %s to %d for "
			 "message %s",
			 String_PrettyCString(value), bodyLength,
			 String_CString(msg->tag));
		}

		Header_Set(msg->headers, &Str_ContentLength,
			   String_PrintF("%d", bodyLength));

	    } else {
		if (value != NULL) {
		    Note("Content-Length: %s should be %d for message %s",
			 String_PrettyCString(value), bodyLength,
			 String_CString(msg->tag));
		} else if (gPicky) {
		    Note("Missing Content-Length: header in message %s",
			 String_CString(msg->tag));
		}
	    }
	}

	// Got From?
	//
	value = Header_Get(msg->headers, &Str_From);
	if (value == NULL) {
	    source = &Str_XFrom;
	    value = String_Clone(Header_Get(msg->headers, source));

	    if (value == NULL) {
		source = &Str_Sender;
		value = String_Clone(Header_Get(msg->headers, source));
	    }

	    if (value == NULL) {
		source = &Str_ReturnPath;
		value = String_Clone(Header_Get(msg->headers, source));
	    }

	    if (value == NULL) {
		source = &Str_EnvelopeSender;
		value = String_Clone(msg->envSender);
	    }

	    if (value == NULL) {
		Note("Missing From: header in message %s",
		     String_CString(msg->tag));

	    } else if (repair) {
		Note("Missing From: header in message %s, substituting\n"
		     " %s: %s",
		     String_CString(msg->tag),
		     String_CString(source), String_CString(value));
		Header_Set(msg->headers, &Str_From, value);
		value = NULL;

	    } else {
		Note("Missing From: header in message %s, but could use\n"
		     " %s: %s",
		     String_CString(msg->tag),
		     String_CString(source), String_CString(value));
	    }

	    String_Free(value);
	}

	// Got Date?
	//
	value = Header_Get(msg->headers, &Str_Date);
	if (value == NULL) {
	    // Look for "X-Date: <date>"
	    //
	    source = &Str_XDate;
	    value = String_Clone(Header_Get(msg->headers, source));

	    // Look for "Received: <junk>; <date>"
	    //
	    if (value == NULL) {
		source = &Str_Received;

		String *received = Header_GetLastValue(msg->headers, source);
		if (received != NULL) {
		    int pos = String_FindChar(received, ';', true);
		    if (pos != -1) {
			Parser tmp;
			Parser_Set(&tmp, received);
			Parser_Move(&tmp, pos + 1);
			Parse_Spaces(&tmp, NULL);
			Parse_UntilEnd(&tmp, &value);
		    }
		}
	    }

	    // Look for "From <sender> <cdate>"
	    //
	    if (value == NULL && msg->envSender != NULL) {
		source = &Str_EnvelopeDate;
		value = String_RFC822Date(&msg->envDate, false);
	    }

	    if (value == NULL) {
		Note("Missing Date: header in message %s",
		     String_CString(msg->tag));

	    } else if (repair) {
		Note("Missing Date: header in message %s, substituting\n"
		     " %s: %s",
		     String_CString(msg->tag), String_CString(source),
		     String_CString(value));
		Header_Set(msg->headers, &Str_Date, value);
		value = NULL;

	    } else {
		Note("Missing Date: header in message %s, but could use\n"
		     " %s: %s",
		     String_CString(msg->tag), String_CString(source),
		     String_CString(value));
	    }

	    String_Free(value);
	}

#if 0
	// "\n\nFrom " line in body?
	//
	String *body = Message_Body(msg);
	int pos = String_FindString(body, &Str_NL2FromSpace, true);
	if (pos != -1) {
	    Parser tmp;
	    String *line;

	    // Point to the letter F in "From "
	    pos += 2;

	    Parser_Set(&tmp, body);
	    Parser_Move(&tmp, pos);
	    Parse_StringStart(&tmp, &line);
	    if (Parse_FromSpaceLine(&tmp, NULL, NULL)) {
		// Backup over the newline
		Parser_Move(&tmp, -1);
		Parse_StringEnd(&tmp, line);

		Note("Found suspect \"From \" line in body of message %s:\n %s",
		     String_CString(msg->tag), String_QuotedCString(line, -1));

		if (gInteractive) {
		    fprintf(stdout, "Here is the context:\n");
		    WriteQuotedExcerpt(stdout, body, pos, 15, "| ");
		    if (User_AskYesOrNo("Split message into two?", false)) {
			Message *newMsg;

			Parser_MoveTo(&tmp, pos);
			if (Parse_Message(&tmp, msg->mbox, &newMsg)) {
			    // Shorten the old body and link in the new msg
			    String_SetLength(body, pos - 1);
			    newMsg->next = msg->next;
			    msg->next = newMsg;
			}

			Note("Created new message %s",
			     String_CString(newMsg->tag));

			Message_SetDirty(msg, true);
			Message_SetDirty(newMsg, true);
		    }
		}
	    }
	    String_Free(line);
	}
#endif
    }
}

void CheckContentLengths(Message *msg, bool repair)
{
    for (; msg != NULL; msg = msg->next) {
	String *value = Header_Get(msg->headers, &Str_ContentLength);
	int oldlen = String_ToInteger(value, -1);
	int bodlen = Message_BodyLength(msg);


	if (oldlen != bodlen) {
	    if (repair) {
		if (value != NULL)
		    Note("Changing Content-Length: header from %s to %d "
			 "for message %s",
			 String_PrettyCString(value), bodlen, 
			 String_CString(msg->tag));
		else
		    Note("Setting Content-Length: header to %d for message %s",
			 bodlen, String_CString(msg->tag));

		Header_Set(msg->headers, &Str_ContentLength, 
			   String_PrintF("%d", bodlen));

	    } else {
		if (gPicky)
		    Note("Missing Content-Length: header in message %s",
			 String_CString(msg->tag));
	    }
	}
    }
}

void RemoveContentLengths(Message *msg, bool escapeEscapedFromToo)
{
    Array *bodyParts = Array_New(0, (Free *) String_Free);

    for (; msg != NULL; msg = msg->next) {
	Header_Delete(msg->headers, &Str_ContentLength, true);

	Parser tmp;
	int lastPos = 0;
	String *body = Message_Body(msg);

	Parser_Set(&tmp, body);

	while (Parse_UntilString(&tmp, &Str_FromSpace, true, NULL)) {
	    char ch;

	    if (Parser_Move(&tmp, -1) &&
		Parse_Char(&tmp, &ch)) {

		int pos = Parser_Position(&tmp);

		if (escapeEscapedFromToo && ch == '>') {
		    // Look for "\n>*From"
		    const char *b, *bodyChars = String_Chars(body);

		    for (b = bodyChars + pos - 2; b > bodyChars && *b == '>';
			 b--);

		    ch = *b;
		}

		if (Char_IsNewline(ch)) {
		    // This "From " marker needs quoting
		    //
		    Array_Append(bodyParts, String_Sub(body, lastPos, pos));
		    lastPos = pos;
		}
	    }
	}

	if (Array_Count(bodyParts) > 0) {
	    String_Define(gtString, ">");

	    Parse_UntilEnd(&tmp, NULL);
	    Array_Append(bodyParts,
			 String_Sub(body, lastPos, Parser_Position(&tmp)));

	    String *newBody = Array_Join(bodyParts, &gtString);
	    String_Free(body);
	    Message_SetBody(msg, newBody);
	}
    }

    Array_Free(bodyParts);
}


Array *SplitMessages(Mailbox *mbox, Array *mary)
{
    Message *msg;

    if (mary == NULL)
	mary = Array_New(0, NULL);

    for (msg = Mailbox_Root(mbox); msg != NULL; msg = msg->next) {
	Array_Append(mary, msg);
    }

    return mary;
}

void ShowMessage(Message *msg)
{
    FILE *out = stdout;

    if (gPager != NULL)
	out = popen(String_CString(gPager), "w");

    Stream *stream = Stream_New(out, gPager, true);
    stream->ignoreErrors = true;

    Stream_WriteMessage(stream, msg);

    if (gPager != NULL)
	pclose(stream->file);
    Stream_Free(stream, false);
}

// "[Mon,]  1 Jan 2000 00:00:00 +0000 (GMT)" => " 1 Jan 00:00"
//
void PrintShortDate(Stream *output, String *rfc822Date)
{
    const char *chars = String_Chars(rfc822Date);
    int len = String_Length(rfc822Date);
    Parser tmp;
    int pos;
    String *day, *mon, *year, *time;

    Parser_Set(&tmp, rfc822Date);

    Parse_Spaces(&tmp, NULL);

    pos = Parser_Position(&tmp);
    if (pos + 4 < len && chars[pos+3] == ',') {
	// Skip weekday
	Parse_UntilSpace(&tmp, NULL);
	Parse_Spaces(&tmp, NULL);
    }

    // Should be pointing to the day now (which may be one or two digits)
    //
    Parse_UntilSpace(&tmp, &day);
    Parse_Spaces(&tmp, NULL);

    // Month
    //
    Parse_UntilSpace(&tmp, &mon);
    Parse_Spaces(&tmp, NULL);
    
    // Year
    //
    Parse_UntilSpace(&tmp, &year);
    Parse_Spaces(&tmp, NULL);
    
    // Time
    //
    Parse_UntilSpace(&tmp, &time);
    Parse_Spaces(&tmp, NULL);

    // OK,  let's do it!
    //
    Stream_PrintF(output, "%2.2s %-3.3s %-5.5s",
		  String_CString(String_Safe(day)),
		  String_CString(String_Safe(mon)),
		  String_CString(String_Safe(time)));

    String_Free(day);
    String_Free(mon);
    String_Free(year);
    String_Free(time);
}

int IntLength(int num)
{
    int digits;

    if (num == 0)
	return 1;

    for (digits = 0; num > 0; num /= 10, digits++);

    return digits;
}

void ListMessage(Stream *output, int num, int numWidth, Message *msg,
		 int previewLines)
{
    String *sizstr = String_ByteSize(String_Length(msg->data));

    Stream_PrintF(output, "%c%*d: ",
		  Message_IsDeleted(msg) ? 'D' : ' ', numWidth, num);
    PrintShortDate(output, Header_Get(msg->headers, &Str_Date));
    Stream_PrintF(output, "  %-20.20s",
		  String_CString(String_Safe(Header_Get(msg->headers, &Str_From))));
    Stream_PrintF(output, "  %-*.*s",
		  33 - numWidth, 33 - numWidth,
		  String_CString(String_Safe(Header_Get(msg->headers, &Str_Subject))));
    Stream_PrintF(output, " %6s\n", String_CString(String_Safe(sizstr)));

    String_Free(sizstr);

    Parser tmp;
    String *line;

    Parser_Set(&tmp, Message_Body(msg));
    while (previewLines-- > 0) {
	if (!Parse_UntilNewline(&tmp, &line))
	    break;
	Parse_Newline(&tmp, NULL);
	Stream_PrintF(output, " %*s  |%.*s\n",
		      numWidth, "",
		      gPageWidth - numWidth - 3, String_CString(line));
	String_Free(line);
    }
}

void ListMailbox(Stream *output, Mailbox *mbox, int pos, int count)
{
    // Adjust to even page boundary
    pos = (pos / count) * count;

    int i = 0;
    int digits = IntLength(pos + count + 1 - 1);
    Message *msg;

    for (msg = Mailbox_Root(mbox); msg != NULL && i < pos; msg = msg->next, i++);

    for (; msg != NULL && i < pos + count; msg = msg->next, i++) {
	ListMessage(output, i + 1, digits, msg, 0);
    }
}

int CompareMessageIDs(const void *a, const void *b)
{
    Message *ma = *(Message **) a;
    Message *mb = *(Message **) b;

    return String_Compare(String_Safe(ma->cachedID),
			  String_Safe(mb->cachedID), true);
}

void SortMessages(Array *mary)
{
    int i;

    if (gVerbose)
	Note("Sorting messages");

    // First cache message IDs
    for (i = 0; i < Array_Count(mary); i++) {
	Message *msg = Array_GetAt(mary, i);

	if (msg->cachedID == NULL)
	    msg->cachedID = Header_Get(msg->headers, &Str_MessageID);
    }
    
    qsort(Array_Items(mary), Array_Count(mary), sizeof(void *),
	  CompareMessageIDs);
}

int ChooseMessageToDelete(Message *a, Message *b)
{
    Stream_PrintF(gStdOut, "\nThese are the messages:\n");

    ListMessage(gStdOut, 1, 1, a, 4);
    ListMessage(gStdOut, 2, 1, b, 4);

    Stream_WriteNewline(gStdOut);

    switch (User_AskChoice("Please choose which message to delete "
			  "(or b(oth) or n(either)):", "12bn", 'n')) {
      case '1':
	Message_SetDeleted(a, true);
	return 1;

      case '2':
	Message_SetDeleted(b, true);
	return 1;

      case 'b':
	Message_SetDeleted(a, true);
	Message_SetDeleted(b, true);
	return 2;

      default:
	return 0;
    }
}

void UniqueMailbox(Mailbox *mbox)
{
    Array *mary = SplitMessages(mbox, NULL);
    int i, count = Array_Count(mary);
    int dups = 0;
    Message *m, *n;

    SortMessages(mary);

    m = Array_GetAt(mary, 0);
    for (i = 1; i < count; i++, m = n) {
	n = Array_GetAt(mary, i);

	if (!Message_IsDeleted(n) &&
	    m->cachedID != NULL && n->cachedID != NULL &&
	    String_IsEqual(m->cachedID, n->cachedID, true)) {
	    static String const *checkKeys[] =
		{&Str_From, &Str_To, &Str_Subject, &Str_Date, NULL};
	    const String **ss;
	    bool same = true;

	    // They've got the same Message-IDs, what about other
	    // salient details like the body and certain key headers.
	    //
	    for (ss = checkKeys; *ss != NULL; ss++) {
		if (!String_IsEqual(Header_Get(m->headers, *ss),
				    Header_Get(n->headers, *ss), true)) {
		    Note("Messages %s and %s have the same Message-ID\n"
			 " %s, but different %s lines",
			 String_CString(m->tag),
			 String_CString(n->tag),
			 String_PrettyCString(m->cachedID),
			 String_CString(*ss));
		    same = false;
		    break;
		}
	    }

	    if (same &&
		!String_IsEqual(Message_Body(m), Message_Body(n), true)) {
		Note("Messages %s and %s have the same Message-ID\n"
		     "%s, but different bodies",
		     String_CString(m->tag),
		     String_CString(n->tag),
		     String_PrettyCString(m->cachedID)); 
		same = false;
	    }

	    if (same) {
		Message_SetDeleted(n, true);
		dups++;

	    } else if (gInteractive) {
		dups += ChooseMessageToDelete(m, n);
	    }
	}
    }

    Note("%s %d duplicate%s",
	 dups == 0 ?"Found" : "Deleted",
	 dups, dups == 1 ? "" : "s");

    Array_Free(mary);
}

/**
 **  Command Processing
 **/

typedef enum {
    kCmd_None = 0,
    kCmd_Check,
    kCmd_Delete,
    kCmd_DeleteAndShowNext,
    kCmd_Exit,
    kCmd_Help,
    kCmd_List,
    kCmd_ListNext,
    kCmd_ListPrevious,
    kCmd_Repair,
    kCmd_Show,
    kCmd_ShowNext,
    kCmd_Unique,
    kCmd_Write,
    kCmd_WriteAndExit,
} Command;

typedef struct {
    const char *cname;
    const char *cargs;
    Command type;
    const char *cdesc;
    String *name;
} CommandTable;

// Command specification syntax:
//
// <cmd> ::= <name>['|'<name>]... [<arg>...] ['#' <description>]
// <arg> ::= 'm' | 'f'

CommandTable kCommandTable[] = {
    // Note: The order of the command table is significant.  If a prefix
    // matches multiple commands, the first one found in this table will
    // be selected.
    //
    {"check",	NULL,		kCmd_Check,
     "check the mailbox' internal consistency"},
    {"delete",	"[<msgs>]",	kCmd_Delete,
     "delete the given message(s)"},
    {"dp",	NULL,		kCmd_DeleteAndShowNext,
     "delete the given message(s), then show the next message"},
    {"exit",	NULL,		kCmd_WriteAndExit,
     "save any changes, then leave the mailbox"},
    {"headers",	"[<msg>]",	kCmd_List,
     "list a page full of message descriptions"},
    {"list",	"[<msg>]",	kCmd_List,
     "list a page full of message descriptions"},
    {"help",	"[<cmd>]",	kCmd_Help,
     "get help on a specific command or all commands (this text)"},
    {"more",	"[<msgs>]",	kCmd_Show,
     "display the contents of the given message(s)"},
    {"next",	NULL,		kCmd_ShowNext,
     "go to the next message and display it"},
    {"+",	NULL,		kCmd_ShowNext,
     "go to the next message and display it"},
    {"print",	"[<msgs>]",	kCmd_Show,
     "display the contents of the given message(s)"},
    {"quit",	NULL,		kCmd_Exit,
     "leave the mailbox without saving any changes"},
    {"repair",	NULL,		kCmd_Repair,
     "check the mailbox' internal consistency and repair if needed"},
    {"save",	"[<file>]",	kCmd_Write,
     "save the mailbox back to its own file or the given file"},
    {"unique",	NULL,		kCmd_Unique,
     "unique the messages in the mailbox by removing duplicates"},
    {"write",	"[<file>]",	kCmd_Write,
     "write the mailbox back to its own file or the given file"},
    {"xit",	NULL,		kCmd_Exit,
     "leave the mailbox without saving any changes"},
    {"z",	NULL,		kCmd_ListNext,
     "show the next page of message descriptions"},
    {"z-",	NULL,		kCmd_ListPrevious,
     "show the previous page of message descriptions"},
    {"?",	NULL,		kCmd_Help,
     "get help on a specific command or all commands (this text)"},
    {NULL}
};

String *NextCmdArg(int *pIndex, Array *args, bool required)
{
    if (*pIndex < Array_Count(args))
	return Array_GetAt(args, (*pIndex)++);
    if (required)
	Error(EX_OK, "Missing argument");
    return NULL;
}

int NextNumArg(int *pIndex, Array *args, bool required)
{
    String *arg = NextCmdArg(pIndex, args, required);
    int num = -1;

    if (arg != NULL) {
	num = String_ToInteger(arg, -1);
	if (num == -1)
	    Error(EX_OK, "Invalid message number");
    }

    return num;
}

Message *GetMessageByNumber(Mailbox *mbox, int cur)
{
    Message *msg;
    int i;

    for (msg = Mailbox_Root(mbox), i = 0; msg != NULL && i < cur;
	 msg = msg->next, i++);

    if (msg == NULL)
	Error(EX_OK, "Message %d does not exist", cur + 1);

    return msg;
}

void InitCommandTable(CommandTable *table)
{
    CommandTable *ct;

    // Only do this once
    if (table->name != NULL)
	return;

    for (ct = table; ct->cname != NULL; ct++) {
	ct->name = String_FromCString(ct->cname, false);
    }
}

void ShowHelp(Stream *output, String *cmd, CommandTable *ctable)
{
    CommandTable *ct;

    if (cmd == NULL) {
	// Non-specific help
	//
	int pos = 1;

	Stream_PrintF(output, " Please enter one of the following commands:\n"
		      "   ");
	for (ct = kCommandTable; ct->name != NULL; ct++) {
	    if (ct != kCommandTable) {
		Stream_PrintF(output, ", ");
		pos += 2;
	    }
	    if (pos + String_Length(ct->name) >= gPageWidth) {
		Stream_PrintF(output, "\n   ");
		pos = 3;
	    }
	    Stream_WriteString(output, ct->name);
	    pos += String_Length(ct->name);
	}
	Stream_PrintF(output, "\n Enter \"help <cmd>\" for more "
		      "information about a specific command or\n "
		      "\"help all\" for all commands.\n");

    } else {
	bool isAll = String_IsEqual(cmd, &Str_All, false);
	int leftWidth = 0;

	for (ct = kCommandTable; ct->name != NULL; ct++) {
	    int width = 1 + String_Length(ct->name) + 1 + 1;
	    if (ct->cargs != NULL)
		width += strlen(ct->cargs);
	    if (leftWidth < width)
		leftWidth = width;
	}

	for (ct = kCommandTable; ct->name != NULL; ct++) {
	    if (isAll || String_IsEqual(cmd, ct->name, false)) {
	    int width = 1 + String_Length(ct->name) + 1 + 1;
	    if (ct->cargs != NULL)
		width += strlen(ct->cargs);

		Stream_PrintF(output, " %s %s %*s-- %s\n",
			      ct->cname, ct->cargs != NULL ? ct->cargs : "",
			      leftWidth - width, "",
			      ct->cdesc);
	    }
	}
    }
}

void RunLoop(Mailbox *mbox, Array *commands)
{
    int cmdCount = Array_Count(commands);
    const String *cmdLine;
    int cur = 0;
    int ci = 0;
    bool done = false;

    InitCommandTable(kCommandTable);

    while (!done) {
	if (ci < cmdCount) {
	    cmdLine = Array_GetAt(commands, ci++);
	} else if (cmdCount > 0 || !User_AskLine("> ", &cmdLine, true)) {
	    break;
	}

	Array *args = String_Split(cmdLine, ' ', true);

	// See what we got... if anything
	//
	if (Array_Count(args) == 0) {
	    Array_Free(args);
	    continue;
	}

	// Find matching command
	//
	int argi = 0;
	String *arg = Array_GetAt(args, argi++);
	CommandTable *ct;
	Command cmd = kCmd_None;
	Message *msg;
	int num;

	for (ct = kCommandTable; ct->name != NULL; ct++) {
	    if (String_HasPrefix(ct->name, arg, false)) {
		cmd = ct->type;
		break;
	    }
	}

	// If we didn't find it, assume kCmd_Show if the
	// first arg is numeric.
	//
	if (cmd == kCmd_None) {
	    num = String_ToInteger(arg, -1);
	    if (num > 0) {
		cmd = kCmd_Show;
		argi--;
	    }
	}

	switch (cmd) {
	  case kCmd_Show:
	    num = NextNumArg(&argi, args, true);
	    if (num == -1)
		break;
	    cur = iMin(iMax(num - 1, 0), Mailbox_Count(mbox));
	    msg = GetMessageByNumber(mbox, cur);
	    if (msg != NULL)
		ShowMessage(msg);
	    break;

	  case kCmd_ShowNext:
	    if (cur + 1 == Mailbox_Count(mbox)) {
		Error(EX_OK, "No more messages");
		break;
	    }
	    cur++;
	    msg = GetMessageByNumber(mbox, cur);
	    if (msg != NULL)
		ShowMessage(msg);
	    break;

	  case kCmd_Delete:
	    num = NextNumArg(&argi, args, false);
	    // XXX: Should check for invalid arg too
	    if (num != -1)
		cur = iMin(iMax(num - 1, 0), Mailbox_Count(mbox));
	    msg = GetMessageByNumber(mbox, cur);
	    if (msg != NULL)
		Message_SetDeleted(msg, true);
	    break;

	  case kCmd_DeleteAndShowNext:
	    num = NextNumArg(&argi, args, false);
	    // XXX: Should check for invalid arg too
	    if (num != -1)
		cur = iMin(iMax(num - 1, 0), Mailbox_Count(mbox));
	    msg = GetMessageByNumber(mbox, cur);
	    if (msg != NULL) {
		Message_SetDeleted(msg, true);
		ShowMessage(msg);
	    }
	    break;

	  case kCmd_List:
	    ListMailbox(gStdOut, mbox, cur, gPageHeight - 1);
	    break;

	  case kCmd_ListNext:
	    cur = iMin(cur + (gPageHeight - 1), Mailbox_Count(mbox));
	    ListMailbox(gStdOut, mbox, cur, gPageHeight - 1);
	    break;

	  case kCmd_ListPrevious:
	    cur = iMax(cur - (gPageHeight - 1), 0);
	    ListMailbox(gStdOut, mbox, cur, gPageHeight - 1);
	    break;

	  case kCmd_Check:
	    CheckMailbox(mbox, false);
	    break;

	  case kCmd_Repair:
	    CheckMailbox(mbox, true);
	    break;

	  case kCmd_Unique:
	    UniqueMailbox(mbox);
	    break;

	  case kCmd_Write:
	    arg = NextCmdArg(&argi, args, false);
	    if (arg == NULL)
		Mailbox_Save(mbox, false);
	    else
		Mailbox_Write(mbox, arg);
	    break;

	  case kCmd_Exit:
	    if (!Mailbox_IsDirty(mbox) ||
		User_AskYesOrNo("Mailbox has not been saved, quit anyway?",
				false))
		return;
	    break;

	  case kCmd_WriteAndExit:
	    done = true;
	    break;

	  case kCmd_Help:
	    ShowHelp(gStdOut, NextCmdArg(&argi, args, false), kCommandTable);
	    break;
	    
	  case kCmd_None:
	    Error(EX_OK, "Unknown command: %s", String_CString(arg));
	    break;
	}
    }

    // Autosave if needed
    //
    if (mbox->dirty)
	Mailbox_Save(mbox, false);
}

void ProcessFile(String *file, Array *commands, Stream *output)
{
    Mailbox *mbox = Mailbox_Open(file);

    if (mbox == NULL)
	Error(EX_NOINPUT, "Could not read %s: %s",
	      String_CString(file), strerror(errno));

    if (!gQuiet) {
	int count = Mailbox_Count(mbox);
	String *sizstr = String_ByteSize(String_Length(mbox->data));

	Note("%s: %d message%s, %s",
	     String_CString(file),
	     count, count == 1 ? "" : "s", String_CString(sizstr));

	String_Free(sizstr);
    }

    RunLoop(mbox, commands);

    if (output != NULL)
	Stream_WriteMailbox(output, mbox, true);

    Mailbox_Free(mbox);
    String_Free(file);
}

void Usage(const char *pname, bool help)
{
    fprintf(stderr, "Usage: %s [-acdfhinopqruvN] [<file> ...]\n", pname);

    if (help) {
	fprintf(stderr, "The options include:\n"
		"  -b \t\tmove mbox to mbox~ before changing it\n"
		"  -c \t\tcheck the mbox for consistency\n"
		"  -d \t\tdebug mode (see source code)\n"
		"  -f <file> \tprocess mbox <file>\n"
		"  -h \t\tprint out this help text\n"
		"  -i \t\tforce interactive mode\n"
		"  -n \t\tdry run -- no changes will be made to the mailboxes\n"
		"  -o <file> \tconcatenate messages into <file>\n"
		"  -p \t\tbe picky and report more indiscretions than otherwise\n"
		"  -q \t\tbe quiet and don't report warnings or notices\n"
		"  -r \t\trepair the given mailboxes\n"
		"  -u \t\tunique messages in each mailbox by removing duplicates\n"
		"  -v \t\tbe verbose and print out more progress information\n"
		"  -N \t\tdon't try to mmap the mbox file\n"
		);
    } else {
	fprintf(stderr, "(run: \"%s -h\" for more information)\n", pname);
    }

    exit(EX_USAGE);
}

void Help(const char *pname)
{
    
}

String *NextMainArg(int *pAC, int argc, char **argv)
{
    return *pAC < argc ? String_FromCString(argv[++*pAC], false) : NULL;
}

int main(int argc, char **argv)
{
    String *outFile = NULL;
    Stream *output = NULL;
    Array *commands = Array_New(0, (Free *) String_Free);
    Array *files = Array_New(0, (Free *) String_Free);
    int ac;

    gInteractive = isatty(0);
    gPager = String_FromCString(getenv("PAGER"), false);
    gStdOut = Stream_Open(NULL, true);

    // We don't care about broken (pager) pipes
    signal(SIGPIPE, SIG_IGN);

    for (ac = 1; ac < argc && argv[ac][0] == '-'; ac++) {
	if (argv[ac][1] == '-') {
	    const char *opt = &argv[ac][2];

#ifdef DEBUG
	    if (strcmp(opt, "debug") == 0) {
		gDebug = true;

	    } else
#endif
	    if (strcmp(opt, "nomap") == 0) {
		gMap = false;
	    } else if (strcmp(opt, "verbose") == 0) {
		gVerbose = true;
	    } else {
		//Usage(argv[0]);
		Array_Append(commands, String_FromCString(opt, false));
	    }

	} else {
	    const char *opt;

	    for (opt = &argv[ac][1]; *opt != '\0'; opt++) {
		switch (*opt) {
		  case 'b': gBackup = true; break;
		  case 'c': Array_Append(commands, &Str_Check); break;
#ifdef DEBUG
		  case 'd': gDebug = true; break;
#endif
		  case 'f':
		    Array_Append(files, NextMainArg(&ac, argc, argv)); break;
		  case 'h': Usage(argv[0], true); break;
		  case 'i': gInteractive = true; break;
		    //case 'l': gAddContentLength = true; break;
		  case 'n': gDryRun = true; break;
		  case 'o': outFile = NextMainArg(&ac, argc, argv); break;
		  case 'p': gPicky = true; break;
		  case 'q': gQuiet = true; break;
		  case 'r': Array_Append(commands, &Str_Repair); break;
		  case 'u': Array_Append(commands, &Str_Unique); break;
		  case 'v': gVerbose = true; break;
		  case 'w': gAutoWrite= true; break;
		  case 'N': gMap = false; break;
		  default:
		    Usage(argv[0], false);
		}
	    }
	}

    }

    if (outFile != NULL && !gDryRun) {
	output = Stream_Open(outFile, true);
    }

    for (; ac < argc; ac++) {
	Array_Append(files, String_FromCString(argv[ac], false));
    }

    if (gDryRun) {
	Note("Running in dry run mode -- no changes will be made");
    }

    if (Array_Count(files) == 0) {
	const char *cMail = getenv("MAIL");
	String *mailFile;

	// XXX: FIXME!
	if (cMail != NULL) {
	    mailFile = String_FromCString(cMail, false);
	} else {
	    mailFile = String_PrintF("/var/mail/%s", getenv("LOGNAME"));
	}

	Array_Append(files, mailFile);
    }

    int i;

    for (i = 0; i < Array_Count(files); i++) {
	ProcessFile(Array_GetAt(files, i), commands, output);
    }

    if (output != NULL)
	Stream_Free(output, true);

    if (gWarnings == 1)
	Note("1 warning was issued", gWarnings);
    else if (gWarnings > 0)
	Note("%d warnings were issued", gWarnings);

    return EX_OK;
}

#ifdef DEBUG
const char *s(String *str)
{
    static char *buf = NULL;
    int len = String_Length(str);

    buf = xalloc(buf, len + 1);
    memcpy(buf, String_Chars(str), len);
    buf[len] = '\0';

    return buf;
}
#endif