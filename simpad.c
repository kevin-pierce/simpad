#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>

struct termios orig_termios;

void disableRawMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enableRawMode() {
    tcgetattr(STDIN_FILENO, &orig_termios);

    // When program exits, disable raw mode
    atexit(disableRawMode);

    struct termios raw = orig_termios;

    // Disable various control characters (CTRL-C, CTRL-O, CTRL-S, CTRL-V, CTRL-Y)
    raw.c_lflag &= ~(IXON | ICRNL);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main() {
    enableRawMode();

    char c;
    while ((read(STDIN_FILENO, &c, 1) == 1) && (c != 'q')) {
        
        // Check if the char is a control character (will not print, so print ASCII code instead)
        if (iscntrl(c)) {
            printf("%d\n", c);
        } 
        // Print the ASCII code of the char, as well as the char itself
        else {
            printf("%d ('%c')\n", c, c);
        }
    }
    return 0;
}