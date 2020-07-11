/************ INCLUDES ************/

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <sys/ioctl.h>

/************ DEFINES ************/

// This expression uses the bitwise AND to strip away the first 3 numbers (0x1f == 00011111)
#define CTRL_KEY(k) ((k) & 0x1f)

/************ DATA ************/

struct editorConfig {
    int termRows;
    int termCols;
    struct termios orig_termios;
};

// Declare E of type editorConfig
struct editorConfig E;

/************ TERMINAL ************/
/*
    This void function is responsible for exiting the program with an error
*/
void die(const char *s) {

    // Reposition cursor at the top left corner 
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    // Print the error number and give a description for it; will print the line that gave the error
    // Non-zero exit status indicates that the program failed (Anything that's not 0 = failed)
    perror(s);
    exit(1);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
        die("tcsetattr");
    }
}


void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
        die("tcgetattr");
    }

    // When program exits, disable raw mode
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;

    // Disable various control characters (CTRL-C, CTRL-O, CTRL-S, CTRL-V, CTRL-Y)
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cflag |=(CS8);
    // Disable newline defaulting to the front of the line
    raw.c_oflag &= ~(OPOST);

    // Set a timeout so that read() will return if it doesn't obtain any output
    // The minimum number of bytes that need to be input here are 0
    // The maximum amount of time to wait before read() returns is 1 1/10 of a second (100ms)
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("tcsetattr");
    }
}

// Wait for one keypress, and return it
char editorReadKey() {
    int nread;
    char c;

    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            die("read");
        }
    }
    return c;
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
        return -1;
    }

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) {
            break;
        }
        if (buf[i] == 'R') {
            break;
        }
        i++;
    }

    buf[i] = '\0';

    // Ensure we receive escape sequence characters (occupying first and second characters of the buffer)
    if (buf[0] != '\x1b' || buf[1] != '[') { 
        return -1;
    }

    // Now we can skip to the third character of the buffer, which are the row and col valus, respectively
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
        return -1;
    }

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize windowSize;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &windowSize) == -1 || (windowSize.ws_col == 0)) {

        // These two escape sequences ensure that the cursor reaches the bottom of the terminal
        // C moves the cursor to the very right, and B moves the cursor to the bottom (C and B are capped to the edge of the terminal)
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            return -1;
        }
        return getCursorPosition(rows, cols);
    }
    else {
        *cols = windowSize.ws_col;
        *rows = windowSize.ws_row;
        return 0;
    }
}

/************ APPEND BUFFER ************/

struct abuf {
    char *b;
    int len;
};

// Constructor for our buffer
#define ABUF_INIT {NULL, 0}

void bufferAppend(struct abuf *ab, const char *s, int len) {
    // Create a block of memory that is the size of our current string + the size of the string we are appending
    char *newChar = realloc(ab -> b, ab -> len + len);

    if (newChar == null) {
        return;
    }
    // Copy the string after the current data stored in our buffer, and update the pointer and length value of buffer
    memcpy(&newChar[ab -> len], s, len);
    ab -> b = newChar;
    ab -> len += len;
}

void bufferFree(struct abuf *ab){
    free(ab -> b);
}

/************ OUTPUT ************/
/* Draw the row starts (~) along the left side of the terminal

*/
void editorDrawRows() {
    int x;
    for (x=0; x<E.termRows; x++){
        bufferAppend(ab, "~", 1);

        // Ensure a ~ is placed on the last line
        if (x < E.termRows - 1){
            bufferAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {

    struct abuf ab = ABUF_INIT;

    // This is an escape sequence
    // \x1b represents the escape character (ASCII 27) 
    // [ is the start of the escape sequence
    // J command indicates to clear the screen (Erase In Display)
    // 2 clears the entire screen (1 would clear until the cursor, 0 would clear the screen from the cursor to the end)
    bufferAppend(&ab, "\x1b[2j", 4);
    bufferAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    bufferAppend(&ab, "\x1b[H", 3);

    write(STDOUT_FILENO, ab.b, ab.len);
    bufferFree(&ab);
}

/************ INPUT ************/

// Waits for a keypress, then handles it
char editorProcessKeypress() {
    char c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2j", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
    }
}

/************ INIT ************/

/*
    Initialize all the fields in the E struct
*/
void initEditor() {
    if (getWindowSize(&E.termRows, &E.termCols) == -1) {
        die("getWindowSize");
    }
}

int main() {
    enableRawMode();
    initEditor();

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}