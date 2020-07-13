/************ INCLUDES ************/

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>

/************ DEFINES ************/

// This expression uses the bitwise AND to strip away the first 3 numbers (0x1f == 00011111)
#define CTRL_KEY(k) ((k) & 0x1f)

#define SIMPAD_VERSION "0.0.1"

// Replace each instance of the wasd characters with a constant representing the arrow keys
// Add detection for special keypresses that utilize escape sequences
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

/************ DATA ************/

struct editorConfig {
    int cursorX, cursorY;
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
int editorReadKey() {
    int nread;
    char c;

    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            die("read");
        }
    }
    
    if (c == '\x1b') {
        char seq[3];

        // If we read an escapse seuqnce character ([) then we immedaitely read the next two bytes
        if (read(STDIN_FILENO, &seq[0], 1) != 1) {
            return '\x1b';
        }
        if (read(STDIN_FILENO, &seq[1], 1) != 1) {
            return '\x1b';
        }

        
        if (seq[0] == '[') {

            if (seq[1] >= '0' && seq[1] <= '9' ){
                if (read(STDIN_FILENO, &seq[2], 1) != 1) {
                    return '\x1b';
                }
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;

                        // Fn + Backspace to simulate the del key
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            }
            else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }  
            }     
        }

        else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    }
    else {
        return c;
    }
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

    if (newChar == NULL) {
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
/* Draw the row starts (~) along the left side of the terminal, and displays a welcome message upon startup
*/
void editorDrawRows(struct abuf *ab) {
    int x;
    for (x=0; x<E.termRows; x++){
        if (x == E.termRows / 3) {

            // Define variable for welcome message
            char welcomeMsg[80];

            int welcomeLen = snprintf(welcomeMsg, sizeof(welcomeMsg), "Simpad Editor -- Version %s", SIMPAD_VERSION);
            
            if (welcomeLen > E.termCols) {
                welcomeLen = E.termCols;
            }
            // Centering the welcome message
            int padding = (E.termCols - welcomeLen) / 2;
            if (padding) {
                bufferAppend(ab, "~", 1);
                padding --;
            }
            while (padding--) {
                bufferAppend(ab, " ", 1);
            }
            bufferAppend(ab, welcomeMsg, welcomeLen);
        }
        else {
            bufferAppend(ab, "~", 1);
        }

        bufferAppend(ab, "\x1b[K", 3);
        // Ensure a ~ is placed on the last line
        if (x < E.termRows - 1){
            bufferAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {

    // Initialize new buffer
    struct abuf ab = ABUF_INIT;

    // This is an escape sequence
    // \x1b represents the escape character (ASCII 27) 
    // [ is the start of the escape sequence
    // J command indicates to clear the screen (Erase In Display)
    // 2 clears the entire screen (1 would clear until the cursor, 0 would clear the screen from the cursor to the end)
    bufferAppend(&ab, "\x1b[?25l", 6);
    bufferAppend(&ab, "\x1b[H", 3);

    // This function can now use bufferAppend
    editorDrawRows(&ab);

    // Convert the text cursor position to 1-indexed values
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cursorY + 1, E.cursorX + 1);
    bufferAppend(&ab, buf, strlen(buf));

    bufferAppend(&ab, "\x1b[?25l", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    bufferFree(&ab);
}

/************ INPUT ************/

// Grant control of the mouse cursor using WASD (Will change to arrow keys later)
// Establish bounds that prevent the cursor from moving off the screen
void editorMoveCursor(int key) {
    switch (key) {
        case ARROW_LEFT:
            if (E.cursorX != 0) E.cursorX--;
            break;
        case ARROW_RIGHT:
            if (E.cursorX != E.termCols - 1) E.cursorX++;
            break;
        case ARROW_UP:
            if (E.cursorY != 0) E.cursorY--;
            break;
        case ARROW_DOWN:
            if (E.cursorY != E.termRows - 1) E.cursorY++;
            break;
    }
}

// Waits for a keypress, then handles it
void editorProcessKeypress() {
    int c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2j", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        
        // Use Fn + left arrow 
        case HOME_KEY:
            E.cursorX = 0;
            break;

        // Use Fn + right arrow
        case END_KEY:
            E.cursorX = E.termCols - 1;
            break;

        // Simulate the function of a page_up / page_down function by sending the cursor
        // to the top or bottom of our terminal window
        case PAGE_UP:
        case PAGE_DOWN:
            {
                int times = E.termRows;
                while (times--) {
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP: ARROW_DOWN);
                }
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
    }

    
}

/************ INIT ************/

/*
    Initialize all the fields in the E struct
*/
void initEditor() {

    // Coordinates of the cursor in rows and columns
    E.cursorX = 0;
    E.cursorY = 0;

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