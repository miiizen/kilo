// UP TO STEP 126
/* Feature test macros */
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

/* Includes */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* Defines */
#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3
// Emulate Ctrl press
#define CTRL_KEY(k) ((k) & 0x1f)

// Editor key aliases - Values higher than the range of char so as not to conflict.
enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/* Data */
typedef struct erow {
    // Row of text in the editor
    int size;
    int rsize;
    char *chars;
    char *render;
} erow;

struct editorConfig {
    // Cursor position
    int cx, cy;
    // Render coordinates
    int rx;
    // Row offset - which row is at the top
    int rowoff;
    // Column offset
    int coloff;
    // Number of rows and columns in terminal
    int screenrows;
    int screencols;
    // Number of rows in file
    int numrows;
    // Array of rows
    erow *row;
    // Dirty flag - has buffer been modified
    int dirty;
    // Name of currently open file
    char *filename;
    // Status message buffer
    char statusmsg[80];
    // Time a message is displayed so it can be removed seconds after
    time_t statusmsg_time;
    // Save original termios config to return to
    struct termios orig_termios;
};

struct editorConfig E;

/* Prototypes */
void editorSetStatusMessage(const char *fmt, ...);

void editorRefreshScreen();

char *editorPrompt(char *prompt, void (*callback)(char *, int));

