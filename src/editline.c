/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Copyright 1992,1993 Simmule Turner and Rich Salz.  All rights reserved.
 *
 * This software is not subject to any license of the American Telephone
 * and Telegraph Company or of the Regents of the University of California.
 *
 * Permission is granted to anyone to use this software for any purpose on
 * any computer system, and to alter it and redistribute it freely, subject
 * to the following restrictions:
 * 1. The authors are not responsible for the consequences of use of this
 *    software, no matter how awful, even if they arise from flaws in it.
 * 2. The origin of this software must not be misrepresented, either by
 *    explicit claim or by omission.  Since few users ever read sources,
 *    credits must appear in the documentation.
 * 3. Altered versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.  Since few users
 *    ever read sources, credits must appear in the documentation.
 * 4. This notice may not be removed or altered.
 */

/*

Defines TODO

advanced commands (search, emacs, etc)

static memory (fixed max line length)
 + history depth

pass line char array to readline


*/

/*
**  Main editing routines for editline library.
*/
#include "editline.h"
#include <ctype.h>
#include <unistd.h>

/*
**  Manifest constants.
*/
#define SCREEN_WIDTH    80
#define SCREEN_ROWS     24
#define NO_ARG          (-1)
#define DEL             127
#define CTL(x)          ((x) & 0x1F)
#define ISCTL(x)        ((x) && (x) < ' ')
#define UNCTL(x)        ((x) + 64)
#define META(x)         ((x) | 0x80)
#define ISMETA(x)       ((x) & 0x80)
#define UNMETA(x)       ((x) & 0x7F)
#if     !defined(HIST_SIZE)
#define HIST_SIZE       20
#endif  /* !defined(HIST_SIZE) */

/*
**  Command status codes.
*/
typedef enum _STATUS {
    CSdone, CSeof, CSmove, CSdispatch, CSstay, CSsignal
} STATUS;

/*
**  The type of case-changing to perform.
*/
typedef enum _CASE {
    TOupper, TOlower
} CASE;

/*
**  Key to command mapping.
*/
typedef struct _KEYMAP {
    CHAR        Key;
    STATUS      (*Function)();
} KEYMAP;

/*
**  Command history structure.
*/
typedef struct _HISTORY {
    int         Size;
    int         Pos;
    CHAR        *Lines[HIST_SIZE];
} HISTORY;

/*
**  Globals.
*/
static const unsigned rl_eof = 0x04;
static const unsigned rl_erase = 0x7F;
static const unsigned rl_intr = 0x03;
static const unsigned rl_kill = 0x15;
static const unsigned rl_quit = 0x1C;

STATIC CHAR             NIL[] = "";
STATIC CONST CHAR       *Input = NIL;
STATIC CHAR             *Line;
STATIC CONST char       *Prompt;
STATIC char             *Screen;
STATIC CONST char       NEWLINE[]= CRLF;
STATIC HISTORY          H;
STATIC int              Repeat;
STATIC int              End;
STATIC int              Mark;
STATIC int              OldPoint;
STATIC int              Point;
STATIC int              PushBack;
STATIC int              Pushed;
FORWARD CONST KEYMAP    Map[26];
STATIC SIZE_T           Length;
STATIC SIZE_T           ScreenCount;
STATIC SIZE_T           ScreenSize;
STATIC char             *backspace;

/* Display print 8-bit chars as `M-x' or as the actual 8-bit char? */
int             rl_meta_chars = 0;

/*
**  Declarations.
*/
STATIC CHAR     *editinput();
#if     defined(USE_TERMCAP)
#include <stdlib.h>
#include <curses.h>
#include <term.h>
#endif  /* defined(USE_TERMCAP) */

/*
**  TTY input/output functions.
*/


STATIC void
TTYflush()
{
    SIZE_T s;
    if (ScreenCount) {
        /* Dummy assignment avoids GCC warning on
         * "attribute warn_unused_result" */
//        ssize_t dummy = write(1, Screen, ScreenCount);
//        (void)dummy;

        for (s = 0; s < ScreenCount; s++)
          _putchar(Screen[s]);

        ScreenCount = 0;
    }
}

STATIC void
TTYput(c)
    CHAR        c;
{
    Screen[ScreenCount] = c;
    if (++ScreenCount >= ScreenSize - 1) {
        ScreenSize += SCREEN_INC;
        RENEW(Screen, char, ScreenSize);
    }
}

STATIC void
TTYputs(p)
    CONST CHAR  *p;
{
    while (*p)
        TTYput(*p++);
}

