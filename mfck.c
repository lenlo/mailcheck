/*
**  mfck -- A mailbox checking tool (and more!)
**
**  Copyright (c) 2008-2019 by Lennart Lovstrand <mfck@lenlolabs.com>
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
#include <setjmp.h>
#include <sys/types.h>
#include <dirent.h>
#include <limits.h>
#include <sys/ioctl.h>

#ifdef USE_READLINE
#  include <readline/readline.h>
#  include <readline/history.h>
#endif

#include "md5.h"

#ifdef USE_GC
#  ifdef DEBUG
#    define GC_DEBUG
#  endif
#  include <gc.h>
#  define malloc				GC_MALLOC
#  define realloc				GC_REALLOC
#  define free					GC_FREE
#endif

#include "vers.h"

#define OPT_FUZZY_NEWLINE
#define OPT_LOCK_FILE

#define kCheck_MaxWarnCount			5
#define kContext_LineCount			2 // before & after

#define kArray_InitialSize			32
#define kArray_GrowthFactor			1.4

#define kRead_InitialSize			(64*1024)
#define kRead_GrowthFactor			1.5

#define kString_CStringPoolSize			10
#define kString_MaxPrintFLength			1024
#define kString_MaxPrettyLength			32

#define kDefaultPageWidth			80
#define kDefaultPageHeight			24

#define kDefaultEditor				"ed"
#define kDefaultPager				"cat"

#define kDefaultLockTimeout			5	// sec

#define kString_ExcerptLength			50

#define kSyntheticMessageIDSuffix		"@synthesized-by-mfck"

#define String_Define(NAM, CSTR)			\
    const String NAM = {CSTR, sizeof(CSTR)-1, kString_Const};

typedef enum {false = 0, true = !false} bool;

typedef enum {kString_Shared = 0, kString_Alloced, kString_Mapped,
	      kString_Const} StringType;

#define kString_NotFound			-1

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
    kDFSB_XUIDKeys	= 0x01,
    kDFSB_ContLen	= 0x02,
    kDFSB_Status	= 0x04,
    kDFSB_Newline	= 0x08,
} DovecotFromSpaceBugType;

typedef struct _Message {
    int num;
    struct _Mailbox *mbox;
    String *tag;
    String *data;
    String *envelope;
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
    bool deleteFileWhenFreed;
} Stream;

#define New(T)			((T *) calloc(1, sizeof(T)))

/*
**  String Constants
*/

// General Strings
String_Define(Str_Empty, "");
String_Define(Str_Newline, "\n");
String_Define(Str_Space, " ");
String_Define(Str_TwoDashes, "--");

// Header Keys
String_Define(Str_Bcc, "bcc");
String_Define(Str_Cc, "cc");
String_Define(Str_ContentLength, "Content-Length");
String_Define(Str_ContentTransferEncoding, "Content-Transfer-Encoding");
String_Define(Str_ContentType, "Content-Type");
String_Define(Str_Date, "Date");
String_Define(Str_From, "From");
String_Define(Str_FromSpace, "From ");
String_Define(Str_GTFromSpace, ">From ");
String_Define(Str_MessageID, "Message-ID");
String_Define(Str_Received, "Received");
String_Define(Str_ResentBcc, "Resent-bcc");
String_Define(Str_ResentCc, "Resent-cc");
String_Define(Str_ResentDate, "Resent-Date");
String_Define(Str_ResentFrom, "Resent-From");
String_Define(Str_ResentMessageID, "Resent-Message-ID");
String_Define(Str_ResentSender, "Resent-Sender");
String_Define(Str_ResentSubject, "Resent-Subject");
String_Define(Str_ResentTo, "Resent-To");
String_Define(Str_ReturnPath, "Return-Path");
String_Define(Str_Sender, "Sender");
String_Define(Str_Status, "Status");
String_Define(Str_Subject, "Subject");
String_Define(Str_To, "To");
String_Define(Str_Xcc, "X-cc");
String_Define(Str_XDate, "X-Date");
String_Define(Str_XFrom, "X-From");
String_Define(Str_XIMAP, "X-IMAP");
String_Define(Str_XIMAPBase, "X-IMAPBase");
String_Define(Str_XKeywords, "X-Keywords");
String_Define(Str_XMessageID, "X-Message-ID");
String_Define(Str_XSubject, "X-Subject");
String_Define(Str_XTo, "X-To");
String_Define(Str_XUID, "X-UID");

String_Define(Str_Body, "Body");

// Content-Transfer-Encodings
String_Define(Str_Binary, "binary");
String_Define(Str_8Bit, "8bit");

// Content-Types (and parameters)
String_Define(Str_Multipart, "multipart");
String_Define(Str_Boundary, "boundary");

// Other Strings
String_Define(Str_All, "all");
String_Define(Str_Check, "check");
String_Define(Str_List, "list");
String_Define(Str_Repair, "repair");
String_Define(Str_Unique, "unique");

String_Define(Str_EnvelopeDate, "envelope date");
String_Define(Str_EnvelopeSender, "envelope sender");

String_Define(Str_Plus, "+");
String_Define(Str_Minus, "-");
String_Define(Str_Colon, ":");
String_Define(Str_Dollar, "$");

String_Define(Str_True, "true");
String_Define(Str_Strict, "strict");

String_Define(Str_DotLock, ".lock");

/*
**  Global Variables
*/

bool gAutoWrite = false;
bool gBackup = false;
bool gCheck = false;
#ifdef DEBUG
bool gDebug = false;
#endif
bool gDryRun = false;
bool gInteractive = false;
bool gMap = true;
bool gShowContext = false;
bool gStrict = false;
bool gQuiet = false;
bool gUnique = false;
bool gVerbose = false;
//bool gWantContentLength = false;

int gWarnings = 0;
int gMessageCounter = 0;
String *gPager = NULL;
Stream *gStdOut;
int gPageWidth = kDefaultPageWidth;
int gPageHeight = kDefaultPageHeight;

jmp_buf *gInterruptReentry = NULL;
FILE *gOpenPipe = NULL;

const char gVersion[] = "mfck version 1.0";
const char gCopyright[] =
    "Copyright (c) 2008-2017, Lennart Lovstrand <mfck@lenlolabs.com>";

/*
**  Forward Declarations
*/

void Exit(int ret);

/*
**  Math Functions
*/

static inline int iMin(int a, int b)	{return a < b ? a : b;}
static inline int iMax(int a, int b)	{return a > b ? a : b;}

/*
**  Char Functions
*/

static inline bool Char_IsNewline(int ch) {return ch == '\r' || ch == '\n';}

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

// Scale the size to the appropriate K-based unit
//
double NormalizeSize(size_t size, char *pSuffix)
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

    return fsize;
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

void WarnV(const char *fmt, va_list args)
{
    if (!gQuiet) {
	fprintf(stdout, "%%");
	vfprintf(stdout, fmt, args);
	fprintf(stdout, "\n");
    }

    gWarnings++;
}

void Warn(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    WarnV(fmt, args);
    va_end(args);
}

