// UP TO STEP 46

#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <string.h>

/* Defines */
#define KILO_VERSION "0.0.1"
// Emulate Ctrl press
#define CTRL_KEY(k) ((k) & 0x1f)

// Editor key aliases - Values higher than the range of char so as not to conflict.
enum editorKey {
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
struct editorConfig {
    // Cursor position
    int cx, cy;
    // Number of rows and columns in terminal
    int screenrows;
    int screencols;
    // Save original termios config to return to
    struct termios orig_termios;
};

struct editorConfig E;

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
void editorDrawRows(struct abuf *ab) {
    // Draw column of tildes on left side of screen
    int y;
    for(y = 0; y < E.screenrows; y++){
        // Display welcome message 1/3 way down
        if(y == E.screenrows / 3) {
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

        // Clear the row (K - erase in line - erase part of the line.  0 default - erase right from cursor)
        abAppend(ab, "\x1b[K", 3);

        // Don't print \r\n on last line
        if(y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {
    // Write bytes to terminal.
    // \x1b (27) escape character
    // [ follows in escape characters

    // Init append buffer
    struct abuf ab = ABUF_INIT;

    //Hide cursor while drawing (25l - cursor off)
    abAppend(&ab, "\x1b[?25l", 6);
    // Reposition cursor (H) to top left corner
    abAppend(&ab, "\x1b[H", 3);
    // Draw tildes
    editorDrawRows(&ab);
    // Move cursor to current location
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy +1, E.cx + 1);
    abAppend(&ab, buf, strlen(buf));
    // Show cursor again
    abAppend(&ab, "\x1b[?25h", 6);

    // Write buffer to output (25h - cursor on)
    write(STDOUT_FILENO, ab.b, ab.len);

    // Free memory
    abFree(&ab);
}

/* Input */
void editorMoveCursor(int key) {
    // Move cursor when user presses arrow keys
    switch(key) {
        case ARROW_LEFT:
            if(E.cx != 0){
                E.cx--;
            }
            break;
        case ARROW_RIGHT:
            if(E.cx != E.screencols - 1) {
                E.cx++;
            }
            break;
        case ARROW_UP:
            if(E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if(E.cy != E.screenrows - 1) {
                E.cy++;
            }
            break;
    }
}

void editorProcessKeypress() {
    // Handles incoming keypresses
    int c = editorReadKey();

    // Ctrl key combinations
    switch(c) {
        case CTRL_KEY('q'):
            // clear screen, exit
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            E.cx = E.screencols - 1;
            break;
        
        case PAGE_UP:
        case PAGE_DOWN:
            {
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
    }
}

/* Init */
void initEditor() {
    // Init all fields in E (editor config) struct

    // Init cursor values
    E.cx = 0;
    E.cy = 0;
    // Set window size
    if(getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
}

int main() {
    enableRawMode();
    initEditor();

    while(1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}