STATIC void
TTYshow(c)
    CHAR        c;
{
    if (c == DEL) {
        TTYput('^');
        TTYput('?');
    }
    else if (ISCTL(c)) {
        TTYput('^');
        TTYput(UNCTL(c));
    }
    else if (rl_meta_chars && ISMETA(c)) {
        TTYput('M');
        TTYput('-');
        TTYput(UNMETA(c));
    }
    else
        TTYput(c);
}

STATIC void
TTYstring(p)
    CHAR        *p;
{
    while (*p)
        TTYshow(*p++);
}

STATIC unsigned int
TTYget()
{
    TTYflush();
    if (Pushed) {
        Pushed = 0;
        return PushBack;
    }
    if (*Input)
        return *Input++;
    return _getchar(); //read(0, &c, (SIZE_T)1) == 1 ? c : EOF;
}

#define TTYback()       (backspace ? TTYputs((CHAR *)backspace) : TTYput('\b'))

STATIC void
TTYbackn(n)
    int         n;
{
    while (--n >= 0)
        TTYback();
}

STATIC void
reposition()
{
    int         i;
    CHAR        *p;

    TTYput('\r');
    TTYputs((CONST CHAR *)Prompt);
    for (i = Point, p = Line; --i >= 0; p++)
        TTYshow(*p);
}

STATIC void
left(Change)
    STATUS      Change;
{
    TTYback();
    if (Point) {
        if (ISCTL(Line[Point - 1]))
            TTYback();
        else if (rl_meta_chars && ISMETA(Line[Point - 1])) {
            TTYback();
            TTYback();
        }
    }
    if (Change == CSmove)
        Point--;
}

STATIC void
right(Change)
    STATUS      Change;
{
    TTYshow(Line[Point]);
    if (Change == CSmove)
        Point++;
}

STATIC STATUS
ring_bell()
{
    TTYput('\07');
    TTYflush();
    return CSstay;
}

STATIC void
ceol()
{
    int         extras;
    int         i;
    CHAR        *p;

    for (extras = 0, i = Point, p = &Line[i]; i <= End; i++, p++) {
        TTYput(' ');
        if (ISCTL(*p)) {
            TTYput(' ');
            extras++;
        }
        else if (rl_meta_chars && ISMETA(*p)) {
            TTYput(' ');
            TTYput(' ');
            extras += 2;
        }
    }

    for (i += extras; i > Point; i--)
        TTYback();
}

STATIC STATUS
insert_string(p)
    CHAR        *p;
{
    SIZE_T      len;
    int         i;
    CHAR        *new;
    CHAR        *q;

    len = strlen((char *)p);
    if (End + len >= Length) {
        if ((new = NEW(CHAR, Length + len + MEM_INC)) == NULL)
            return CSstay;
        if (Length) {
            COPYFROMTO(new, Line, Length);
            DISPOSE(Line);
        }
        Line = new;
        Length += len + MEM_INC;
    }

    for (q = &Line[Point], i = End - Point; --i >= 0; )
        q[len + i] = q[i];
    COPYFROMTO(&Line[Point], p, len);
    End += len;
    Line[End] = '\0';
    TTYstring(&Line[Point]);
    Point += len;

    return Point == End ? CSstay : CSmove;
}

STATIC STATUS
redisplay()
{
    TTYputs((CONST CHAR *)NEWLINE);
    TTYputs((CONST CHAR *)Prompt);
    TTYstring(Line);
    return CSmove;
}

STATIC CHAR *
next_hist()
{
    return H.Pos >= H.Size - 1 ? NULL : H.Lines[++H.Pos];
}

STATIC CHAR *
prev_hist()
{
    return H.Pos == 0 ? NULL : H.Lines[--H.Pos];
}

STATIC STATUS
do_insert_hist(p)
    CHAR        *p;
{
    if (p == NULL)
        return ring_bell();
    Point = 0;
    reposition();
    ceol();
    End = 0;
    return insert_string(p);
}

STATIC STATUS
do_hist(move)
    CHAR        *(*move)();
{
    CHAR        *p;
    int         i;

    i = 0;
    do {
        if ((p = (*move)()) == NULL)
            return ring_bell();
    } while (++i < Repeat);
    return do_insert_hist(p);
}

STATIC STATUS
h_next()
{
    return do_hist(next_hist);
}

STATIC STATUS
h_prev()
{
    return do_hist(prev_hist);
}

STATIC STATUS
fd_char()
{
    int         i;

    i = 0;
    do {
        if (Point >= End)
            break;
        right(CSmove);
    } while (++i < Repeat);
    return CSstay;
}

