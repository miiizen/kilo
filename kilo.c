#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

/* Data */
// Save original termios config to return to
struct termios orig_termios;

/* Terminal */
void die(const char *s) {
    /* Print error message and exit */
    perror(s);
    exit(1);
}

void disableRawMode() {
    /* Set termios config back to original */
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode() {
    /* Make changes to get input directly into the program without having to press enter or ctrl-d etc. */
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
        die("tcgetattr");

    // Call disableRawMode when program returns from main or exit()s
    atexit(disableRawMode);

    struct termios raw = orig_termios;

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

/* Init */
int main() {
    enableRawMode();

    while(1) {
        char c = '\0';
        if(read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) die("read");
        // Is the character a control character...
        if(iscntrl(c)) {
            printf("%d\r\n", c);
        } else {
            // Print the character and it's ascii code
            printf("%d ('%c')\r\n", c, c);
        }
        if(c == 'q') break;
    }
    return 0;
}