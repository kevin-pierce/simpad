/************ INCLUDES ************/

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>

/************ DEFINES ************/

// This expression uses the bitwise AND to strip away the first 3 numbers (0x1f == 00011111)
#define CTRL_KEY(k) ((k) & 0x1f)

/************ DATA ************/

struct termios orig_termios;

/************ TERMINAL ************/
/*
    This void function is responsible for exiting the program with an error
*/
void die(const char *s) {

    // Reposition cursor at the top left corner 
    write(STDOUT_FILENO, "\x1b[2j", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    // Print the error number and give a description for it; will print the line that gave the error
    // Non-zero exit status indicates that the program failed (Anything that's not 0 = failed)
    perror(s);
    exit(1);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1) {
        die("tcsetattr");
    }
}


void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        die("tcgetattr");
    }

    // When program exits, disable raw mode
    atexit(disableRawMode);

    struct termios raw = orig_termios;

    // Disable various control characters (CTRL-C, CTRL-O, CTRL-S, CTRL-V, CTRL-Y)
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
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

/************ OUTPUT ************/

void editorDrawRows() {
    int y;
    for (y=0; y<24; y++){
        write(STDOUT_FILENO, "~\r\n", 3);
    }
}

void editorRefreshScreen() {
    // This is an escape sequence
    // \x1b represents the escape character (ASCII 27) 
    // [ is the start of the escape sequence
    // J command indicates to clear the screen (Erase In Display)
    // 2 clears the entire screen (1 would clear until the cursor, 0 would clear the screen from the cursor to the end)
    write(STDOUT_FILENO, "\x1b[2j", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    editorDrawRows();

    write(STDOUT_FILENO, "\x1b[H", 3);
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

int main() {
    enableRawMode();

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}