STATIC STATUS
delete_string(count)
    int         count;
{
    int         i;
    CHAR        *p;

    if (count <= 0 || End == Point)
        return ring_bell();

    if (count == 1 && Point == End - 1) {
        /* Optimize common case of delete at end of line. */
        End--;
        p = &Line[Point];
        i = 1;
        TTYput(' ');
        if (ISCTL(*p)) {
            i = 2;
            TTYput(' ');
        }
        else if (rl_meta_chars && ISMETA(*p)) {
            i = 3;
            TTYput(' ');
            TTYput(' ');
        }
        TTYbackn(i);
        *p = '\0';
        return CSmove;
    }
    if (Point + count > End && (count = End - Point) <= 0)
        return CSstay;

    for (p = &Line[Point], i = End - (Point + count) + 1; --i >= 0; p++)
        p[0] = p[count];
    ceol();
    End -= count;
    TTYstring(&Line[Point]);
    return CSmove;
}

STATIC STATUS
bk_char()
{
    int         i;

    i = 0;
    do {
        if (Point == 0)
            break;
        left(CSmove);
    } while (++i < Repeat);

    return CSstay;
}

STATIC STATUS
bk_del_char()
{
    int         i;

    i = 0;
    do {
        if (Point == 0)
            break;
        left(CSmove);
    } while (++i < Repeat);

    return delete_string(i);
}

STATIC STATUS
kill_line()
{
    int         i;

    if (Repeat != NO_ARG) {
        if (Repeat < Point) {
            i = Point;
            Point = Repeat;
            reposition();
            (void)delete_string(i - Point);
        }
        else if (Repeat > Point) {
            right(CSmove);
            (void)delete_string(Repeat - Point - 1);
        }
        return CSmove;
    }

    Line[Point] = '\0';
    ceol();
    End = Point;
    return CSstay;
}

STATIC STATUS
insert_char(c)
    int         c;
{
    STATUS      s;
    CHAR        buff[2];
    CHAR        *p;
    CHAR        *q;
    int         i;

    if (Repeat == NO_ARG || Repeat < 2) {
        buff[0] = c;
        buff[1] = '\0';
        return insert_string(buff);
    }

    if ((p = NEW(CHAR, Repeat + 1)) == NULL)
        return CSstay;
    for (i = Repeat, q = p; --i >= 0; )
        *q++ = c;
    *q = '\0';
    Repeat = 0;
    s = insert_string(p);
    DISPOSE(p);
    return s;
}

STATIC STATUS
meta()
{
    unsigned int        c;

    if ((int)(c = TTYget()) == EOF)
        return CSeof;
    /* Also include VT-100 arrows. */
    if (c == '[' || c == 'O') {
        c = TTYget();
        switch (c) {
        case EOF:       return CSeof;
        case 'A':       return h_prev();
        case 'B':       return h_next();
        case 'C':       return fd_char();
        case 'D':       return bk_char();
        default: 
          return ring_bell();
        }
    }

    return ring_bell();
}

STATIC STATUS
emacs(c)
    unsigned int        c;
{
    STATUS              s;
    const KEYMAP        *kp;

    if (rl_meta_chars && ISMETA(c)) {
        Pushed = 1;
        PushBack = UNMETA(c);
        return meta();
    }
    for (kp = Map; kp->Function; kp++)
        if (kp->Key == c)
            break;
    s = kp->Function ? (*kp->Function)() : insert_char((int)c);
    if (!Pushed)
        /* No pushback means no repeat count; hacky, but true. */
        Repeat = NO_ARG;
    return s;
}

STATIC STATUS
TTYspecial(c)
    unsigned int        c;
{
    if (ISMETA(c))
        return CSdispatch;

    if (c == rl_erase || (int)c == DEL)
        return bk_del_char();
    if (c == rl_kill) {
        if (Point != 0) {
            Point = 0;
            reposition();
        }
        Repeat = NO_ARG;
        return kill_line();
    }
    if (c == rl_eof && Point == 0 && End == 0)
        return CSeof;
    if (c == rl_intr) {
        return CSsignal;
    }
    if (c == rl_quit) {
        return CSeof;
    }

    return CSdispatch;
}

STATIC CHAR *
editinput()
{
    unsigned int        c;

    if (_waitchar(0) == 0)
        return NULL;

    c = _getchar();

    switch (TTYspecial(c)) {
        case CSdone:
            return Line;
        case CSeof:
            return NULL;
        case CSsignal:
            return (CHAR *)"";
        case CSmove:
            reposition();
            break;
        case CSdispatch:
            switch (emacs(c)) {
            case CSdone:
                return Line;
            case CSeof:
                return NULL;
            case CSsignal:
                return (CHAR *)"";
            case CSmove:
                reposition();
                break;
            case CSdispatch:
            case CSstay:
                break;
            }
            break;
        case CSstay:
            break;
    }
    TTYflush();
    return NULL;
}