void Error(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    fprintf(stderr, "?");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

void Fatal(int err, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    fprintf(stderr, "?Fatal Error: ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);

    if (err != EX_OK)
	Exit(err);
}

/* Show n lines of context around the given position.
 */
void ShowContext(const char *text, int length, int pos)
{
    int b, e, i, count;

    for (b = pos, count = kContext_LineCount + 1; b > 0 && count > 0; b--) {
	if (Char_IsNewline(text[b]))
	    count--;
    }
    // Point to just after the newline
    if (count == 0)
	b += 2;

    for (e = pos, count = kContext_LineCount; e < length && count > 0; e++) {
	if (Char_IsNewline(text[e]))
	    count--;
    }
    // Point to just after the newline
    if (count == 0 && e < length && Char_IsNewline(text[e]))
	e++;

    for (i = b; i < e; i++) {
	if (i == b || text[i-1] == '\n')
	    fputs("] ", stderr);
	/*
	if (i == pos)
	    fputs("<here>", stderr);
	*/
	putc(text[i], stderr);
    }
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
	Fatal(EX_UNAVAILABLE, "Out of memory when trying to allocate %u bytes",
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

static inline const char *String_Chars(const String *str)
{
    return str == NULL ? NULL : str->buf;
}

static inline int String_Length(const String *str)
{
    return str == NULL ? 0 : str->len;
}

#ifdef NOT_CURRENTLY_USED
static inline const char *String_End(const String *str)
{
    return str == NULL ? NULL : str->buf + str->len;
}
#endif

static inline const String *String_Safe(String *str)
{
    return str != NULL ? str : &Str_Empty;
}

static inline void String_SetChars(String *str, const char *buf)
{
    str->buf = buf;
}

static inline void String_SetLength(String *str, int len)
{
    str->len = len;
    if (str->type == kString_Alloced)
	((char *) str->buf)[len] = '\0';
}

static inline void String_Set(String *str, const char *buf, int len)
{
    str->buf = buf;
    str->len = len;
}

static inline String *String_New(StringType type, const char *chars, int length)
{
    String *str = New(String);
    String_Set(str, chars, length);
    str->type = type;
    return str;
}

static inline int String_CharAt(const String *str, int pos)
{
    if (pos < 0 || pos >= String_Length(str))
	return EOF;
    return String_Chars(str)[pos];
}

static inline String *String_Sub(const String *str, int start, int end)
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
    char *buf = xalloc(NULL, length + 1);
    buf[length] = '\0';
    return String_New(kString_Alloced, buf, length);
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

String *String_Append(const String *sub, ...)
{
    const String *s;
    va_list args;
    int len = 0;

    va_start(args, sub);
    for (s = sub; s != NULL; s = va_arg(args, const String *)) {
	len += String_Length(s);
    }
    va_end(args);

    String *res = String_Alloc(len);
    char *p = (char *) String_Chars(res);

    va_start(args, sub);
    for (s = sub; s != NULL; s = va_arg(args, String *)) {
	int sublen = String_Length(s);
	memcpy(p, String_Chars(s), sublen);
	p += sublen;
    }
    va_end(args);

    return res;
}

// Change the string by adjusting its start and length
//
static inline void String_Adjust(String *str, int offset)
{
    str->buf += offset;
    str->len -= offset;
}

static inline bool String_IsEmpty(const String *str)
{
    return String_Length(str) == 0;
}

static inline bool String_IsEqual(const String *a, const String *b,
				  bool sameCase)
{
    if (String_Length(a) != String_Length(b))
	return false;

    return sameCase ?
	strncmp(String_Chars(a), String_Chars(b), String_Length(b)) == 0 :
	strncasecmp(String_Chars(a), String_Chars(b), String_Length(b)) == 0;
}

static inline bool String_HasPrefix(const String *str, const String *sub,
				    bool sameCase)
{
    int stlen = String_Length(str);
    int sulen = String_Length(sub);

    if (stlen < sulen)
	return false;

    return (sameCase ?
	    strncmp(String_Chars(str), String_Chars(sub), sulen) :
	    strncasecmp(String_Chars(str), String_Chars(sub), sulen)) == 0;
}

static inline bool String_HasSuffix(const String *str, const String *sub,
				    bool sameCase)
{
    int stlen = String_Length(str);
    int sulen = String_Length(sub);

    if (stlen < sulen)
	return false;

    return (sameCase ?
	    strncmp(String_Chars(str) + stlen - sulen, String_Chars(sub),
		    sulen) :
	    strncasecmp(String_Chars(str) + stlen - sulen, String_Chars(sub),
			sulen)) == 0;
}

static inline int String_Compare(const String *a, const String *b,
				 bool sameCase)
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

#ifdef NOT_CURRENTLY_USED
static inline int String_CompareCI(const String *a, const String *b)
{
    return String_Compare(a, b, false);
}

static inline int String_CompareCS(const String *a, const String *b)
{
    return String_Compare(a, b, true);
}
#endif

int _String_FindTwoChars(const String *str, char ach, char bch)
{
    const char *p = String_Chars(str);
    const char *end = p + String_Length(str);

    while (p < end) {
	if (*p == ach || *p == bch)
	    return p - String_Chars(str);
	p++;
    }

    return kString_NotFound;
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

    return kString_NotFound;
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
	return kString_NotFound;

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

    return kString_NotFound;
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

    while ((pos = String_FindChar(&tmp, firstChar, sameCase)) !=
	   kString_NotFound) {
	String_Adjust(&tmp, pos);
	offset += pos;

	if (String_HasPrefix(&tmp, sub, sameCase))
	    return offset;

	// Skip past found char
	offset++;
	String_Adjust(&tmp, 1);
    }

    return kString_NotFound;
}

bool String_FoundString(const String *str, const String *sub, bool sameCase)
{
    return String_FindString(str, sub, sameCase) != kString_NotFound;
}

static inline int String_FindNewline(const String *str)
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

    // Some strings are guaranteed to already be null terminated
    if (str->type == kString_Alloced || str->type == kString_Const)
	return str->buf;

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
    char *dup = xalloc(NULL, len + 1);
    memcpy(dup, buf, len);
    dup[len] = '\0';

    return String_New(kString_Alloced, dup, len);
}

String *String_ByteSize(size_t size)
{
    char suffix;
    double fsize = NormalizeSize(size, &suffix);

    if (fsize == 0)
	return String_PrintF("%.1f%cB", fsize, suffix);
    else if (fsize < 10)
	return String_PrintF("%.1f%cB", fsize + 0.09, suffix);
    else
	return String_PrintF("%.0f%cB", fsize + 0.9, suffix);
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

static inline void _Array_CheckBounds(const Array *array, int ix)
{
    if (ix < 0 || ix >= Array_Count(array)) {
	Fatal(EX_UNAVAILABLE, "Out of bounds reference to array %#lx at %d",
	      (unsigned long) array, ix);
    }
}

static inline void *Array_GetAt(const Array *array, int ix)
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

void Parser_ShowContext(Parser *par)
{
    ShowContext(par->start,
		String_Chars(&par->rest) - par->start +
		String_Length(&par->rest),
		Parser_Position(par));
}

void Parser_Warn(Parser *par, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    WarnV(fmt, args);
    va_end(args);
    
    if (gShowContext)
	Parser_ShowContext(par);
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

bool Parse_BackupNewline(Parser *par)
{
    const char *b = String_Chars(&par->rest);
    const char *p = b;

    if (p > par->start && p[-1] == '\n')
	p--;
    if (p > par->start && p[-1] == '\r')
	p--;

    Parser_Move(par, p - b);

    return p < b;
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

    if (pos == kString_NotFound) {
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

    if (pos == kString_NotFound) {
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

    if (pos == kString_NotFound) {
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

int String_ToInteger(const String *str, int defValue)
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

Stream *Stream_New(FILE *file, const String *name, bool cloneName)
{
    Stream *stream = New(Stream);

    stream->file = file;
    stream->name = cloneName ? String_Clone(name) : (String *) name;

    return stream;
}

static Stream *StreamOpenWith(Stream *stream, const String *path,
			      bool write, bool fail)
{
    FILE *file;

    if (path == NULL) {
	file = write ? stdout : stdin;
	path = String_FromCString(write ? "(stdout)" : "(stdin)", false);

    } else {
	const char *cPath = String_CString(path);
	file = fopen(cPath, write ? "w" : "r");

	if (file == NULL) {
	    if (fail) 
		Fatal(write ? EX_CANTCREAT : EX_NOINPUT,
		      "Can't open file %s: %s", cPath, strerror(errno));
	    else
		return NULL;
	}

	path = String_Clone(path);
    }

    if (stream == NULL)
	return Stream_New(file, path, false);

    stream->file = file;
    if (stream->name != path) {
	String_Free(stream->name);
	stream->name = String_Clone(path);
    }

    return stream;
}

Stream *Stream_Open(const String *path, bool write, bool fail)
{
    return StreamOpenWith(NULL, path, write, fail);
}

Stream *Stream_Reopen(Stream *stream, bool write, bool fail)
{
    return StreamOpenWith(stream, stream->name, write, fail);
}

Stream *Stream_OpenTemp(const String *path, bool write, bool fail)
{
    int len = String_Length(path);
    char template[len + 1 + 6 + 1];

    memcpy(template, String_Chars(path), len);
    strcpy(template + len, "-XXXXXX");

    int fd = mkstemp(template);
    if (fd == -1) {
	if (fail)
	    Fatal(EX_CANTCREAT, "Can't create temporary file %s", template);
	else
	    return NULL;
    }

    Stream *stream = Stream_New(fdopen(fd, write ? "w" : "r"),
				String_FromCString(template, true), false);
    stream->deleteFileWhenFreed = true;
    return stream;
}

void Stream_Close(Stream *stream)
{
    if (stream->file != NULL && fclose(stream->file) != 0) {
	Fatal(EX_IOERR, "%s: %s", strerror(errno),
	      String_CString(stream->name));
    }

    stream->file = NULL;
}

void Stream_Free(Stream *stream, bool close)
{
    if (close)
	Stream_Close(stream);
    if (stream->deleteFileWhenFreed) {
	const char *cPath = String_CString(stream->name);
	(void) unlink(cPath);
    }
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

    // Try mapping the file if it's reasonably large
    //
    if (gMap && size >= 8192) {
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
	data = xalloc(data, size + 1);
	data[size] = '\0';

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
	Fatal(EX_IOERR, "Could not write 1 byte to %s: %s",
	      String_CString(output->name), strerror(errno));
}

void Stream_WriteChars(Stream *output, const char *chars, int len)
{
    if (len > 0 && fwrite(chars, len, 1, output->file) != 1 &&
	!output->ignoreErrors)
	Fatal(EX_IOERR, "Could not write %d byte%s to %s: %s",
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

String *Array_JoinTail(Array *array, const String *delim, int index)
{
    int i, count = Array_Count(array);
    int delimLen = String_Length(delim);
    int newLength = (count - index - 1) * delimLen;

    if (count <= index)
	return NULL;

    for (i = index; i < count; i++) {
	String *str = Array_GetAt(array, i);
	newLength += String_Length(str);
    }

    String *newString = String_Alloc(newLength);
    char *newChars = (char *) String_Chars(newString);

    for (i = index; i < count; i++) {
	String *str = Array_GetAt(array, i);
	int len = String_Length(str);

	if (i > index && delimLen > 0) {
	    memcpy(newChars, String_Chars(delim), delimLen);
	    newChars += delimLen;
	}
	memcpy(newChars, String_Chars(str), len);
	newChars += len;
    }

    return newString;
}

String *Array_Join(Array *array, const String *delim)
{
    return Array_JoinTail(array, delim, 0);
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

// Returns -1 if none was found
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

// Note: This is not a stringent ctime parser since some mail
// systems leave out the seconds field and/or add a timezone.
//
static bool ParseCTimeHelper(Parser *par, struct tm *pTime)
{
    int wday, year, mon, day, hour, min, sec = 0;
    bool gotZone = false;

    // Parse "www mmm dd hh:mm[:ss] [zone] yyyy [zone]"
    //  e.g. "Mon Apr  1 12:34:56 2008"
    //       "Wed May 15 11:37 PDT 1996"

    wday = ParseKeyword(par, kWeekdays);

    if (!Parse_ConstChar(par, ' ', true, NULL))
	return false;

    mon = ParseKeyword(par, kMonths);

    if (!Parse_ConstChar(par, ' ', true, NULL))
	return false;

    day = ParseTwoDigits(par, true);
    if (day == -1)
	return false;

    if (!Parse_ConstChar(par, ' ', true, NULL))
	return false;

    hour = ParseTwoDigits(par, false);
    if (hour == -1)
	return false;

    if (!Parse_ConstChar(par, ':', true, NULL))
	return false;
    
    min = ParseTwoDigits(par, false);
    if (min == -1)
	return false;

    if (Parse_ConstChar(par, ':', true, NULL)) {
        sec = ParseTwoDigits(par, false);
	if (sec == -1)
	    return false;
    }

    if (!Parse_ConstChar(par, ' ', true, NULL))
	return false;

    char ch = Parse_Peek(par);

    // Check for optional timezone (named or numeric)
    //
    if (isalpha(ch) || ch == '+' || ch == '-') {
	Parse_UntilSpace(par, NULL);
	if (!Parse_ConstChar(par, ' ', true, NULL))
	    return false;
	gotZone = true;
    }
    
    int y1 = ParseTwoDigits(par, false);
    int y2 = ParseTwoDigits(par, false);

    if (y1 == -1 || y2 == -1)
	return false;

    year = y1 * 100 + y2;

    // Check for optional timezone again (named or numeric)
    //
    if (!gotZone) {
	ch = Parse_Peek(par);
	if (isalnum(ch) || ch == '+' || ch == '-') {
	    Parse_UntilSpace(par, NULL);
	}
    }

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
			     labs(tm->tm_gmtoff / 3600),
			     labs(tm->tm_gmtoff / 60 % 60));
}

void Stream_WriteCTime(Stream *output, struct tm *tm)
{
    // www mmm dd hh:mm:ss yyyy

    Stream_PrintF(output, "%s %s %02d %02d:%02d:%02d %4d",
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

Headers *Headers_Clone(Headers *headers, Message *msg)
{
    Headers *newHeaders = New(Headers);
    Header **pNewHead = &newHeaders->root;
    Header *oldHead;

    newHeaders->msg = msg;

    for (oldHead = headers->root; oldHead != NULL; oldHead = oldHead->next) {
	Header *newHead = New(Header);

	newHead->key = String_Clone(oldHead->key);
	newHead->value = String_Clone(oldHead->value);
	newHead->line = String_Clone(oldHead->line);

	*pNewHead = newHead;
	pNewHead = &newHead->next;
    }

    *pNewHead = NULL;

    return newHeaders;
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
    Header *head = New(Header);
    int warnCount = 0;
    char ch;

    if (gCheck) {
	// Check validity of header name
	//
	ch = Parse_Peek(par);
	if ((ch >= '\0' && ch <= ' ') || ch == ':') {
	    Parser_Warn(par, "Header starts with illegal character %s",
			Char_QuotedCString(ch));
	}
    }

    // Parse header name
    //
    int pos = Parser_Position(par);
    Parse_StringStart(par, &head->line);
    Parse_StringStart(par, &head->key);
    while (Parse_Char(par, &ch) && ch != ':') {
	if (ch == ' ') {
	    // Whoa, hold it right there!  There shouldn't be any spaces in
	    // header keys.  Is it a "From " line that we've stumbled upon?
	    Parse_StringEnd(par, head->key);
	    if (String_IsEqual(head->key, &Str_FromSpace, true)) {
		// Yup, complain & back up.
		Parser_MoveTo(par, pos);
		Parser_Warn(par, "Encountered unexpected \"From \" line in "
			    "headers {@%d}", Parser_Position(par));
		return false;
	    }
	    /* Or is it a ">From" line?
	     */
	    if (String_IsEqual(head->key, &Str_GTFromSpace, true)) {
		// Yup, complain & accept it.
		Parser_Warn(par, "Encountered unexpected \"%s\" line in "
			    "headers {@%d}", 
			    String_CString(head->key), Parser_Position(par));
		break;
	    }
	}
	if (gCheck && ch >= '\0' && ch <= ' ') {
	    if (++warnCount < kCheck_MaxWarnCount)
		Parser_Warn(par, "Illegal character %s in message "
			    "headers%s {@%d}", Char_QuotedCString(ch), 
			    warnCount == kCheck_MaxWarnCount ?
				" (and more)" : "",
			    Parser_Position(par));
	}
    }
    // Backup over the colon (unless it's ">From ")
    if (ch == ':') {
	Parse_StringEnd(par, head->key);
	head->key->len--;
	String_TrimSpaces(head->key);
    }

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
	if (Parser_AtEnd(par) || !Parse_Header(par, pHead)) {
	    Parser_Warn(par, "Message %s: Header parsing ended prematurely",
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
	    if (!String_IsEqual(head->key, &Str_GTFromSpace, true))
		Stream_WriteChars(output, ": ", 2);
	    Stream_WriteString(output, head->value);
	    Stream_WriteNewline(output);
	}
    }
}

String *Message_SynthesizeMessageID(Message *msg)
{
    const String *idHeaderKeys[] = {
	&Str_Cc, &Str_Date, &Str_From, &Str_Sender, &Str_Subject, &Str_To, NULL
    };
    md5_state_t md5state;
    md5_byte_t md5digest[16];
    String *result = String_Alloc(1 + sizeof(md5digest) * 2 +
				  strlen(kSyntheticMessageIDSuffix) + 1);
    Header *header;

    md5_init(&md5state);

    /* Compute the message ID based on the MD5 checksum for a set of
     * identifying headers + the message body.
     */

    for (header = msg->headers->root; header != NULL; header = header->next) {
	const String **pkey;

	for (pkey = idHeaderKeys; *pkey != NULL; pkey++) {
	    if (String_IsEqual(header->key, *pkey, true)) {
		// XXX: Should decode the header value before using it
		md5_append(&md5state,
			   (md5_byte_t *) String_Chars(header->value),
			   String_Length(header->line));
		break;
	    }
	}
    }

    md5_append(&md5state, (md5_byte_t *) String_Chars(msg->body),
	       String_Length(msg->body));

    md5_finish(&md5state, md5digest);

    sprintf((char *) String_Chars(result),
	    "<%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x"
	    kSyntheticMessageIDSuffix ">",
	    md5digest[ 0], md5digest[ 1], md5digest[ 2], md5digest[ 3],
	    md5digest[ 4], md5digest[ 5], md5digest[ 6], md5digest[ 7],
	    md5digest[ 8], md5digest[ 9], md5digest[10], md5digest[11],
	    md5digest[12], md5digest[13], md5digest[14], md5digest[15]);

    return result;
}

/**
 **  MIME Functions
 **/

/* Quick'n'Dirty implementation -- resulting string should be String_Freed */
String *MIME_GetParameter(String *str, const String *key)
{
    Parser parser;
    String *value;

    Parser_Set(&parser, str);

    while (Parse_UntilChar(&parser, ';', false, NULL)) {
	(void) Parse_ConstChar(&parser, ';', false, NULL);
	(void) Parse_Spaces(&parser, NULL);
	if (Parse_ConstString(&parser, key, false, NULL)) {
	    (void) Parse_Spaces(&parser, NULL);
	    if (Parse_ConstChar(&parser, '=', false, NULL)) {
		(void) Parse_Spaces(&parser, NULL);
		// Got it!  Is it quoted?
		if (Parse_ConstChar(&parser, '"', false, NULL) &&
		    Parse_UntilChar(&parser, '"', false, &value)) {
		    return value;
		} else if (Parse_UntilChar(&parser, ';', false, &value)) {
		    String_TrimSpaces(value);
		    return value;
		} else {
		    Parse_UntilEnd(&parser, &value);
		    String_TrimSpaces(value);
		    return value;
		}
	    }
	}
    }

    return NULL;
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
	String_Free(msg->envelope);
	Headers_Free(msg->headers);
	String_Free(msg->body);
	// Don't free msg->cachedID -- it is "owned" by the headers
	xfree(msg);

	if (!all)
	    break;
    }
}

Message *Message_Clone(Message *msg)
{
    Message *new = Message_New(NULL, 0);

    new->tag = String_Clone(msg->tag);
    new->data = String_Clone(msg->data);
    new->envelope = String_Clone(msg->envelope);
    new->envSender = String_Clone(msg->envSender);
    new->envDate = msg->envDate;
    new->headers = Headers_Clone(msg->headers, new);
    new->body = String_Clone(msg->body);
    new->cachedID = NULL;
    new->deleted = msg->deleted;
    new->dirty = true;
    new->dovecotFromSpaceBug = msg->dovecotFromSpaceBug;

    return new;
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
    Header_Set(msg->headers, &Str_ContentLength,
	       String_PrintF("%d", String_Length(body)));
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

static bool ParseFromSpaceHelper(Parser *par, String **pEnvSender,
				 struct tm *pEnvTime)
{
    int pos = Parser_Position(par);

    // Got "From "?
    //
    if (!Parse_ConstString(par, &Str_FromSpace, true, NULL))
	return false;

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
    if (!Parse_CTime(par, pEnvTime)) {
	String_FreeP(pEnvSender);
	Parser_MoveTo(par, pos);
	return false;
    }

    // Allow possible "garbage" to come after the timestamp,
    // e.g. "remote from foobar" like in the old uucp days.

    (void) Parse_UntilNewline(par, NULL);
    if (!Parse_Newline(par, NULL)) {
	String_FreeP(pEnvSender);
	Parser_MoveTo(par, pos);
	return false;
    }	

    return true;
}

bool Parse_FromSpaceLine(Parser *par, String **pLine, String **pEnvSender,
			 struct tm *pEnvTime)
{
    if (pLine != NULL)
	Parse_StringStart(par, pLine);

    bool success = ParseFromSpaceHelper(par, pEnvSender, pEnvTime);

    if (pLine != NULL) {
	if (success)
	    Parse_StringEnd(par, *pLine);
	else
	    String_FreeP(pLine);
    }

    return success;
}

// Return true and update the parser to point to the next "From " line given
// that it is preceeded by the required number of newlines. Otherwise, return
// false and don't move the parser position.
//
bool Parse_UntilFromSpace(Parser *par, int newlines)
{
    int savedPos = Parser_Position(par);

    while (Parse_UntilString(par, &Str_FromSpace, true, NULL)) {
	int i, pos = Parser_Position(par);

	for (i = 0; i < newlines && Parse_BackupNewline(par); i++);
	// We succeeded if we found enough newlines before the From_
	// line *and* we didn't go back before our starting position.
	if (i == newlines && Parser_Position(par) > savedPos)
	    return true;

	Parser_MoveTo(par, pos + String_Length(&Str_FromSpace));
    }

    Parser_MoveTo(par, savedPos);

    return false;
}

void WarnContentLength(Message *msg, int contLen, int bodyLen)
{
    int delta = abs(contLen - bodyLen);

    if (delta > 1 && contLen > bodyLen) {
	Warn("Message %s: Truncated, %d bytes missing",
	     String_CString(msg->tag), contLen - bodyLen);

    } else if (delta > 1 && contLen < bodyLen) {
	Warn("Message %s: Oversized, %d bytes too many",
	     String_CString(msg->tag), bodyLen - contLen);

    } else if (gStrict) {
	Warn("Message %s: Incorrect Content-Length: %d; using %d",
	     String_CString(msg->tag), contLen, bodyLen);
    }
}

int ProcessDovecotFromSpaceBugBody(Parser *par, int endPos,
				   DovecotFromSpaceBugType bug,
				   Array *bodyParts)
{
    int xHeadSpace = 0;
    String *part = NULL;

    if (bodyParts != NULL)
	Parse_StringStart(par, &part);

    // Dovecot isn't very stringent about what's preceeding a legal "From "
    // line, so just look for a single newline instead of two.
    //
    // Don't forget that the first line may be a valid "From " line too, so
    // apply a bit of trickery to get the parsing sequence right.
    //
    for (;;) {
	if (!Parse_FromSpaceLine(par, NULL, NULL, NULL)) {
	    if (!Parse_UntilNewline(par, NULL) ||
		Parser_Position(par) >= endPos)
		break;

	    Parse_Newline(par, NULL);
	    continue;
	}

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
	    if ((((bug & kDFSB_ContLen) != 0 &&
		  Parse_ConstString(par, &Str_ContentLength, false, NULL)) ||
		 ((bug & kDFSB_XUIDKeys) != 0 && 
		  Parse_ConstString(par, &Str_XUID, false, NULL)) ||
		 ((bug & kDFSB_XUIDKeys) != 0 && 
		  Parse_ConstString(par, &Str_XKeywords, false, NULL)) ||
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
	kDFSB_XUIDKeys | kDFSB_ContLen | kDFSB_Status,
	kDFSB_XUIDKeys | kDFSB_ContLen,
	kDFSB_XUIDKeys | kDFSB_Status,
	kDFSB_XUIDKeys,
	kDFSB_XUIDKeys | kDFSB_ContLen | kDFSB_Status |  kDFSB_Newline,
	kDFSB_XUIDKeys | kDFSB_ContLen |  kDFSB_Newline,
	kDFSB_XUIDKeys | kDFSB_Status |  kDFSB_Newline,
	kDFSB_XUIDKeys | kDFSB_Newline,
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
	// Go back to point at the body.
	//
	Parser_MoveTo(par, savedPos - cllen);
	xHeadSpace = ProcessDovecotFromSpaceBugBody(par, savedPos, *bugp, NULL);

	// Did we find anything, and if so, did it make the Content-Length
	// valid?
	//
	if (xHeadSpace > 0 && Parser_MoveTo(par, savedPos + xHeadSpace)) {
#if 1 || defined(OPT_FUZZY_NEWLINE)
	    int ch = Parse_Peek(par);

	    // Look for "[\n]\nFrom "...
	    //
	    if (ch == 'F' || ch == EOF) {
		// Got an 'F' instead of a newline. Maybe we've arrived
		// right at the next message's "From " line instead?
		// Check to see if a newline preceeds us.  If so, continue
		// as if nothing had happened.
		//
		Parser_Move(par, -1);
		ch = Parse_Peek(par);
		if (ch != '\n') {
		    // No good
		    Parser_Move(par, 1);
		}
	    }
#endif

	    int pos = Parser_Position(par);

	    // Allow one or two newlines here as one might
	    // have gotten added by Dovecot when it added the
	    // extra headers.
	    //
	    if (!Parse_Newline(par, NULL))
		continue;
	    
	    if (Parse_Newline(par, NULL))
		pos = Parser_Position(par) - 1;

	    if (Parser_AtEnd(par) ||
		Parse_FromSpaceLine(par, NULL, NULL, NULL)) {
		// Yup!  (God damn it)  
		// Move to the new end and remember this for the future...
		//
		Parser_MoveTo(par, pos);
		msg->dovecotFromSpaceBug = *bugp;
		return true;
	    }
	}
    }

    Parser_MoveTo(par, savedPos);

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

    // Content-Length: should be correct now, but better check it
    String *clstr = Header_Get(msg->headers, &Str_ContentLength);
    int cllen = String_ToInteger(clstr, -1);
    int bolen = String_Length(msg->body);

    if (cllen != bolen) {
	if (cllen != -1)
	    WarnContentLength(msg, cllen, bolen);

	Header_Set(msg->headers, &Str_ContentLength,
		   String_PrintF("%d", bolen));
    }
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
	if (Parser_Move(par, cllen)) {
	    int endPos = Parser_Position(par);
#ifdef OPT_FUZZY_NEWLINE
	    int ch = Parse_Peek(par);

	    if (ch == 'F') {
		// Maybe we get too far?  Go back one char and recheck.
		Parser_Move(par, -1);
		ch = Parse_Peek(par);
		if (ch != '\n') {
		    Parser_Move(par, 1);
		    ch = 'F';
		}
	    }
#endif

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
#ifdef IMMEDIATE_WARNINGS
		Parser_Warn(par, "Message %s: Corrupted by Dovecot "
			    "\"From \" bug", String_CString(msg->tag));
#endif
		return;

	    } else {
#ifdef EXTEND_INCORRECT_CONTENT_LENGTH
		// Couldn't find a proper "From " line were we expected
		// it.  Start scanning at the beginning of the message
		// and make note of the last "From " lines we find before
		// the (alleged) end of the message + the first one
		// after.  Then see which one gives the least error.
		//
		Parser_MoveTo(par, bodyPos);

		int lastPos = -1;
		int fromPos = -1;

		while (Parse_UntilFromSpace(par, 2)) {
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

		WarnContentLength(msg, cllen, fromPos - bodyPos);
		Parser_MoveTo(par, fromPos);
		return;
#else
		// Couldn't find a proper "From " line where we expected it.
		// Start scanning at the beginning of the message and break
		// at the first proper "From " line we find.
		//
		Parser_MoveTo(par, bodyPos);

		int fromPos = -1;

		while (Parse_UntilFromSpace(par, 2)) {
		    Parse_Newline(par, NULL);
		    fromPos = Parser_Position(par);
		    Parse_Newline(par, NULL);

		    if (Parse_FromSpaceLine(par, NULL, NULL, NULL)) {
			break;
		    }
		}

		if (fromPos == -1) {
		    // Never found *any* "From " line -- go to end
		    //
		    Parse_UntilEnd(par, NULL);
		    fromPos = Parser_Position(par);
		}

#ifdef IMMEDIATE_WARNINGS
		WarnContentLength(msg, cllen, fromPos - bodyPos);
#endif
		Parser_MoveTo(par, fromPos);
		return;
#endif
	    }
	}
    }

    /* Invalid or missing Content-Length.  See if we happen to have a
     * multipart message with a valid ending boundary.  If so, we can
     * be pretty sure as to where the message ends.
     */
    String *contentType = Header_Get(msg->headers, &Str_ContentType);
    if (contentType != NULL &&
	String_HasPrefix(contentType, &Str_Multipart, false)) {
	String *boundary = MIME_GetParameter(contentType, &Str_Boundary);
	bool done = false;

	if (boundary != NULL) {
	    String *boundaryEnd =
		String_Append(&Str_TwoDashes, boundary, &Str_TwoDashes, NULL);

	    done = (Parse_UntilString(par, boundaryEnd, true, NULL) &&
		    Parser_Move(par, -1) &&
		    Parse_Newline(par, NULL) &&
		    Parse_ConstString(par, boundaryEnd, true, NULL) &&
		    Parse_Newline(par, NULL));

	    String_Free(boundaryEnd);
	}

	String_Free(boundary);
			
	if (done)
	    // Got it!
	    return;
    }

    /* As a last resort, try searching for a valid "\nFrom " line instead.
     * This is a bit dodgy as messages may potentially contain such a
     * line as part of their bodies, e.g. when quoting another message.
     * But what can you do...
     */

    Parser_MoveTo(par, bodyPos);

#if 1
    /* Look for a valid "From " line, either as the first line of the
     * body, or later on preceeded by a newline.  Note that if we find
     * it as the first line of the body, we will return with the
     * parser position set to the 'F' in "\nFrom " but if we find
     * it later, it will be set to the '\n'.  There is a newline
     * preceeding the "From " in both cases, but in the former case
     * it serves "double duty" as the header terminator as well.
     */
    int pos = Parser_Position(par);
    do {
	if (Parse_FromSpaceLine(par, NULL, NULL, NULL)) {
	    Parser_MoveTo(par, pos);
	    return;
	}
    } while (Parse_UntilFromSpace(par, 1) &&
	     (pos = Parser_Position(par)) &&
	     Parse_Newline(par, NULL));
#else
    /* Look for a valid "\nFrom " line. Note that we're first back
     * up over the newline that terminated the headers before starting
     * to search.  This is because that newline might actually be part
     * of the next message's "\nFrom " line instead.  If this happens,
     * we will return with the parser pointing to that newline, which
     * may cause our caller (Parse_Message) to save a body with a negative
     * length!  That's why this code currently is disabled...
     */
    Parse_BackupNewline(par);
    
    while (Parse_UntilFromSpace(par, 1)) {
        int pos = Parser_Position(par);

	(void) Parse_Newline(par, NULL);
        if (Parse_FromSpaceLine(par, NULL, NULL, NULL)) {
	    /* Be aware that we might return a position that is
	     * before the bodyPos in case the newline separating the
	     * headers from the body also belongs to the next message's
	     * "\nFrom " line.
	     */
            Parser_MoveTo(par, pos);
            return;
        }
    }
#endif

    // Go to the end of the mailbox minus one newline
    //
    Parse_UntilEnd(par, NULL);
    Parser_Move(par, -1);
    // Looking at newline?  If not, go to real end (shouldn't happen)
    if (!Char_IsNewline(Parse_Peek(par)))
	Parser_Move(par, 1);
}

bool Parse_Message(Parser *par, Mailbox *mbox, bool useAllData, Message **pMsg)
{
    // Skip over possible newslines (should not be here, but...)
    if (Parse_Newline(par, NULL)) {
	Warn("Unexpected newline(s) after message %d", mbox->count);
	while (Parse_Newline(par, NULL));
    }

    int savedPos = Parser_Position(par);

    if (Parser_AtEnd(par))
	return false;

    Message *msg = New(Message);

    msg->mbox = mbox;
    msg->num = ++mbox->count;
    msg->tag = String_PrintF("#%d {@%d}", msg->num, Parser_Position(par));

    Parse_StringStart(par, &msg->data);

    // Allow (expect) for a "From " envelope to start the message
    //
    if (!Parse_FromSpaceLine(par, &msg->envelope, &msg->envSender,
			     &msg->envDate)) {
	Parser_Warn(par, "Could not find a valid \"From \" line for message %s",
		    String_CString(msg->tag));
    } else if (String_IsEmpty(msg->envSender)) {
	Parser_Warn(par, "Empty envelope sender for message %s",
		    String_CString(msg->tag));
    }

    // Parse headers (until & including empty line)
    //
    if (!Parse_Headers(par, msg, &msg->headers)) {
	Parser_Warn(par, "Message %s: Could not parse headers",
		    String_CString(msg->tag));
	Parser_MoveTo(par, savedPos);
	return false;
    }

    // Parse body
    //
    Parse_StringStart(par, &msg->body);

    // Find the end of the message
    //
    if (useAllData)
	Parse_UntilEnd(par, NULL);
    else
	MoveToEndOfMessage(par, msg);

    Parse_StringEnd(par, msg->body);
    Parse_StringEnd(par, msg->data);

    *pMsg = msg;

    return true;
}

void Stream_WriteMessage(Stream *output, Message *msg)
{
    if (msg->envelope != NULL) {
	Stream_WriteString(output, msg->envelope);

    } else if (msg->envSender != NULL) {
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

extern void Mailbox_Unlock(const String *source);

void Mailbox_Free(Mailbox *mbox)
{
    Mailbox_Unlock(mbox->source);
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

	if (pos++ == kString_NotFound)
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

void Mailbox_Append(Mailbox *mbox, Message *msg)
{
    Message **pRoot = &mbox->root;

    if (msg->mbox != NULL || msg->next != NULL)
	Fatal(EX_SOFTWARE, "Internal error: Trying to add tied message "
	      "%s to mailbox %s", msg->tag, Mailbox_Name(mbox));

    while (*pRoot != NULL)
	pRoot = &(*pRoot)->next;

    *pRoot = msg;
    msg->mbox = mbox;
    msg->num = ++mbox->count;

    Mailbox_SetDirty(mbox, true);
}

bool Parse_Messages(Parser *par, Mailbox *mbox)
{
    Message **pMsg = &mbox->root;

    if (gVerbose)
	Note("Parsing mailbox %s", String_CString(Mailbox_Name(mbox)));

    // Append at the end
    while (*pMsg != NULL)
	pMsg = &(*pMsg)->next;

    while (Parse_Message(par, mbox, false, pMsg)) {
	Parse_Newline(par, NULL);
	pMsg = &(*pMsg)->next;
    }

    if (!Parser_AtEnd(par))
	Parser_Warn(par, "Unparsable garbage at end of mailbox (@%d):\n %s",
		    Parser_Position(par), String_QuotedCString(&par->rest, 72));

    return true;
}

int readint(int fd)
{
    char buf[16];
    int nc = read(fd, buf, sizeof(buf) - 1);
    if (nc < 0)
	return -1;
    buf[nc] = '\0';
    return atoi(buf);
}

bool writeint(int fd, int k)
{
    char buf[16];
    sprintf(buf, "%d", k);
    int len = strlen(buf);
    return write(fd, buf, len) == len;
}

static Array *gLockedMailboxes;

bool Mailbox_Lock(const String *source, int timeout)
{
    //String *tmpFile;
    //String *lockFile;

    if (gDryRun)
	return true;

#ifdef OPT_CCLIENT_LOCK
    ...
#endif

#ifdef OPT_LOCK_FILE
    String *lockFile = String_Append(source, &Str_DotLock, NULL);
    const char *cLockFile = String_CString(lockFile);
    time_t start, end;
    int fd;

    time(&start);

    // Try to create a .lock file, but only if one doesn't already exist
    //
    while ((fd = open(cLockFile, O_WRONLY|O_EXCL|O_CREAT, 0444)) == -1) {
	int err = errno;
	
	// Give up if we've been waiting too long
	//
	time(&end);

	if (end - start > timeout) {
	    // Fake a more meaningful error if needed
	    errno = (err == EEXIST) ? ENOLCK : err;
	    String_Free(lockFile);
	    return false;
	}
	
	// Check if 1) the file already exists, 2) if it contains a pid of
	// the owner, and 3) if the owner process still exists.
	//
	if (errno == EEXIST) {
	    fd = open(cLockFile, O_RDONLY);
	    if (fd != -1) {
		int pid = readint(fd);
		close(fd);

		if (pid > 0 && kill(pid, 0) == -1) {
		    // Process is gone, nuke the lock
		    Note("Removing lock %s from defunct process %d",
			 cLockFile, pid);
		    if (unlink(cLockFile) != 0) {
			// Ugh!
			String_Free(lockFile);
			return false;
		    }

		    // Try locking it again
		    continue;
		}
	    }
	}

	sleep(1);
    }

    String_Free(lockFile);

    // Great, got the .lock file!  Now record our pid in it
    //
    if (!writeint(fd, getpid())) {
	int err = errno;
	close(fd);
	errno = err;
	return false;
    }

    if (close(fd) != 0) {
	return false;
    }
#endif

    // Remember that we've locked this mailbox
    Array_Append(gLockedMailboxes, String_Clone(source));

    return true;
}

void Mailbox_Unlock(const String *source)
{
    if (gDryRun)
	return;

#ifdef OPT_LOCK_FILE
    String *lockFile = String_Append(source, &Str_DotLock, NULL);
    const char *cLockFile = String_CString(lockFile);
    int fd;

    // Make sure we still own the lock
    fd = open(cLockFile, O_RDONLY);
    if (fd == -1) {
	Warn("Could not open lock file %s: %s", cLockFile, strerror(errno));
	String_Free(lockFile);
	return;
    }

    int pid = readint(fd);
    close(fd);

    if (pid != getpid()) {
	// Not ours
	if (pid < 0)
	    Warn("Could not read lock file %s: %s", cLockFile, strerror(errno));
	else if (pid == 0)
	    Warn("Someone stole lock file %s", cLockFile);
	else
	    Warn("Someone with pid %d stole lock file %s", pid, cLockFile);
	String_Free(lockFile);
	return;
    }

    if (unlink(cLockFile) != 0) {
	Error("Could not remove lock file %s: %s", cLockFile, strerror(errno));
	String_Free(lockFile);
	return;
    }

    String_Free(lockFile);
#endif

    int i;

    for (i = 0; i < Array_Count(gLockedMailboxes); i++) {
	const String *oldLock = Array_GetAt(gLockedMailboxes, i);
	if (String_IsEqual(oldLock, source, true)) {
	    Array_DeleteAt(gLockedMailboxes, i);
	    break;
	}
    }
}

void Mailbox_UnlockAll(void)
{
    int i;

    for (i = Array_Count(gLockedMailboxes) - 1; i >= 0; i--) {
	Mailbox_Unlock(Array_GetAt(gLockedMailboxes, i));
    }
}

Mailbox *Mailbox_OpenQuietly(const String *source, bool create)
{
    // We only support mbox files for now
    //
    Stream *input = Stream_Open(source, false, false);
    String *data = NULL;
    Mailbox *mbox;

    if (input != NULL) {
	if (!Stream_ReadContents(input, &data)) {
	    // Save the errno for our caller
	    //
	    int err = errno;
	    Stream_Free(input, true);
	    errno = err;
	    return NULL;
	}

	Stream_Free(input, true);

    } else if (!create) {
	return NULL;
    }

    Parser parser;

    mbox = New(Mailbox);

    mbox->source = String_Clone(source);
    mbox->data = data;

    if (data != NULL) {
	Parser_Set(&parser, data);
	Parse_Messages(&parser, mbox);
    }

    return mbox;
}

Mailbox *Mailbox_Open(const String *source, bool create)
{
    if (gVerbose)
	Note("Locking mailbox %s", String_CString(source));

    if (!Mailbox_Lock(source, kDefaultLockTimeout)) {
	Error("Could not lock %s: %s",
	      String_CString(source), strerror(errno));
	return NULL;
    }

    if (gVerbose)
	Note("Opening mailbox %s", String_CString(source));

    Mailbox *mbox = Mailbox_OpenQuietly(source, create);

    if (mbox == NULL) {
	Mailbox_Unlock(source);
	Error("Could not open %s: %s",
	      String_CString(source), strerror(errno));
	return NULL;
    }

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
	String *imap = NULL;

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

bool Mailbox_Write(Mailbox *mbox, const String *destination, bool fatal)
{
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
    const String *file = destination;
    Stream *tmp = Stream_OpenTemp(file, true, true);

    Stream_WriteMailbox(tmp, mbox, true);
    Stream_Close(tmp);

    const char *cFile = String_CString(destination);

    if (gBackup) {
	int len = String_Length(file);
	char bakPath[len + 1 + 1];

	memcpy(bakPath, cFile, len);
	strcpy(&bakPath[len], "~");

	if (rename(cFile, bakPath) != 0) {
	    Fatal(fatal ? EX_CANTCREAT : EX_OK,
		  "Could not rename %s to %s: %s",
		  cFile, bakPath, strerror(errno));
	    return false;
	}
    }

    if (rename(String_CString(tmp->name), cFile) != 0) {
	Fatal(fatal ? EX_CANTCREAT : EX_OK, "Could not rename %s to %s: %s",
	      String_CString(tmp->name), cFile, strerror(errno));
	return false;
    }

    Stream_Free(tmp, false);

    Mailbox_SetDirty(mbox, false);

    return true;
}

bool Mailbox_Save(Mailbox *mbox, bool force, bool fatal)
{
    if (!Mailbox_IsDirty(mbox) && !force) {
	Note("Leaving mailbox %s unchanged",
	     String_CString(Mailbox_Name(mbox)));
	return true;

    } else {
	return Mailbox_Write(mbox, mbox->source, fatal);
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
    static String line = {NULL, 0, kString_Const};
#ifdef USE_READLINE
    static char *buf = NULL;

    xfree(buf);

    buf = readline(prompt);

    if (buf == NULL)
	return false;
    
    add_history(buf);
#else
    static char buf[1024];

    fprintf(stdout, "%s", prompt);
    if (fgets(buf, sizeof(buf), stdin) == NULL)
	return false;
#endif

    if (pLine != NULL) {
	int len = strlen(buf);
	if (len > 0 && buf[len-1] == '\n')
	    len--;
	buf[len] = '\0';
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
	int ch;

#if 0
	// Present the question and the options
	//
	fprintf(stdout, "%s [", question);
	for (p = choices; *p != '\0'; p++) {
	    putc((*p == def) ? toupper(*p) : *p, stdout);
	}
	fprintf(stdout, "] ");
#else
	fprintf(stdout, "%s %c\010", question, def);
#endif

	// Get an answer
	//
	while ((ch = getchar()) != EOF && ch == ' ');
	if (ch == EOF)
	    break;

	// Validate the answer
	//
	if (ch == '\n')
	    break;

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
** Message Sets
*/

typedef struct _MessageSet {
    int min, max;
    struct _MessageSet *link;
} MessageSet;

MessageSet *MessageSet_Make(int min, int max, MessageSet *link)
{
    MessageSet *set = New(MessageSet);

    set->min = min;
    set->max = max;
    set->link = link;

    return set;
}

void MessageSet_Free(MessageSet *set)
{
    while (set != NULL) {
	MessageSet *link = set->link;
	free(set);
	set = link;
    }
}

// Parse a message set specification of the form:
//   <min>['-'[<max>]][','...] | '*'
//
bool Parse_MessageSet(Parser *par, MessageSet **pSet, int last)
{
    int min, max;
    MessageSet *link = NULL;

    if (Parse_ConstChar(par, '*', true, NULL)) {
	min = 1;
	max = last;

    } else {
	if (!Parse_Integer(par, &min))
	return false;
	if (Parse_ConstChar(par, '-', true, NULL)) {
	    if (!Parse_Integer(par, &max))
		max = last;
	} else {
	    max = min;
	}
	if (Parse_ConstChar(par, ',', true, NULL)) {
	    (void) Parse_MessageSet(par, &link, last);
	}
    }

    *pSet = MessageSet_Make(min, max, link);
    return true;
}

MessageSet* MessageSet_Append(MessageSet *a, MessageSet *b)
{
    MessageSet **pa;

    for (pa = &a; *pa != NULL; pa = &(*pa)->link);
    *pa = b;

    return a != NULL ? a : b;
}

int MessageSet_First(MessageSet *set)
{
    if (set == NULL)
	return -1;
    return set->min;
}

int MessageSet_Next(MessageSet *set, int cur)
{
    for (; set != NULL; set = set->link) {
	if (cur < set->min)
	    return set->min;
	if (cur < set->max)
	    return cur + 1;
    }

    return -1;
}

/*
**  Application Functions
*/

void InterruptHandler(int signum)
{
    putchar('\n');

    if (gOpenPipe != NULL) {
	pclose(gOpenPipe);
	gOpenPipe = NULL;
    }

#if 0
    // Kill all subprocesses in our process group
    printf("# killpg\n");
    sig_t oldHandler = signal(SIGTERM, SIG_IGN);
    killpg(0, SIGTERM);
    signal(SIGTERM, oldHandler);
#endif

    if (signum == SIGINT && gInterruptReentry != NULL)
	longjmp(*gInterruptReentry, 0);

    // Close all open mailboxes
    Mailbox_UnlockAll();

    // Resend signal (and hopefully die)
    signal(signum, SIG_DFL);
    kill(0, signum);

    // Still here?
    exit(EX_UNAVAILABLE);
}

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


/* Check the string for "illegal" characters such as control chars
 * or non-ASCII chars (unless eightBitOK is set).
 * Return offset to illegal char if found, or -1 if OK.
 */
static int FindIllegalChar(String *str, bool controlOK, bool eightBitOK)
{
    int i, len = String_Length(str);
    const char *chars = String_Chars(str);

    for (i = 0; i < len; i++, chars++) {
	if (*chars == '\r' || *chars == '\n' || *chars == '\t')
	    continue;
	if (!controlOK && ((*chars >= '\0' && *chars < ' ') || *chars == '\177'))
	    return i;
	if (!eightBitOK && !isascii(*chars))
	    return i;
    }

    return kString_NotFound;
}

typedef struct {
    bool repair;		// Are we repairing?
    char autoChoice;		// Should this choice apply without asking?
    bool quit;			// Have the user told us to quit repairing?
} RepairState;

void InitRepairState(RepairState *state, bool repair)
{
    state->repair = repair;
    state->autoChoice = gInteractive ? '\0' : 'y';
    state->quit = false;
}

bool IsRepairingAll(RepairState *state)
{
    return state->repair && state->autoChoice == 'y';
}

bool ShouldRepair(RepairState *state)
{
    int choice = state->autoChoice;

    if (!state->repair)
	return false;

    if (choice == '\0')
	choice = User_AskChoice(" Repair [ynq]?", "ynYNq", 'y');

    // An uppercase answer will apply to all remaining questions
    if (isupper(choice))
	choice = state->autoChoice = tolower(choice);

    state->quit = (choice == 'q');

    return choice == 'y';
}

void CheckMailbox(Mailbox *mbox, bool strict, bool repair)
{
    Message *msg;
    RepairState state;

    InitRepairState(&state, repair);

    for (msg = Mailbox_Root(mbox); msg != NULL && !state.quit;
	 msg = msg->next) {
	String *value;
	const String *source = NULL;
	int cllen;

	// Check Content-Length
	//
	value = Header_Get(msg->headers, &Str_ContentLength);
	cllen = String_ToInteger(value, -1);

	int bodyLength = Message_BodyLength(msg);

	// Always care about incorrect Content-Lengths, but only
	// care about missing ones if we're being strict.
	if (cllen != bodyLength && (value != NULL || strict)) {
	    // Got the Dovecot "From " bug?
	    //
	    if (msg->dovecotFromSpaceBug != kDFSB_None) {
		// Yup, remove bogus headers from the body
		Warn("Message %s: Corrupted by Dovecot \"From \" bug%s",
		     String_CString(msg->tag),
		     IsRepairingAll(&state) ? " (repairing)" : "");

		if (ShouldRepair(&state)) {
		    RepairDovecotFromSpaceBugBody(msg);
		    // Repairing the message will change it's length
		    bodyLength = Message_BodyLength(msg);
		} else if (state.quit)
		    break;

	    } else {
		if (value == NULL)
		    Warn("Message %s: Missing Content-Length:, should be %d%s",
			 String_CString(msg->tag), bodyLength,
			 IsRepairingAll(&state) ? " (repairing)" : "");
		else
		    Warn("Message %s: Incorrect Content-Length: %s, "
			 "should be %d%s", String_CString(msg->tag),
			 String_PrettyCString(value), bodyLength,
			 IsRepairingAll(&state) ? " (repairing)" : "");

		if (ShouldRepair(&state))
		    Header_Set(msg->headers, &Str_ContentLength,
			       String_PrintF("%d", bodyLength));
		else if (state.quit)
		    break;
	    }
	}

	// Got Message-ID?
	//
	value = Header_Get(msg->headers, &Str_MessageID);
	if (value == NULL || String_IsEmpty(value)) {
	    source = &Str_XMessageID;
	    value = Header_Get(msg->headers, source);

	    if (value == NULL || String_IsEmpty(value)) {
		String *synthID = Message_SynthesizeMessageID(msg);

		Warn("Message %s: Missing Message-ID: header, %s with %s",
		     String_CString(msg->tag),
		     IsRepairingAll(&state) ? "replacing" : "could replace",
		     String_CString(synthID));

		if (ShouldRepair(&state))
		    Header_Set(msg->headers, &Str_MessageID, synthID);
		else if (state.quit)
		    break;
	    }
	}

	// Only strict tests below
	//
	if (!strict)
	    continue;

	// Got ">From " in headers?
	//
	value = Header_Get(msg->headers, &Str_GTFromSpace);
	if (value != NULL) {
	    Warn("Message %s: Bogus \">From \" line in the the headers:\n"
		 " \">From %s\"%s",
		 String_CString(msg->tag), String_CString(value),
		 IsRepairingAll(&state) ? " (removing)" : "");

	    if (ShouldRepair(&state))
		Header_Delete(msg->headers, &Str_GTFromSpace, false);
	    else if (state.quit)
		break;
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
		Warn("Message %s: Missing From: header",
		     String_CString(msg->tag));

	    } else {
		Warn("Message %s: Missing From: header, %s %s:\n"
		     " \"%s\"", String_CString(msg->tag),
		     IsRepairingAll(&state) ? "using" : "but could use",
		     String_CString(source), String_CString(value));

		if (ShouldRepair(&state)) {
		    Header_Set(msg->headers, &Str_From, value);
		    value = NULL;
		} else if (state.quit)
		    break;
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
		    if (pos != kString_NotFound) {
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
		Warn("Message %s: Missing Date: header",
		     String_CString(msg->tag));

	    } else {
		Warn("Message %s: Missing Date: header, %s %s:\n"
		     " \"%s\"", String_CString(msg->tag),
		     IsRepairingAll(&state) ? "using" : "but could use",
		     String_CString(source), String_CString(value));

		if (ShouldRepair(&state)) {
		    Header_Set(msg->headers, &Str_Date, value);
		    value = NULL;
		} else if (state.quit)
		    break;
	    }

	    String_Free(value);
	}

	// Make sure there's no (undeclared) binary data in headers or body
	//
	Header *head;

	for (head = msg->headers->root; head != NULL; head = head->next) {
	    int pos = FindIllegalChar(head->line, false, false);
	    if (pos >= 0) {
		Warn("Message %s: Illegal character %s in header:\n"
		     " %s", String_CString(msg->tag),
		     Char_QuotedCString(String_CharAt(head->line, pos)),
		     String_PrettyCString(head->line));
	    }
	}

#if 0 // Need to check multipart headers!
	String *cte =
	    Header_Get(msg->headers, &Str_ContentTransferEncoding);
	if (cte == NULL || !String_HasPrefix(cte, &Str_Binary, false)) {
	    String *body = Message_Body(msg);
	    
	    bool is8Bit = String_HasPrefix(cte, &Str_8Bit, false);
	    int pos = FindIllegalChar(body, true, is8Bit);
	    if (pos >= 0) {
		int off = iMax(0, pos - kString_ExcerptLength / 2);
		String *sub = String_Sub(body, off, String_Length(body));

		Warn("Message %s: Illegal character %s in body:\n %s%s",
		     String_CString(msg->tag),
		     Char_QuotedCString(String_CharAt(body, pos)),
		     off == 0 ? "" : "...",
		     String_QuotedCString(sub, kString_ExcerptLength));
	    }
	}
#endif
    }
}

void Message_Join(Message *a, Message *b)
{
    Message_SetBody(a, String_Append(Message_Body(a), &Str_Newline,
				     b->data, NULL));
    Message_SetDeleted(b, true);
}

bool Message_Split(Message *msg, bool interactively)
{
    // "\n\nFrom " line in body?
    //
    String *body = Message_Body(msg);
    Parser parser;

    Parser_Set(&parser, body);

    for (;;) {
	String *line;

	if (!Parse_UntilFromSpace(&parser, 2))
	    break;

	if (!Parse_Newline(&parser, NULL) || !Parse_Newline(&parser, NULL))
	    Fatal(EX_SOFTWARE, "Internal error, couldn't parse double newline"
		  "in Message_Split");

	int pos = Parser_Position(&parser);

	if (Parse_FromSpaceLine(&parser, &line, NULL, NULL)) {
	    bool split = true;

	    // Remove the newline from the string we parsed
	    if (!String_IsEmpty(line))
		String_SetLength(line, String_Length(line) - 1);

	    Note("Message %s: Found \"From \" line in body:\n %s",
		 String_CString(msg->tag), String_QuotedCString(line, -1));

	    if (interactively) {
		fprintf(stdout, "Message context:\n");
		WriteQuotedExcerpt(stdout, body, pos, 15, "| ");
		split = User_AskYesOrNo("Split message?", split);
	    }

	    String_Free(line);

	    if (split) {
		Message *newMsg = NULL;

		Parser_MoveTo(&parser, pos);
		while (Parse_Message(&parser, msg->mbox, false, &newMsg)) {
		    if (body != NULL) {
			// Shorten the old body and link in the new msg
			String_SetLength(body, pos - 1);
			Message_SetDirty(msg, true);
		    }
		    newMsg->next = msg->next;
		    msg->next = newMsg;
		    Note("Created new message %s", String_CString(newMsg->tag));

		    Message_SetDirty(newMsg, true);

		    msg = newMsg;
		    body = NULL;

		    Parse_Newline(&parser, NULL);
		}

		return newMsg != NULL;
	    }
	}
    }

    return false;
}

#if 0
void CheckContentLengths(Message *msg, bool strict, bool repair)
{
    for (; msg != NULL; msg = msg->next) {
	String *value = Header_Get(msg->headers, &Str_ContentLength);
	int oldlen = String_ToInteger(value, -1);
	int bodlen = Message_BodyLength(msg);


	if (oldlen != bodlen) {
	    if (repair) {
		if (value != NULL)
		    Note("Message %s: Changing Content-Length: %s to %d",
			 String_CString(msg->tag),
			 String_PrettyCString(value), bodlen);
			 
		else
		    Note("Message %s: Setting Content-Length: to %d",
			 String_CString(msg->tag), bodlen);

		Header_Set(msg->headers, &Str_ContentLength, 
			   String_PrintF("%d", bodlen));

	    } else {
		if (strict)
		    Warn("Message %s: Missing Content-Length: header",
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
#endif

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
    // Disable SIGINT handling while running the pager
    FILE *output = stdout;

    signal(SIGINT, SIG_IGN);

    if (gPager != NULL)
	gOpenPipe = output = popen(String_CString(gPager), "w");


    Stream *stream = Stream_New(output, gPager, true);
    stream->ignoreErrors = true;

    Stream_PrintF(stream, "[Mailbox %s: Message %s]\n",
		  String_CString(Mailbox_Name(msg->mbox)),
		  String_CString(msg->tag));
    Stream_WriteMessage(stream, msg);

    if (gPager != NULL) {
	pclose(gOpenPipe);
	gOpenPipe = NULL;
    }
    Stream_Free(stream, false);
    
    // Restore our interrupt handler
    signal(SIGINT, InterruptHandler);
}

bool EditFile(String *path)
{
    static String *editor = NULL;
    const char *cPath = String_CString(path);
    struct stat sbuf;
    time_t oldmtime;

    if (editor == NULL) {
	const char *cEditor = getenv("EDITOR");
	if (cEditor == NULL)
	    cEditor = kDefaultEditor;
	editor = String_FromCString(cEditor, false);
    }

    if (stat(cPath, &sbuf) != 0) {
	perror(cPath);
	return false;
    }

    oldmtime = sbuf.st_mtime;

    Note("Editing message file %s", cPath);

    String *cmd = String_Append(editor, &Str_Space, path, NULL);
    int ret = system(String_CString(cmd));
    if (ret == -1) {
	Error("Could not execute %s: %s",
	      String_CString(cmd), strerror(errno));
    } else if (ret != 0) {
	Error("%s signalled an error, discarding changes",
	      String_CString(cmd));
    }
    String_Free(cmd);

    if (ret != 0 || stat(cPath, &sbuf) != 0 || sbuf.st_mtime == oldmtime) {
	return false;
    }

    return true;
}

Stream *SaveTempMessage(Message *msg)
{
    String *tmpPath = String_FromCString("/tmp/mfck", false);
    Stream *stream = Stream_OpenTemp(tmpPath, true, true);

    Stream_WriteMessage(stream, msg);
    Stream_Close(stream);

    return stream;
}

void EditMessage(Message *msg)
{
    Stream *stream = SaveTempMessage(msg);
    String *data = NULL;

    if (!EditFile(stream->name)) {
	Note("Message unchanged");
	Stream_Free(stream, true);
	return;
    }

    if (!Stream_Reopen(stream, false, true) ||
	!Stream_ReadContents(stream, &data)) {
	perror(String_CString(stream->name));
	Stream_Free(stream, true);
	return;
    }

    Stream_Free(stream, true);

    Parser parser;
    Message *newMsg;

    Parser_Set(&parser, data);
    if (!Parse_Message(&parser, msg->mbox, true, &newMsg)) {
	Error(EX_OK, "Could not parse message");
	String_Free(data);
	return;
    }

    // Make the message own the (orignal) file data so it will
    // be freed when the message gets freed
    //
    if (String_Chars(data) != String_Chars(newMsg->data)) {
	Warn("Internal error: Message data != parsed file data; leaking!");
    } else {
	String_Free(newMsg->data);
	newMsg->data = data;
    }

    Message **pLink = &msg->mbox->root;

    while (*pLink != NULL && *pLink != msg)
	pLink = &(*pLink)->next;

    if (*pLink != msg) {
	Warn("Internal error: Can't find message %s in mailbox %s",
	     String_CString(msg->tag), String_CString(msg->mbox->source));

	*pLink = newMsg;

    } else {
	*pLink = newMsg;
	newMsg->next = msg->next;
	Message_Free(msg, false);
    }

    Message_SetDirty(newMsg, true);
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
		 int previewLines, int cur)
{
    // ' '<num:numWidth>': '<date:12>'  '<from>'  '<subject>'  '<size:6>
    String *sizstr = String_ByteSize(String_Length(msg->data));
    int fromSubjectWidth = gPageWidth - 27 - numWidth;
    int fromWidth = fromSubjectWidth * 2 / 5;
    int subjectWidth = fromSubjectWidth - fromWidth;

    Stream_PrintF(output, "%c%*d%c ",
		  num == cur ? '>' : ' ', numWidth, num,
		  Message_IsDeleted(msg) ? 'D' : ':');
    PrintShortDate(output, Header_Get(msg->headers, &Str_Date));
    Stream_PrintF(output, "  %-*.*s", fromWidth, fromWidth,
		  String_CString(String_Safe(Header_Get(msg->headers, &Str_From))));
    Stream_PrintF(output, "  %-*.*s",
		  //33 - numWidth, 33 - numWidth,
		  subjectWidth, subjectWidth,
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

void ListMailbox(Stream *output, Mailbox *mbox, int cur, int count)
{
    // A negative count means all messages
    if (count < 0)
	count = Mailbox_Count(mbox) - cur - 1;

    // Adjust to even page boundary with zero offset
    //int start = ((cur - 1) / count) * count;
    int start = cur;
    int i = 1;
    int digits = IntLength(start + count + 1 - 1);
    Message *msg;

    for (msg = Mailbox_Root(mbox); msg != NULL && i < start; msg = msg->next, i++);

    for (; msg != NULL && i < start + count; msg = msg->next, i++) {
	ListMessage(output, i, digits, msg, 0, cur);
    }
}

#define kSearchBody		((String *) -1)

// Key can be NULL which will make us try to find the string everywhere
void FindMessages(Stream *output, Mailbox *mbox, const String *key,
		  const String *string)
{
    Message *msg;
    int numWidth = IntLength(mbox->count);

    if (String_IsEqual(key, &Str_Body, false))
	key = kSearchBody;

    for (msg = Mailbox_Root(mbox); msg != NULL; msg = msg->next) {
	bool found = false;
	Header *head;

	if (key == NULL) {
	    for (head = msg->headers->root; head != NULL; head = head->next) {
		if (String_FoundString(head->value, string, false)) {
		    found = true;
		    break;
		}
	    }
	} else if (key != kSearchBody) {
	    const String *value = Header_Get(msg->headers, key);
	    if (value != NULL)
		found = String_FoundString(value, string, false);
	}

	if (!found && (key == NULL || key == kSearchBody)) {
	    found = String_FoundString(msg->body, string, false);
	}

	if (found) {
	    ListMessage(output, msg->num, numWidth, msg, 0, -1);
	}
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

void DiffMessages(Message *a, Message *b)
{
    Stream *tmpa = SaveTempMessage(a);
    Stream *tmpb = SaveTempMessage(b);
    String *cmd =
	String_PrintF("diff -dc %s %s | %s",
		      String_CString(tmpa->name), String_CString(tmpb->name),
		      gPager != NULL ? String_CString(gPager) : kDefaultPager);

    if (system(String_CString(cmd)) == -1) {
	Error("Could not execute \"%s\": %s",
	      String_CString(cmd), strerror(errno));
    }
    String_Free(cmd);

    Stream_Free(tmpa, true);
    Stream_Free(tmpb, true);
}

int ChooseMessageToDelete(Message *a, Message *b, char *autoChoice)
{
    Stream_PrintF(gStdOut, "\n");

    ListMessage(gStdOut, 1, 1, a, 4, -1);
    ListMessage(gStdOut, 2, 1, b, 4, -1);

    Stream_WriteNewline(gStdOut);

    for (;;) {
	char choice = *autoChoice;

	if (choice == '\0')
	    choice = User_AskChoice("Please choose which message to delete "
				    "(or b(oth), d(iff), or n(either)):",
				    "12bnBNdq", 'n');

	if (isupper(choice))
	    choice = *autoChoice = tolower(choice);

	switch (choice) {
	  case '1':
	    Note("Deleting the first message]");
	    Message_SetDeleted(a, true);
	    return 1;

	  case '2':
	    Note("Deleting the second message]");
	    Message_SetDeleted(b, true);
	    return 1;

	  case 'b':
	    Note("Deleting both messages]");
	    Message_SetDeleted(a, true);
	    Message_SetDeleted(b, true);
	    return 2;

	  case 'd':
	    DiffMessages(a, b);
	    break;

	  case 'n':
	    Note("Deleting no messages]");
	    return 0;

	  case 'q':
	    return -1;
	}
    }
}

void UniqueMailbox(Mailbox *mbox)
{
    Array *mary = SplitMessages(mbox, NULL);
    int i, count = Array_Count(mary);
    int allDups = 0;
    Message *m, *n;
    char autoChoice = '\0';

    SortMessages(mary);

    m = Array_GetAt(mary, 0);
    for (i = 1; i < count; i++, m = n) {
	n = Array_GetAt(mary, i);

	if (!Message_IsDeleted(m) && !Message_IsDeleted(n) &&
	    m->cachedID != NULL && n->cachedID != NULL &&
	    String_IsEqual(m->cachedID, n->cachedID, true)) {
	    static String const *checkKeys[] = {
		&Str_From, &Str_To, &Str_Cc, &Str_Bcc, &Str_Subject, &Str_Date,
		&Str_ResentFrom, &Str_ResentTo, &Str_ResentCc, &Str_ResentBcc,
		&Str_ResentSubject, &Str_ResentDate, &Str_ResentMessageID,
		&Str_XFrom, &Str_XTo, &Str_Xcc, &Str_XSubject, &Str_XDate,
		NULL
	    };
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
		Note("Messages %s and %s with Message-ID\n"
		     " %s are the same, deleting the latter",
		     String_CString(m->tag),
		     String_CString(n->tag),
		     String_PrettyCString(m->cachedID)); 
		Message_SetDeleted(n, true);
		allDups++;

	    } else if (gInteractive) {
		int dups = ChooseMessageToDelete(m, n, &autoChoice);
		if (dups < 0)
		    break;
		allDups += dups;
	    }
	}
    }

    Note("%s %d duplicate%s",
	 allDups == 0 ? "Found" : "Deleted",
	 allDups, allDups == 1 ? "" : "s");

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
    kCmd_Diff,
    kCmd_Edit,
    kCmd_Exit,
    kCmd_Find,
    kCmd_Help,
    kCmd_Join,
    kCmd_List,
    kCmd_ListNext,
    kCmd_ListPrevious,
    kCmd_Repair,
    kCmd_Save,
    kCmd_Show,
    kCmd_ShowPrevious,
    kCmd_ShowNext,
    kCmd_Split,
    kCmd_Strict,
    kCmd_Undelete,
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
    // Don't make help strings be longer than this --------->
    //
    {"+",	NULL,		kCmd_ShowNext,
     "go to the next message and display it"},
    {"-",	NULL,		kCmd_ShowPrevious,
     "go to the previous message and display it"},
    {"check",	"[strict]",	kCmd_Check,
     "check the mailbox' internal consistency"},
    {"delete",	"[<msgs>]",	kCmd_Delete,
     "mark one or more messages as deleted"},
    {"diff",	"<msg1> <msg2>", kCmd_Diff,
     "compare two messages and show the differences"},
    {"dp",	NULL,		kCmd_DeleteAndShowNext,
     "delete the current message, then show the next message"},
    {"edit",	"[<msg>]",	kCmd_Edit,
     "edit the specified message using a file-based editor"},
    {"exit",	NULL,		kCmd_WriteAndExit,
     "save any changes, then leave the mailbox"},
    {"find",	"[<header>:] <string>",	kCmd_Find,
     "find any messages containing the given string"},
    {"headers",	"[<msg>]",	kCmd_List,
     "list a page full of message descriptions"},
    {"list",	"[<msg>]",	kCmd_List,
     "list a page full of message descriptions"},
    {"help",	"[<cmd>]",	kCmd_Help,
     "get help on a specific command or all commands"},
    {"join",	"<msgs>",	kCmd_Join,
     "join messages by replacing them with a single message"},
    {"more",	"[<msgs>]",	kCmd_Show,
     "display the contents of the given message(s)"},
    {"next",	NULL,		kCmd_ShowNext,
     "go to the next message and display it"},
    {"previous", NULL,		kCmd_ShowPrevious,
     "go to the previous message and display it"},
    {"print",	"[<msgs>]",	kCmd_Show,
     "display the contents of the given message(s)"},
    {"quit",	NULL,		kCmd_Exit,
     "leave the mailbox without saving any changes"},
    {"repair",	"[strict]",	kCmd_Repair,
     "check the mailbox' internal state and repair if needed"},
    {"save",	"[<msgs>] <file>", kCmd_Save,
     "save the messages to the given file"},
    {"split",	"[<msgs>]",	kCmd_Split,
     "look for 'From ' lines in the messages and split them"},
    {"strict", "[<on/off>]", kCmd_Strict,
     "set/show 'strict' mode when checking mailboxes"},
    {"undelete", "[<msgs>]",	kCmd_Undelete,
     "undelete one or more messages"},
    {"unique",	NULL,		kCmd_Unique,
     "unique the messages in the mailbox by removing dups"},
#if 0
    {"write",	"[<file>]",	kCmd_Write,
     "write the mailbox back to its own file or the given file"},
#endif
    {"xit",	NULL,		kCmd_Exit,
     "leave the mailbox without saving any changes"},
    {"z",	NULL,		kCmd_ListNext,
     "show the next page of message descriptions"},
    {"z-",	NULL,		kCmd_ListPrevious,
     "show the previous page of message descriptions"},
    {"?",	NULL,		kCmd_Help,
     "get help on a specific command or all commands"},
    {NULL}
};

String *NextArg(int *pIndex, Array *args, bool required)
{
    if (*pIndex < Array_Count(args))
	return Array_GetAt(args, (*pIndex)++);
    if (required)
	Error("Missing argument");
    return NULL;
}

bool NoNextArg(int *pIndex, Array *args)
{
    if (*pIndex < Array_Count(args)) {
	Error("Too many arguments");
	return false;
    }

    return true;
}

int String_ToMessageNumber(const String *str, Mailbox *mbox)
{
    if (String_IsEqual(str, &Str_Dollar, true))
	return Mailbox_Count(mbox);

    return String_ToInteger(str, -1);
}

MessageSet *MessageSetArg(String *arg, int last)
{
    Parser parser;
    MessageSet *set = NULL;

    if (arg == NULL)
	return NULL;

    Parser_Set(&parser, arg);
    if (!Parse_MessageSet(&parser, &set, last) || !Parser_AtEnd(&parser)) {
	Error("Malformed message set: %s", String_CString(arg));
	MessageSet_Free(set);
	return NULL;
    }

    return set;
}

MessageSet *NextMessageSetArgs(int *pIndex, Array *args, int leave,
			       int defNum, int maxNum)
{
    MessageSet *set = NULL;
    int count = Array_Count(args) - *pIndex - leave;

    while (count-- > 0) {
	MessageSet *set2 = MessageSetArg(NextArg(pIndex, args, true), maxNum);
	if (set2 == NULL) {
	    MessageSet_Free(set);
	    return NULL;
	}
	set = MessageSet_Append(set, set2);
    }

    if (set == NULL)
	set = MessageSet_Make(defNum, defNum, NULL);

    return set;
}

#if 0
int NextNumArg(int *pIndex, Array *args, bool required)
{
    String *arg = NextCmdArg(pIndex, args, required);
    int num = -1;

    if (arg != NULL) {
	num = String_ToInteger(arg, -1);
	if (num == -1)
	    Error("Invalid message number");
    }

    return num;
}
#endif

bool TrueString(const String *str, bool def)
{
    static const char *trueStrings[] = {
	"y", "yes", "t", "true", "on", NULL
    };
    const char **tsp;
    const char *cstr = String_CString(str);

    if (cstr == NULL)
	return def;

    for (tsp = trueStrings; *tsp != NULL; tsp++) {
	if (strcasecmp(*tsp, cstr) == 0)
	    return true;
    }

    return false;
}

Message *GetMessageByNumber(Mailbox *mbox, int cur)
{
    Message *msg = NULL;
    int i;

    if (cur > 0)
	for (msg = Mailbox_Root(mbox), i = 1; msg != NULL && i < cur;
	     msg = msg->next, i++);

    if (msg == NULL)
	Error("Message %d does not exist", cur);

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

void ShowHelp(Stream *output, String *cmd)
{
    CommandTable *ct;

    if (cmd == NULL) {
	// Non-specific help
	//
	int pos = 3;

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
	Stream_PrintF(output, "\n\n Enter \"help <cmd>\" for more "
		      "information about a specific command or\n "
		      "\"help all\" for all commands.\n");

    } else {
	bool isAll = String_IsEqual(cmd, &Str_All, false);
	int leftWidth = 0;

	Stream_PrintF(output, " These commands are available:\n");
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

#ifdef USE_READLINE
char *CompleteCommand(const char *prefix, int state)
{
    static int curIndex;
    int prelen = strlen(prefix);

    if (state == 0)
	curIndex = -1;

    while (kCommandTable[++curIndex].cname != NULL) {
	if (strncmp(prefix, kCommandTable[curIndex].cname, prelen) == 0)
	    return strdup(kCommandTable[curIndex].cname);
    }

    return NULL;
}
#endif

void RunLoop(Mailbox *mbox, Array *commands)
{
    jmp_buf reentry;
    int msgCount;
    int cmdCount = Array_Count(commands);
    const String *cmdLine;
    int cur = 1;
    int ci = 0;
    bool done = false;
    bool success;

    InitCommandTable(kCommandTable);
#ifdef USE_READLINE
    rl_completion_entry_function = CompleteCommand;
#endif

    while (!done) {
	setjmp(reentry);
	gInterruptReentry = &reentry;

	if (ci < cmdCount) {
	    cmdLine = Array_GetAt(commands, ci++);
	} else if (!gInteractive || !User_AskLine("@", &cmdLine, true)) {
	    break;
	}

	Array *args = String_Split(cmdLine, ' ', true);

	// Update message count each time around in case
	// the mailbox has been modified
	//
	msgCount = Mailbox_Count(mbox);

	// Find matching command
	//
	int argi = 0;
	const String *arg;
	String *str;
	CommandTable *ct;
	Command cmd = kCmd_None;
	MessageSet *set = NULL;
	Message *msg;
	int count, num;

	if (Array_Count(args) == 0) {
	    // Default to kCmd_ShowNext
	    cmd = kCmd_ShowNext;

	} else {
	    arg = Array_GetAt(args, argi++);

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
		num = String_ToMessageNumber(arg, mbox);
		if (num > 0) {
		    cmd = kCmd_Show;
		    argi--;
		}
	    }
	}

	switch (cmd) {
	  case kCmd_Show:
	    set = NextMessageSetArgs(&argi, args, 0, cur, msgCount);
	    if (set == NULL)
		break;
	    for (num = MessageSet_First(set); num != -1;
		 num = MessageSet_Next(set, num)) {
		msg = GetMessageByNumber(mbox, num);
		if (msg == NULL)
		    break;
		ShowMessage(msg);
		cur = num;
	    }
	    break;

	  case kCmd_ShowPrevious:
	    if (!NoNextArg(&argi, args))
		break;
	    if (cur <= 1) {
		Error("No more messages");
		break;
	    }
	    msg = GetMessageByNumber(mbox, --cur);
	    if (msg != NULL)
		ShowMessage(msg);
	    break;

	  case kCmd_ShowNext:
	  show_next:
	    if (!NoNextArg(&argi, args))
		break;
	    if (cur >= msgCount) {
		Error("No more messages");
		break;
	    }
	    msg = GetMessageByNumber(mbox, ++cur);
	    if (msg != NULL)
		ShowMessage(msg);
	    break;

	  case kCmd_Delete:
	  case kCmd_Undelete:
	    set = NextMessageSetArgs(&argi, args, 0, cur, msgCount);
	    if (set == NULL)
		break;
	    for (num = MessageSet_First(set); num != -1;
		 num = MessageSet_Next(set, num)) {
		msg = GetMessageByNumber(mbox, num);
		if (msg == NULL)
		    break;
		Message_SetDeleted(msg, cmd == kCmd_Delete);
		cur = num;
	    }
	    break;

	  case kCmd_DeleteAndShowNext:
	    if (!NoNextArg(&argi, args))
		break;
	    msg = GetMessageByNumber(mbox, cur);
	    if (msg != NULL) {
		Message_SetDeleted(msg, true);
		goto show_next;
	    }
	    break;

	  case kCmd_Diff: {
	      arg = NextArg(&argi, args, true);
	      if (arg == NULL) break;
	      Message *msg1 =
		  GetMessageByNumber(mbox, String_ToMessageNumber(arg, mbox));
	      if (msg1 == NULL) break;
	      arg = NextArg(&argi, args, true);
	      if (arg == NULL) break;
	      Message *msg2 =
		  GetMessageByNumber(mbox, String_ToMessageNumber(arg, mbox));
	      if (msg2 == NULL) break;

	      DiffMessages(msg1, msg2);
	      break;
	  }

	  case kCmd_List:
	    arg = NextArg(&argi, args, false);
	    if (String_IsEqual(arg, &Str_Minus, true))
		goto list_previous;
	    if (String_IsEqual(arg, &Str_Plus, true))
		goto list_next;
	    if (arg != NULL)
		cur = String_ToMessageNumber(arg, mbox);
	    arg = NextArg(&argi, args, false);
	    if (arg != NULL)
		num = iMax(1, String_ToMessageNumber(arg, mbox) - cur);
	    else
		num = gPageHeight - 1;
	    if (!NoNextArg(&argi, args))
		break;
	    ListMailbox(gStdOut, mbox, cur, num);
	    break;

	  case kCmd_ListNext:
	  list_next:
	    if (!NoNextArg(&argi, args))
		break;
	    cur = iMin(iMax(1, cur) + (gPageHeight - 1), msgCount);
	    ListMailbox(gStdOut, mbox, cur, gPageHeight - 1);
	    break;

	  case kCmd_ListPrevious:
	  list_previous:
	    if (!NoNextArg(&argi, args))
		break;
	    cur = iMax(cur - (gPageHeight - 1), 1);
	    ListMailbox(gStdOut, mbox, cur, gPageHeight - 1);
	    break;

	  case kCmd_Find:
	    // Args are [<header>':'] <string>...
	    arg = NextArg(&argi, args, true);
	    if (String_HasSuffix(arg, &Str_Colon, true)) {
		arg = String_Sub(arg, 0, String_Length(arg) - 1);
	    } else {
		arg = NULL;
		argi--;
	    }
	    str = Array_JoinTail(args, &Str_Space, argi);
	    FindMessages(gStdOut, mbox, arg, str);
	    String_Free((String *) arg);
	    String_Free(str);
	    break;

	  case kCmd_Strict:
	    arg = NextArg(&argi, args, false);
	    if (!NoNextArg(&argi, args))
		break;
	    gStrict = TrueString(arg, !gStrict);
	    Note("Strict checking mode is turned %s",
		 gStrict ? "on" : "off");
	    break;

	  case kCmd_Check:
	    arg = NextArg(&argi, args, false);
	    if (!NoNextArg(&argi, args))
		break;
	    if (arg != NULL && String_HasPrefix(&Str_Strict, arg, false))
		arg = &Str_True;
	    CheckMailbox(mbox, TrueString(arg, gStrict), false);
	    break;

	  case kCmd_Repair:
	    arg = NextArg(&argi, args, false);
	    if (!NoNextArg(&argi, args))
		break;
	    if (arg != NULL && String_HasPrefix(&Str_Strict, arg, false))
		arg = &Str_True;
	    CheckMailbox(mbox, TrueString(arg, gStrict), true);
	    break;

	  case kCmd_Unique:
	    if (!NoNextArg(&argi, args))
		break;
	    UniqueMailbox(mbox);
	    break;

	  case kCmd_Join:
	    if (argi == Array_Count(args)) {
		Error("Missing argument");
		break;
	    }
	    set = NextMessageSetArgs(&argi, args, 0, -1, msgCount);
	    if (set == NULL)
		break;

	    cur = num = MessageSet_First(set);
	    Message *first = GetMessageByNumber(mbox, cur);
	    if (first == NULL)
		break;

	    count = 0;
	    while ((num = MessageSet_Next(set, num)) != -1) {
		msg = GetMessageByNumber(mbox, num);
		if (msg != NULL) {
		    Message_Join(first, msg);
		    count++;
		}
	    }
	    if (count == 0) {
		Error("Please supply multiple messages to join");
	    } else {
		Note("Appended %d message%s onto message %s",
		     count, count == 1 ? "" : "s",
		     String_CString(first->tag));
	    }
	    break;

	  case kCmd_Split:
	    set = NextMessageSetArgs(&argi, args, 0, cur, msgCount);
	    if (set == NULL)
		break;
	    for (num = MessageSet_First(set); num != -1;
		 num = MessageSet_Next(set, num)) {
		msg = GetMessageByNumber(mbox, num);
		if (msg != NULL) {
		    Message_Split(msg, gInteractive);
		    cur = num;
		}
	    }
	    break;

	  case kCmd_Edit:
	    arg = NextArg(&argi, args, false);
	    num = arg != NULL ? String_ToMessageNumber(arg, mbox) : cur;
	    if (!NoNextArg(&argi, args))
		break;
	    msg = GetMessageByNumber(mbox, num);
	    if (msg != NULL) {
		EditMessage(msg);
		cur = num;
	    }
	    break;

	  case kCmd_Save:
	    set = NextMessageSetArgs(&argi, args, 1, cur, msgCount);
	    if (set == NULL)
		break;
	    arg = NextArg(&argi, args, true);
	    if (arg == NULL)
		break;

	    Mailbox *mbox2 = Mailbox_Open(arg, true);
	    if (mbox2 == NULL)
		break;

	    count = 0;
	    for (num = MessageSet_First(set); num != -1;
		 num = MessageSet_Next(set, num)) {
		msg = GetMessageByNumber(mbox, num);
		if (msg != NULL) {
		    Mailbox_Append(mbox2, Message_Clone(msg));
		    cur = num;
		    count++;
		}
	    }
	    success = Mailbox_Save(mbox2, false, false);
	    Mailbox_Free(mbox2);
	    if (success)
		Note("%d message%s saved to %s", count, count == 1 ? "" : "s",
		     String_CString(arg));
	    break;

	  case kCmd_Write:
	    arg = NextArg(&argi, args, false);
	    if (arg == NULL)
		Mailbox_Save(mbox, false, false);
	    else
		Mailbox_Write(mbox, arg, false);
	    break;

	  case kCmd_Exit:
	    if (!NoNextArg(&argi, args))
		break;
	    if (Mailbox_IsDirty(mbox))
		Note("Leaving modified mailbox unsaved");
	    return;

	  case kCmd_WriteAndExit:
	    if (!NoNextArg(&argi, args))
		break;
	    done = true;
	    break;

	  case kCmd_Help:
	    ShowHelp(gStdOut, NextArg(&argi, args, false));
	    break;
	    
	  case kCmd_None:
	    Error("Unknown command: %s", String_CString(arg));
	    break;
	}
	MessageSet_Free(set);
	set = NULL;
    }
    gInterruptReentry = NULL;

    // Autosave if needed
    //
    if (Mailbox_IsDirty(mbox)) {
	if (gDryRun)
	    Note("Dry run mode -- not autosaving modified mailbox");
	else if (true || gAutoWrite ||
		 (gInteractive &&
		  User_AskYesOrNo("Save modified mailbox?", false)))
	    Mailbox_Save(mbox, false, false);
    }
}

bool ProcessFile(String *file, Array *commands, Stream *output)
{
    Mailbox *mbox = Mailbox_Open(file, false);
    
    if (mbox == NULL)
	return false;

    if (!gQuiet || (gQuiet && gVerbose)) {
	int count = Mailbox_Count(mbox);
	String *sizstr = String_ByteSize(String_Length(mbox->data));
	bool oldQuiet = gQuiet;

	gQuiet = false;

	Note("%s: %d message%s, %s",
	     String_CString(file),
	     count, count == 1 ? "" : "s", String_CString(sizstr));

	String_Free(sizstr);
	
	gQuiet = oldQuiet;
    }

    if (gInteractive || Array_Count(commands) > 0)
	RunLoop(mbox, commands);

    if (output != NULL)
	Stream_WriteMailbox(output, mbox, true);

    Mailbox_Free(mbox);
    String_Free(file);

    return true;
}

void Usage(const char *pname, bool help)
{
    const char *p = strrchr(pname, '/');
    if (p != NULL)
	pname = p + 1;

    fprintf(stderr, "Usage: %s [-acdfhinopqruvN] <mbox> ...\n", pname);

    if (help) {
	fprintf(stderr, "\n%s is a mailbox file checking tool.  It will allow "
		"you to check\nyour mbox files' integrity, examine their "
		"contents, and optionally\nperform automatic repairs.\n",
		pname);
	fprintf(stderr, "\nOptions include:\n"
		"  -b \t\tbackup mbox to mbox~ before changing it\n"
		"  -c \t\tcheck the mbox for consistency\n"
		"  -d \t\tdebug mode (see source code)\n"
		"  -f <file> \tprocess mbox <file>\n"
		"  -h \t\tprint out this help text\n"
		"  -i \t\tinitiate interactive mode\n"
		"  -n \t\tdry run -- no changes will be made to any file\n"
		"  -o <file> \tconcatenate messages into <file>\n"
		"  -q \t\tbe quiet and don't report warnings or notices\n"
		"  -r \t\trepair the given mailboxes\n"
		"  -s \t\tbe strict and report more indiscretions than otherwise\n"
		"  -u \t\tunique messages in each mailbox by removing duplicates\n"
		"  -v \t\tbe verbose and print out more progress information\n"
		"  -C \t\tshow a few lines of context around parse errors\n"
		"  -N \t\tdon't try to mmap the mbox file\n"
		"  -V \t\tprint out %s version information and then exit\n",
		pname);
	fprintf(stderr, "\nIf given no options, %s will simply to try read "
		"the given mbox files\nand then quit. ", pname);

	fprintf(stderr, "More interesting usage examples would be:\n\n");
	fprintf(stderr, "%s -c mbox\tto check the mbox file and report "
		"most errors\n", pname);
	fprintf(stderr, "%s -cs mbox\tto check the mbox file and report "
		"more errors\n", pname);
	fprintf(stderr, "%s -rb mbox\tto check the mbox, perform any "
		"necessary repairs, and save\n\t\tthe original file as "
		"mbox~\n", pname);
	fprintf(stderr, "%s -ci mbox\tto check the mbox and then enter an "
		"interactive mode where\n\t\tyou can further inspect it "
		"and make possible changes\n", pname);
	fprintf(stderr, "\nIf you just want to test things out without making "
		"any changes, add the -n\nflag and no files will be "
		"modified.\n");
    } else {
	fprintf(stderr, " (Run \"%s -h\" for more information)\n", pname);
    }

    Exit(EX_USAGE);
}

void ShowVersion(void)
{
    printf("%s (rev %d)\n%s\n", gVersion, kRevision, gCopyright);
}

String *NextMainArg(int *pAC, int argc, char **argv)
{
    return *pAC < argc ? String_FromCString(argv[++*pAC], false) : NULL;
}

void Exit(int ret)
{
    Mailbox_UnlockAll();
    exit(ret);
}

// Add all "unhidden" files at or below path to the given array.
// Returns # of errors.
//
int AddFiles(Array *files, String *path)
{
    const char *cPath = String_CString(path);
    struct stat sbuf;
    int errors = 0;

    if (stat(cPath, &sbuf) != 0) {
	perror(cPath);
	return 1;
    }

    if (S_ISDIR(sbuf.st_mode)) {
	DIR *dir = opendir(cPath);
	struct dirent *de;

	if (dir == NULL) {
	    perror(cPath);
	    return 1;
	}

	while ((de = readdir(dir)) != NULL) {
	    // Ignore ./.. and any other .file
	    if (de->d_name[0] == '.')
		continue;

	    // XXX: Will leak strings here...
	    errors +=
		AddFiles(files, String_PrintF("%s/%s", cPath, de->d_name));
	}

	closedir(dir);

    } else {
	Array_Append(files, path);
    }

    return errors;
}

int main(int argc, char **argv)
{
    gLockedMailboxes = Array_New(0, (Free *) String_Free);

    String *outFile = NULL;
    Stream *output = NULL;
    Array *commands = Array_New(0, (Free *) String_Free);
    Array *files = Array_New(0, (Free *) String_Free);
    int errors = 0;
    int ac, i;

    const char *cPager = getenv("PAGER");

    if (cPager == NULL)
	cPager = "more";

    //gInteractive = isatty(0);
    gPager = String_FromCString(cPager, false);
    gStdOut = Stream_Open(NULL, true, true);

    // We don't care about broken (pager) pipes
    signal(SIGPIPE, SIG_IGN);

    // Handle interrupts
    signal(SIGHUP, InterruptHandler);
    signal(SIGINT, InterruptHandler);
    signal(SIGQUIT, InterruptHandler);
    signal(SIGILL, InterruptHandler);
    signal(SIGABRT, InterruptHandler);
    signal(SIGBUS, InterruptHandler);
    signal(SIGSEGV, InterruptHandler);
    signal(SIGTERM, InterruptHandler);

    // Give usage if no arguments at all were given
    if (argc == 1)
	Usage(argv[0], false);

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
	    } else if (strcmp(opt, "help") == 0) {
		Usage(argv[0], true);
	    } else if (strcmp(opt, "version") == 0) {
		ShowVersion();
		Exit(0);
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
		    if (AddFiles(files, NextMainArg(&ac, argc, argv)) != 0)
			Exit(1);
		    break;
		  case 'h': Usage(argv[0], true); break;
		  case 'i': gInteractive = true; break;
		  case 'l': Array_Append(commands, &Str_List); break;
		  case 'n': gDryRun = true; break;
		  case 'o': outFile = NextMainArg(&ac, argc, argv); break;
		  case 'q': gQuiet = true; break;
		  case 'r': Array_Append(commands, &Str_Repair); break;
		  case 's': gStrict = true; break;
		  case 'u': Array_Append(commands, &Str_Unique); break;
		  case 'v': gVerbose = true; break;
		  case 'w': gAutoWrite= true; break;
		  case 'C': gShowContext = true; break;
		    //case 'L': gWantContentLength = true; break;
		  case 'N': gMap = false; break;
		  case 'V': ShowVersion(); Exit(0); break;
		  default:
		    Usage(argv[0], false);
		}
	    }
	}
    }

    /* Figure out the terminal window size
     */
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1) {
	if (ws.ws_col > 0)
	    gPageWidth = ws.ws_col;
	if (ws.ws_row > 0)
	    gPageHeight = ws.ws_row;
    }

    // There's o height limit if we're not interactive
    if (!gInteractive)
	gPageHeight = -1;

    if (outFile != NULL && !gDryRun) {
	output = Stream_Open(outFile, true, true);
    }

    // The rest should all be mbox files (or directories thereof)
    if (ac < argc) {
	for (; ac < argc; ac++) {
	    errors += AddFiles(files, String_FromCString(argv[ac], false));
	}

	// Default to the user's inbox if no explicit files were given
    } else if (Array_Count(files) == 0) {
	const char *cMail = getenv("MAIL");
	String *mailFile;

	// XXX: FIXME!
	if (cMail != NULL) {
	    mailFile = String_FromCString(cMail, false);
	} else {
	    mailFile = String_PrintF("/var/mail/%s", getenv("LOGNAME"));
	}

	errors += AddFiles(files, mailFile);
    }

    // Process the mbox files
    for (i = 0; i < Array_Count(files); i++) {
	if (!ProcessFile(Array_GetAt(files, i), commands, output))
	    errors++;

	if (gQuiet && gVerbose && gWarnings > 0) {
	    gQuiet = false;
	    Warn("%d warning%s issued",
		 gWarnings, gWarnings == 1 ? " was" : "s were");
	    gWarnings = 0;
	    gQuiet = true;
	}
    }

    if (output != NULL)
	Stream_Free(output, true);

    return errors;
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