/* Terminal */
void die(const char *s) {
    /* Clear screen, print error message and exit */
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disableRawMode() {
    /* Set termios config back to original */
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode() {
    /* Make changes to get input directly into the program without having to press enter or ctrl-d etc. */
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
        die("tcgetattr");

    // Call disableRawMode when program returns from main or exit()s
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;

    /* c_lflag - local/misc flags, c_oflag - output flag, c_iflag - input flags
        Switch flags by flipping bits
        NOT(00000000000000000000000000001000) = 11111111111111111111111111110111 OR 00000000000000000000000000001000 = 00000000000000000000000000000000
     ICRNL:
        Turn off translating any carriage returns to newlines. (Fix Ctrl-M)
     IXON:
        Turn off Ctrl-S, Ctrl-Q to pause and resume data transmission
     OPOST:
        Turn off output processing, eg "\r" to "\r\n"
     ECHO:
        Turn off echoing to the terminal 
     ICANON:
        Turn off canonical mode.  Read input byte by byte instead of line by line.
     IEXTEN:
        Turn off Ctrl-V to input literal characters
     ISIG:
        Turn off Ctrl-C, Ctrl-Z signals to stop, pause program
     MISC:
        BRKINT - breaks send Ctrl-C
        INPCK - enables parity checking
        ISTRIP - strips 8th bit of each input byte
        CS8 - bit mask.  Sets character size to 8 with OR operator
     */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    // Set timeout for read
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    // TCSAFLUSH discards leftover bytes from input
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

int editorReadKey() {
    // Wait for a keypress and return it.  Low (terminal) level
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1){
        if(nread == -1 && errno != EAGAIN)
            die("read");
    }

    // Escape sequences
    if(c == '\x1b') {
        char seq[3];

        // Read 2 bytes.  If timeout, user pressed escape
        if(read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        if(read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';

        // [ means escape sequence
        if(seq[0] == '[') {
            // If it's a digit...
            if(seq[1] >= '0' && seq[1] <= '9') {
                // Timeout, user pressed esc
                if(read(STDIN_FILENO, &seq[2], 1) != 1)
                    return '\x1b';
                // Expect tilde after digit for page up/down
                if(seq[2] == '~') {
                    switch (seq[1]) {
                        // Home key + end key can have multiple escape sequences
                        // <esc>[~1 etc.
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                // Next char is a letter
                // <esc>[~A etc.
                switch(seq[1]) {
                    // Up
                    case 'A': return ARROW_UP;
                    // Down
                    case 'B': return ARROW_DOWN;
                    // Right
                    case 'C': return ARROW_RIGHT;
                    // Left
                    case 'D': return ARROW_LEFT;
                    // Home
                    case 'H': return HOME_KEY;
                    // End
                    case 'F': return END_KEY;
                }
            }
        } else if(seq[0] == 'O') {
            // Home or end keys <esc>OH <esc>OF
            switch(seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return '\x1b';
    } else {
        return c;
    }
}

int getCursorPosition(int *rows, int *cols) {
    // Get the cursor position on the screen by using n (device status report, 6 - cursor position)
    // this returns an escape sequence
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;
    // Write to buffer
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        i++;
    }
    buf[i] = '\0';
    // Extract cols and rows
    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
        return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    // Place number of columns and rows into winsize struct.
    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // ioctl can't get window size
        // Move cursor to very bottom right of screen
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;
        return getCursorPosition(rows, cols);
    } else {
        // Success: Set args that were passed in to window size.
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/* Row operations */
void editorUpdateRow(erow *row) {
    // Allocate memory then copy row contents
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++) { 
        if (row->chars[j] == '\t') tabs++;
    }

    free(row->render);
    row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';

            while (idx % KILO_TAB_STOP != 0) {
                row->render[idx++] = ' ';
            }
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorInsertRow(int at, char *s, size_t len) {
    // Validate at is within the file
    if(at < 0 || at > E.numrows)
        return;

    // Allocate memory for line length and copy text to the row.  Append row to the rows array
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len +1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
}

void editorDelRow(int at) {
    // Make sure at is valid (in the file)
    if(at < 0 || at >= E.numrows)
        return;
    // Free memory and remove element from the array
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
    // Validate at is within the length of the line or 1 over (at the end)
    // at is index to insert character at
    if(at < 0 || at > row->size) {
        at = row -> size;
    }
    // Make space for extra character and NULL byte
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    // Increment size and add character
    row->size++;
    row->chars[at] = c;
    // Update row so the new character renders
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
    // Allocate memory for new string
    row->chars = realloc(row->chars, row->size + len + 1);
    // Copy new string
    memcpy(&row->chars[row->size], s, len);
    // Update row size + append null byte
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
    // check if cursor is past the start or end of the line
    if(at < 0 || at >= row->size)
        return;
    // Overwrite deleted characters with those before it
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

int editorRowCxToRx(erow *row, int cx) {
    // Translate tabs to spaces
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t')
        rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
    rx++;
    }
    return rx;
}

int editorRowRxToCx(erow *row, int rx) {
    int cur_rx = 0;
    int cx;
    for(cx = 0; cx < row->size; cx++) {
        // Translate tabs into spaces
        if(row->chars[cx] == '\t')
            cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
        cur_rx++;

        // Once cur_rx hits the end of the rendered line, return the cx value
        if(cur_rx > rx)
            return cx;
    }
    return cx;
}

/* Editor operations */
void editorInsertChar(int c) {
    // Check if cursor is on the tilde after the end of the file
    // Append new row before inserting a character
    if(E.cy == E.numrows) {
        editorInsertRow(E.numrows, "", 0);
    }
    // Insert character and move cursor
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewLine() {
    if(E.cx == 0) {
        // At beginning of line, insert empty row above
        editorInsertRow(E.cy, "", 0);
    } else {
        // Split row, insert row and put characters to the right of the cursor into the new row
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

void editorDelChar() {
    // Check if cursor is past end of the file
    if(E.cy == E.numrows)
        return;
    // Check if cursor is at the start of the first row
    if(E.cx == 0 && E.cy == 0)
        return;

    // Get row and delete character to the left of the cursor
    erow *row = &E.row[E.cy];
    if(E.cx > 0) {
        editorRowDelChar(row, E.cx -1);
        E.cx--;
    } else {
        // Append the contents of the current row to the previous row, then delete current row
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}


/* File I/O */
char *editorRowsToString(int *buflen) {
    // Get length of all lines in file (plus newlines at the end of every line)
    int totlen = 0;
    int j;
    for (j = 0; j < E.numrows; j++) {
        totlen += E.row[j].size + 1;
    }
    *buflen = totlen;

    // Allocate memory for buffer and copy all text into it
    char *buf = malloc(totlen);
    char *p = buf;
    for (j = 0; j< E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editorOpen(char *filename) {
    // Set filename
    free(E.filename);
    E.filename = strdup(filename);

    // Open file
    FILE *fp = fopen(filename, "r");
    if(!fp)
        die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    // Read lines until getlines() return -1 (End of file)
    while((linelen = getline(&line, &linecap, fp)) != -1) {
        // Strip newline/carriage returns
        while(linelen > 0 && (line[linelen -1] == '\n' || line[linelen -1] == '\r'))
            linelen--;
        editorInsertRow(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave() {
    // Prompt user to provide filename if there is not one already
    if(E.filename == NULL) {
        E.filename = editorPrompt("Save as: %s", NULL);
        if(E.filename == NULL) {
            editorSetStatusMessage("Save aborted.");
        }
    }
    // Get text in editor into a buffer
    int len;
    char *buf = editorRowsToString(&len);
    // Write to file (create file if it doesn't exist already with normal permissions)
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if(fd != -1) {
        // Set file's size to the specified length
        if(ftruncate(fd, len) != -1) {
            // Write to file
            if(write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/* Find */
void editorFindCallback(char *query, int key) {
    // -1 if no last match or row match was on
    // Only set this to anything other than -1 when an arrow key is pressed
    static int last_match = -1;

    // 1: search forward, -1: search backward
    static int direction = 1;

    // Stop if user presses enter or escape
    if(key == '\r' || key == '\x1b') {
        // Leave search mode, reset values to initial
        last_match = -1;
        direction = 1;
        return;
    } else if(key == ARROW_RIGHT || key == ARROW_DOWN) {
        // Search forwards
        direction = 1;
    } else if(key == ARROW_LEFT || key == ARROW_UP) {
        // Search backwards
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }

    // You can only search forward if there are no results
    if(last_match == -1)
        direction = 1;
    
    // Index of row we are searching
    int current = last_match;
    // Loop through lines and search for string in each line
    int i;
    for(i = 0; i < E.numrows; i++) {
        // Move forward/back a line
        current += direction;
        // Allow wrap around to beginning of file from end or vice versa
        if(current == -1) {
            current = E.numrows - 1;
        }
        else if(current == E.numrows) {
            current = 0;
        }
        
        erow *row = &E.row[current];

        char *match = strstr(row->render, query);
        if(match) {
            // Set up last match for the next time round
            last_match = current;
            E.cy = current;
            // Move cursor to the start of the result
            E.cx = editorRowRxToCx(row, match - row->render);
            // Scroll result to the top next screen refresh
            E.rowoff = E.numrows;
            break;
        }
    }
}

void editorFind() {
    // Save data to return cursor to original position
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;

    // Get query
    char *query = editorPrompt("Search: %s (ESC/Arrow keys/Enter)", editorFindCallback);
    
    // Free memory
    if(query) {
        free(query);
    } else {
        // Query is null, they pressed escape
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }
}


/* Append buffer */

// Pointer to buffer start and length
struct abuf {
    char *b;
    int len;
};

// Define empty buffer
#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    // Append a character to the buffer

    // Get block of memory that is size of current string plus length of string to append
    char *new = realloc(ab->b, ab->len + len);

    if(new == NULL)
        return;
    // Copy s to the end of the buffer and update length
    memcpy(&new[ab->len], s, len);
    // Update pointer to start of buffer
    ab->b = new;
    ab-> len += len;
}

void abFree(struct abuf *ab) {
    // Destructor, free memory
    free(ab->b);
}


/* Output */
void editorScroll() {
    // Use render cursor values
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }
    // Is cursor above visible window
    if(E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    // Is cursor below visible window
    if(E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    // Is cursor to the left of visible window
    if(E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    // Is cursor to the right of visible window
    if(E.rx >= E.coloff + E.screencols) {
        E.coloff = E.rx - E.screencols + 1;
    }
}

void editorDrawRows(struct abuf *ab) {
    // Draw column of tildes on left side of screen
    int y;
    for(y = 0; y < E.screenrows; y++){
        // If text doesn't fit on one screen
        int filerow = y + E.rowoff;
        if(filerow >= E.numrows) {
            //Draw empty row with a tilde at the start

            // Display welcome message 1/3 way down when an empty file is opened
            if(E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                "Kilo editor -- version %s", KILO_VERSION);
                // Truncate message
                if(welcomelen > E.screencols)
                    welcomelen = E.screencols;
                    // Pad message to centre of screen
                int padding = (E.screencols - welcomelen) / 2;
                if(padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while(padding--)
                    abAppend(ab, " ", 1);

                abAppend(ab, welcome, welcomelen);
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            // Draw row with text in it
            // Subtract column offset for horizontal scrolling
            int len = E.row[filerow].rsize - E.coloff;
            // User scrolled past 0
            if(len < 0)
                len = 0;
            if(len > E.screencols)
                len = E.screencols;
            abAppend(ab, &E.row[filerow].render[E.coloff], len);
        }

        // Clear the row (K - erase in line - erase part of the line.  0 default - erase right from cursor)
        abAppend(ab, "\x1b[K", 3);

        // Don't print \r\n on last line
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab) {
    // Invert colours
    abAppend(ab, "\x1b[7m", 4);

    // Status and row status buffer
    char status[80], rstatus[80];
    // Get length of status containing filename and number of lines, file name and if it has been edited
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        E.filename ? E.filename : "[No Name]", E.numrows,
        E.dirty ? "(modified)" : "");
    // Get current line number
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);
    // Trim length if it goes over the number of columns on the screen
    if(len > E.screencols) {
        len = E.screencols;
    }
    // Add to output buffer
    abAppend(ab, status, len);
    // Generate remaining whitespace on buffer
    while(len < E.screencols) {
        // Right align rstatus by continuing to print spaces until we reach the correct point on the line
        if(E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    // Return to normal text formatting
    abAppend(ab, "\x1b[m", 3);
    // Draw empty newline
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
    // Clear message bar
    abAppend(ab, "\x1b[K", 3);
    // Make sure message will fit the width of screen
    int msglen = strlen(E.statusmsg);
    if(msglen > E.screencols) {
        msglen = E.screencols;
    }
    // Display if the message is less than 5 seconds old
    if(msglen && (time(NULL) - E.statusmsg_time < 5)) {
        abAppend(ab, E.statusmsg, msglen);
    }
}

void editorRefreshScreen() {
    // Write bytes to terminal.
    // \x1b (27) escape character
    // [ follows in escape characters
    editorScroll();

    // Init append buffer
    struct abuf ab = ABUF_INIT;

    //Hide cursor while drawing (25l - cursor off)
    abAppend(&ab, "\x1b[?25l", 6);
    // Reposition cursor (H) to top left corner
    abAppend(&ab, "\x1b[H", 3);
    // Draw row of text
    editorDrawRows(&ab);
    // Draw status bar
    editorDrawStatusBar(&ab);
    // Draw message bar
    editorDrawMessageBar(&ab);
    // Move cursor to current location
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));
    // Show cursor again
    abAppend(&ab, "\x1b[?25h", 6);

    // Write buffer to output
    write(STDOUT_FILENO, ab.b, ab.len);

    // Free memory
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
    /* Variadic function - can take different number of arguments */
    // Deal with multiple arguments:

    // List of arguments passed in
    va_list ap;
    // Pass arg before ... so addresses of ... args can be found
    va_start(ap, fmt);
    // REad format string and call va_arg()
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    // End parsing arguments
    va_end(ap);
    // Set to current time
    E.statusmsg_time = time(NULL);

}

/* Input */
char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
    // Allocate memeory for input buffer
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    // init buffer with a NULL character
    size_t buflen = 0;
    buf[0] = '\0';

    while(1) {
        // Display prompt and refresh screen
        // Prompt should contain %s for buf to be displayed
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        // Get keypress
        int c = editorReadKey();
        if(c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            // Delete character by setting it to null
            if(buflen != 0)
                buf[--buflen] = '\0';
        } else if(c == '\x1b') {
            // User presses escape - cancel save
            editorSetStatusMessage(" ");
            // Call callback function if one is specified
            if(callback)
                callback(buf, c);
            free(buf);
            return NULL;
        } else if(c == '\r') {
            // User pressed enter, return buffer if it is not empty
            if(buflen != 0) {
                editorSetStatusMessage(" ");
                // Call callback if specified
                if(callback)
                    callback(buf, c);
                return buf;
            }
        } else if(!iscntrl(c) && c < 128) {
            // If character is not a control character and is printable
            if(buflen == bufsize - 1) {
                // Reallocate memory if buffer is about to overflow
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            // Add character to string
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
        // Call callback if specified
        if(callback)
            callback(buf, c);
    }

}

void editorMoveCursor(int key) {
    // Move cursor when user presses arrow keys
    // Check if cursor is on an actual line
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch(key) {
    case ARROW_LEFT:
        if (E.cx != 0) {
            E.cx--;
        } else if (E.cy > 0) {
            // Move to end of previous line
            E.cy--;
            E.cx = E.row[E.cy].size;
        }
        break;
        case ARROW_RIGHT:
            if(row && E.cx < row->size) {
                    E.cx++;
            } else if(row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if(E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if(E.cy < E.numrows) {
                E.cy++;
            }
            break;
    }

    // Snap to end of line
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}

void editorProcessKeypress() {
    static int quit_times = KILO_QUIT_TIMES;

    // Handles incoming keypresses
    int c = editorReadKey();

    // Ctrl key combinations
    switch(c) {
        case '\r':
            editorInsertNewLine();
            break;
        
        case CTRL_KEY('q'):
            // Prompt user on unsaved changes
            if(E.dirty && quit_times > 0) {
                editorSetStatusMessage("File has unsaved changes. Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            // clear screen, exit
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            // Go to end of the line
            if(E.cy < E.numrows) {
                E.cx = E.row[E.cy].size;
            }
            break;

        case CTRL_KEY('f'):
            // Find
            editorFind();
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            // Move cursor to the right then press backspace for the delete key
            if(c == DEL_KEY) {
                editorMoveCursor(ARROW_RIGHT);
            }
            editorDelChar();
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                // Position cursor at top/bottom of screen and simulate a screen's worth of up or down arrow presses before refresh
                if(c == PAGE_UP) {
                    E.cy = E.rowoff;
                } else if(c == PAGE_DOWN) {
                    E.cy = E.rowoff + E.screenrows - 1;
                    if(E.cy > E.numrows) {
                        E.cy = E.numrows;
                    }
                }
                // Get number of rows on screen and scroll up that many times.
                int times = E.screenrows;
                while (times--)
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
        
        case CTRL_KEY('l'):
        case '\x1b':
            // Ignore refresh screen and escape sequence
            break;
        
        default:
            editorInsertChar(c);
            break;
    }

    quit_times = KILO_QUIT_TIMES;
}

/* Init */
void initEditor() {
    /* Init all fields in E (editor config) struct */

    // Init cursor values
    E.cx = 0;
    E.cy = 0;
    // Init render cursor values
    E.rx = 0;
    // Init row offset
    E.rowoff = 0;
    // Init column offset
    E.coloff = 0;
    // Init number of rows for editor
    E.numrows = 0;
    // Init current row to null
    E.row = NULL;
    // Init dirty flag - buffer has not been modified
    E.dirty = 0;
    // Init currently open filename to null
    E.filename = NULL;
    // init status message buffer
    E.statusmsg[0] = '\0';
    // Init status message time
    E.statusmsg_time = 0;

    // Set window size
    if(getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
    // Make room for status bar and status message
    E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if(argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-F = find | CTRL-Q = quit");

    while(1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}