STATIC void
hist_add(p)
    CHAR        *p;
{
    int         i;

    if ((p = (CHAR *)strdup((char *)p)) == NULL)
        return;
    if (H.Size < HIST_SIZE)
        H.Lines[H.Size++] = p;
    else {
        DISPOSE(H.Lines[0]);
        for (i = 0; i < HIST_SIZE - 1; i++)
            H.Lines[i] = H.Lines[i + 1];
        H.Lines[i] = p;
    }
    H.Pos = H.Size - 1;
}

/*
**  For compatibility with FSF readline.
*/
/* ARGSUSED0 */
void
rl_reset_terminal(p)
    char        *p;
{
    (void)p;
}

void
rl_initialize()
{
}

char *
readline(void)
{
    CHAR        *line;

    if ((line = editinput()) != NULL) {
    //    line = (CHAR *)strdup((char *)line);
        TTYputs((CONST CHAR *)NEWLINE);
        TTYflush();
        DISPOSE(Screen);
        DISPOSE(H.Lines[--H.Size]);
        return (char *)line;
    }

    return NULL;
}

void
resetline(char *prompt)
{
    if (Line == NULL) {
        Length = MEM_INC;
        if ((Line = NEW(CHAR, Length)) == NULL)
            return NULL;
    }

    Repeat = NO_ARG;
    OldPoint = Point = Mark = End = 0;
    Line[0] = '\0'; 

    hist_add(NIL);
    ScreenSize = SCREEN_INC;
    Screen = NEW(char, ScreenSize);
    Prompt = prompt ? prompt : (char *)NIL;
    TTYputs((CONST CHAR *)Prompt); TTYflush();
}

void
add_history(p)
    char        *p;
{
    if (p == NULL || *p == '\0')
        return;

#if     defined(UNIQUE_HISTORY)
    if (H.Size && strcmp(p, (char *)H.Lines[H.Size - 1]) == 0)
        return;
#endif  /* defined(UNIQUE_HISTORY) */
    hist_add((CHAR *)p);
}


STATIC STATUS
beg_line()
{
    if (Point) {
        Point = 0;
        return CSmove;
    }
    return CSstay;
}

STATIC STATUS
del_char()
{
    return delete_string(Repeat == NO_ARG ? 1 : Repeat);
}

STATIC STATUS
end_line()
{
    if (Point != End) {
        Point = End;
        return CSmove;
    }
    return CSstay;
}

STATIC STATUS
accept_line()
{
    Line[End] = '\0';
    return CSdone;
}

STATIC STATUS
transpose()
{
    CHAR        c;

    if (Point) {
        if (Point == End)
            left(CSmove);
        c = Line[Point - 1];
        left(CSstay);
        Line[Point - 1] = Line[Point];
        TTYshow(Line[Point - 1]);
        Line[Point++] = c;
        TTYshow(c);
    }
    return CSstay;
}

STATIC STATUS
wipe()
{
    int         i;

    if (Mark > End)
        return ring_bell();

    if (Point > Mark) {
        i = Point;
        Point = Mark;
        Mark = i;
        reposition();
    }

    return delete_string(Mark - Point);
}

STATIC STATUS
move_to_char()
{
    unsigned int        c;
    int                 i;
    CHAR                *p;

    if ((int)(c = TTYget()) == EOF)
        return CSeof;
    for (i = Point + 1, p = &Line[i]; i < End; i++, p++)
        if (*p == c) {
            Point = i;
            return CSmove;
        }
    return CSstay;
}

STATIC CONST KEYMAP Map[26] = {
    {   CTL('@'),       ring_bell       },
    {   CTL('A'),       beg_line        },
    {   CTL('B'),       bk_char         },
    {   CTL('D'),       del_char        },
    {   CTL('E'),       end_line        },
    {   CTL('F'),       fd_char         },
    {   CTL('G'),       ring_bell       },
    {   CTL('H'),       bk_del_char     },
    {   CTL('J'),       accept_line     },
    {   CTL('K'),       kill_line       },
    {   CTL('L'),       redisplay       },
    {   CTL('M'),       accept_line     },
    {   CTL('N'),       h_next          },
    {   CTL('O'),       ring_bell       },
    {   CTL('P'),       h_prev          },
    {   CTL('Q'),       ring_bell       },
    {   CTL('S'),       ring_bell       },
    {   CTL('T'),       transpose       },
    {   CTL('U'),       ring_bell       },
    {   CTL('W'),       wipe            },
    {   CTL('Z'),       ring_bell       },
    {   CTL('['),       meta            },
    {   CTL(']'),       move_to_char    },
    {   CTL('^'),       ring_bell       },
    {   CTL('_'),       ring_bell       },
    {   0,              NULL            }
};

