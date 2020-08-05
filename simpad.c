/************ INCLUDES ************/

// Define feature test macros to decide what features to expose
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>

/************ DEFINES ************/

// This expression uses the bitwise AND to strip away the first 3 numbers (0x1f == 00011111)
#define CTRL_KEY(k) ((k) & 0x1f)
#define SIMPAD_VERSION "0.0.1"
#define SIMPAD_TAB_STOP 8
#define SIMPAD_QUIT_TIMES 1

// Replace each instance of the wasd characters with a constant representing the arrow keys
// Add detection for special keypresses that utilize escape sequences
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

// An enum containing the different highlight values in the highlight array 
enum editorHighlight {
    HIGHLIGHT_NORMAL = 0,
    HIGHLIGHT_NUMBER,
    HIGHLIGHT_MATCH
};

/************ DATA ************/

typedef struct editorRow {
    int size;
    int renderSize;
    char *chars;
    char *render; // We can now control how to render tabs
    unsigned char *highlight;
} editorRow;

struct editorConfig {
    int cursorX, cursorY;
    int renderX; // Horizontal coordinate variable (some characters do not only occupy one column space)
    int rowOffset; 
    int colOffset;
    int termRows;
    int termCols;
    int numRows;
    editorRow *row;
    int changed;
    char *fileName;
    char statusMsg[80];
    time_t statusMsg_time;
    struct termios orig_termios;
};

// Declare E of type editorConfig
struct editorConfig E;

/************ FUNCTION PROTOTYPES ************/

// This prototype allows us to call editorSetStatus before it is defined later in the file (due to C's single-pass compilation method)
void editorSetStatusMessage(const char *formatString, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

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
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |=(CS8);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

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
            if (seq[1] >= '0' && seq[1] <= '9'){
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

/************ SYNTAX HIGHLIGHTING ************/

// A function that takes a character and returns true if it is considered a separator character
int isSeparator(int c){
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(editorRow *row){
    row->highlight = realloc(row->highlight, row->renderSize);
    memset(row->highlight, HIGHLIGHT_NORMAL, row->renderSize); // Set all characters in the row array to the default highlight value

    int previousSeparator = 1; // Beginning of a line is considered a separator, defaulted to true
    int i = 0;
    while (i < row->renderSize){
        char c = row->render[i];
        unsigned char previousHighlight = (i > 0) ? row->highlight[i - 1] : HIGHLIGHT_NORMAL;

        // Color all numbers from now on (We can also now read numbers with a decimal)
        if ((isdigit(c) && (previousSeparator || previousHighlight == HIGHLIGHT_NUMBER)) || 
            (c == '.' && previousHighlight == HIGHLIGHT_NUMBER)){
            row->highlight[i] = HIGHLIGHT_NUMBER;
            i++;
            previousSeparator = 0;
            continue;
        }

        previousSeparator = isSeparator(c);
        i++;
    }
}

int editorSyntaxToColor(int highlight){
    switch (highlight){
        case HIGHLIGHT_NUMBER:
            return 31; // Red
        case HIGHLIGHT_MATCH:
            return 34; // Blue
        default:
            return 37; // White 
    }
}

/************ ROW OPERATIONS ************/

// Convert a chars index to a render index, and figure out how many spaces each tabbed space occupies
int editorRowCursorXToRenderX(editorRow *row, int cursorX){
    int renderX = 0;
    int i;
    for (i = 0; i < cursorX; i++){
        if (row->chars[i] == '\t'){
            renderX += (SIMPAD_TAB_STOP - 1) - (renderX % SIMPAD_TAB_STOP);
        }
        renderX++;
    }
    return renderX;
}

int editorRowRenderXToCursorX(editorRow *row, int renderX){
    int cursorRenderX = 0;
    int cursorX;
    for (cursorX = 0; cursorX < row->size; cursorX++){
        if (row->chars[cursorX] == '\t'){
            cursorRenderX += (SIMPAD_TAB_STOP - 1) - (cursorRenderX % SIMPAD_TAB_STOP);
        }
        cursorRenderX++;

        if (cursorRenderX > renderX) return cursorX;
    }
    return cursorX;
}

// Reads the characters from an editorRow to fill the contents of a 
// rendered row (The one to ACTUALLY be displayed)
void editorUpdateRow(editorRow *row){
    int tabs = 0;
    int i;

    for (i = 0; i < row->size; i++) {
        if (row->chars[i] == '\t') tabs++;
    }
    free(row->render);
    row->render = malloc(row->size + tabs*(SIMPAD_TAB_STOP - 1) + 1);

    // Render tabs as multiple spaces
    int index = 0;
    for (i = 0; i < row->size; i++){
        if (row->chars[i] == '\t'){
            row->render[index++] = ' ';
            while(index % SIMPAD_TAB_STOP != 0) {
                row->render[index++] = ' ';
            }
        } else {
            row->render[index++] = row->chars[i];
        } 
    }
    // Index now contains the number of chars copied into row->render
    row->render[index] = '\0';
    row->renderSize = index;

    // Update the highlighted array (All we are doing is updating the array in the event we choose to highlight it)
    editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t length) {
    if (at < 0 || at > E.numRows) return;

    // Multiply num of bytes each row occupies by the number of rows we want
    E.row = realloc(E.row, sizeof(editorRow) * (E.numRows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(editorRow) * (E.numRows - at)); // make room at specified index for the new row

    E.row[at].size = length;
    E.row[at].chars = malloc(length + 1);
    memcpy(E.row[at].chars, s, length);
    E.row[at].chars[length] = '\0';

    E.row[at].renderSize = 0;
    E.row[at].render = NULL;
    E.row[at].highlight = NULL;
    editorUpdateRow(&E.row[at]);

    E.numRows++;
    E.changed++;
}
// Free the memory occupied by the editor row we are freeing
void editorFreeRow(editorRow *row){
    free(row->render);
    free(row->chars);
    free(row->highlight);
}

void editorDeleteRow(int at){
    if (at < 0 || at >= E.numRows) return; // Validate the at index
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(editorRow) * (E.numRows - at - 1)); // Overwrite the deleted row with the rest of the rows that come after it
    E.numRows--; // Decrement the total number of rows by 1
    E.changed++;
}

void editorRowInsertCharacter(editorRow *row, int at, int character) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1); // Overwrite the deleted characters with the ones that come after
    row->size++;
    row->chars[at] = character;

    editorUpdateRow(row);
    E.changed++;
}

void editorRowAppendString(editorRow *row, char *s, size_t length){
    // The empty row's new size is the empty row + the length of the row below it + the null byte
    row->chars = realloc(row->chars, row->size + length + 1); 

    // Copy the row below to the end of the contents of the row we deleted
    memcpy(&row->chars[row->size], s, length);
    row->size += length;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.changed++;
}

void editorRowDeleteCharacter(editorRow *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.changed++;
}

/************ EDITOR OPERATIONS ************/

void editorInsertCharacter(int c) {
    if (E.cursorY == E.numRows) {
        editorInsertRow(E.numRows, "", 0);
    }
    editorRowInsertCharacter(&E.row[E.cursorY], E.cursorX, c);
    E.cursorX++;
}

void editorInsertNewline() {
    // If we are at the beginning of a line
    if (E.cursorX == 0){
        editorInsertRow(E.cursorY, "", 0);
    }
    else {
        editorRow *row = &E.row[E.cursorY];
        editorInsertRow(E.cursorY + 1, &row->chars[E.cursorX], row->size - E.cursorX); // Pass characters in the current row right of the cursor
        row = &E.row[E.cursorY]; // Reassign pointer
        row->size = E.cursorX; // Truncate the row we are on to where the cursor is
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cursorY++;
    E.cursorX = 0; // Move cursor down a line to the start
}

void editorDeleteCharacter() {
    if (E.cursorY == E.numRows) return; // If the cursor is past the end of the file, nothing to delete
    if (E.cursorX == 0 && E.cursorY == 0) return; // If the cursor is at the beginning of the first line, do nothing

    editorRow *row = &E.row[E.cursorY]; // get the row the cursor is on
    if (E.cursorX > 0){
        editorRowDeleteCharacter(row, E.cursorX - 1); // if there is a character to the left, delete it
        E.cursorX--;
    }
    // If the cursor is at the start of a line 
    else {
        E.cursorX = E.row[E.cursorY - 1].size;
        editorRowAppendString(&E.row[E.cursorY - 1], row->chars, row->size); // row points to the row we delete, so we append row->chars to the previous row, then delete it
        editorDeleteRow(E.cursorY);
        E.cursorY--;
    }
}

/************ FILE INPUT/OUTPUT ************/

char *editorRowsToString(int *bufferLen) {
    int totalLen = 0;
    int i;
    // Add up the length of each row, so we know how much memory to allocate
    for (i = 0; i < E.numRows; i++) {
        totalLen += E.row[i].size + 1;
    }
    *bufferLen = totalLen;

    char *buffer = malloc(totalLen);
    char *p = buffer;

    for (i = 0; i < E.numRows; i++){
        memcpy(p, E.row[i].chars, E.row[i].size);
        p += E.row[i].size;
        *p = '\n';
        p++;
    }

    return buffer;
}

// Responsible for opening and reading a file 
void editorOpen(char *fileName) {
    free(E.fileName);
    E.fileName = strdup(fileName);

    FILE *filePointer = fopen(fileName, "r");
    if (!filePointer) {
        die("fopen");
    }
    char *line = NULL;
    size_t lineCap = 0;
    ssize_t lineLength;

    while ((lineLength = getline(&line, &lineCap, filePointer)) != -1){
        while (lineLength > 0 && (line[lineLength - 1] == '\n' || 
                                  line[lineLength - 1] == '\r')) {
            lineLength--;
        }
        editorInsertRow(E.numRows, line, lineLength);
    }
    free(line);
    fclose(filePointer);
    E.changed = 0;
}

void editorSave() {
    // New file (no filename)
    if (E.fileName == NULL) {
        E.fileName = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        if (E.fileName == NULL){
            editorSetStatusMessage("Save aborted!");
            return;
        }
    } 

    int length;
    char *buffer = editorRowsToString(&length);

    // Create a new file and open it (O_CREAT) for reading and writing (O_RDRW) - 0644 is the permissions flag, giving the owner full permission over the file, while every other user can only read the file.
    int newFile = open(E.fileName, O_RDWR | O_CREAT, 0644);

    if (newFile != -1){
        if (ftruncate(newFile, length) != -1) {
            if (write(newFile, buffer, length) == length) {
                close(newFile);
                free(buffer);
                E.changed = 0;
                editorSetStatusMessage("%d bytes written to disk", length); // Status bar will now display whether we succesfully saved or not
                return;
            }
        }
        close(newFile);
    }
    free(buffer);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/************ SEARCH FEATURE ***********/

void editorFindCallback(char *query, int key){

    static int lastMatch = -1; // The prior search result (-1 if no result, or index of the last match row)
    static int direction = 1;  // 1 = down, -1 = up

    static int savedHighlightedLine; // Which line needs to be restored
    static char *savedHighlight = NULL;

    if (savedHighlight) {
        memcpy(E.row[savedHighlightedLine].highlight, savedHighlight, E.row[savedHighlightedLine].renderSize);
        free(savedHighlight);
        savedHighlight = NULL;
    }
    if (key == '\r' || key == '\x1b'){ // User presses enter or escape, in which case they leave search mode
        lastMatch = -1;
        direction = 1;
        return;
    }
    else if (key == ARROW_RIGHT || key == ARROW_DOWN){
        direction = 1;
    }
    else if (key == ARROW_LEFT || key == ARROW_UP){
        direction = -1;
    }
    else {
        lastMatch = -1;
        direction = 1;
    }

    if (lastMatch == -1) {
        direction = 1;
    }
    int current = lastMatch; // Current is the index of the row we are searching 
    int i;

    for (i = 0; i < E.numRows; i++){
        current += direction;
        if (current == -1){
            current = E.numRows - 1;
        }
        else if (current == E.numRows){
            current = 0;
        }

        editorRow *row = &E.row[current];
        char *match = strstr(row->render, query);

        if (match){
            lastMatch = current;
            E.cursorY = current;
            E.cursorX = editorRowRenderXToCursorX(row, match - row->render);
            E.rowOffset = E.numRows;

            // Searched text using Ctrl+F is now highlighted
            savedHighlightedLine = current;
            savedHighlight = malloc(row->renderSize);
            memcpy(savedHighlight, row->highlight, row->renderSize);
            memset(&row->highlight[match - row->render], HIGHLIGHT_MATCH, strlen(query));
            break;
        }
    }
}

void editorFind() {

    int savedCursorX = E.cursorX;
    int savedCursorY = E.cursorY;
    int savedColOffset = E.colOffset;
    int savedRowOffset = E.rowOffset;

    char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);
    if (query) {
        free(query);
    }    
    // Restore original mouse position when search is cancelled
    else {
        E.cursorX = savedCursorX;
        E.cursorY = savedCursorY;
        E.colOffset = savedColOffset;
        E.rowOffset = savedRowOffset;
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
    char *newChar = realloc(ab->b, ab->len + len);

    if (newChar == NULL) {
        return;
    }
    // Copy the string after the current data stored in our buffer, and update the pointer and length value of buffer
    memcpy(&newChar[ab->len], s, len);
    ab -> b = newChar;
    ab -> len += len;
}

void bufferFree(struct abuf *ab){
    free(ab -> b);
}

/************ OUTPUT ************/
/* Draw the row starts (~) along the left side of the terminal, and displays a welcome message upon startup
*/

void editorScroll() {
    E.renderX = 0;
    if (E.cursorY < E.numRows) {
        E.renderX = editorRowCursorXToRenderX(&E.row[E.cursorY], E.cursorX);
    }

    // Above the visible window?
    if (E.cursorY < E.rowOffset){
        E.rowOffset = E.cursorY;
    }
    // Past the bottom of the visible window?
    if (E.cursorY >= E.rowOffset + E.termRows) {
        E.rowOffset = E.cursorY - E.termRows + 1;
    }
    // Left side of the screen
    if (E.renderX < E.colOffset){
        E.colOffset = E.renderX;
    }
    // Right side of the screen
    if (E.renderX >= E.colOffset + E.termCols){
        E.colOffset = E.renderX - E.termCols + 1;
    }
}

void editorDrawRows(struct abuf *ab) {
    int x;
    for (x=0; x<E.termRows; x++){
        int fileRow = x + E.rowOffset;
        if (fileRow >= E.numRows) {
            // The welcome message will only display if our text buffer is empty
            if (E.numRows == 0 && x == E.termRows / 3) {
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
        }
        else {
            // Check if we are drawing a row that is part of the text buffer, or a row that comes after the text buffer
            int len = E.row[fileRow].renderSize - E.colOffset;
            if (len < 0) {
                len = 0;
            }
            if (len > E.termCols) {
                len = E.termCols;
            }
            char *c = &E.row[fileRow].render[E.colOffset];
            unsigned char *highlight = &E.row[fileRow].highlight[E.colOffset];
            int currentColor = -1;

            // Cannot simply feed render substring to print into bufferAppend()
            // We have to loop through each character 
            for (int i = 0; i < len; i++){
                if (highlight[i] == HIGHLIGHT_NORMAL) {
                    if (currentColor != -1) {
                        bufferAppend(ab, "\x1b[39m", 5); // Use the default text colour before printing
                        currentColor = -1; // When we want the default text colour
                    }
                    bufferAppend(ab, &c[i], 1);
                }
                // Set text colour to the value that editorSyntaxToColor() returns
                else {
                    int color = editorSyntaxToColor(highlight[i]);
                    if (color != currentColor) { 
                        currentColor = color;
                        char buf[16];
                        int colorLen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        bufferAppend(ab, buf, colorLen);
                    }
                    bufferAppend(ab, &c[i], 1);
                }
            }
            bufferAppend(ab, "\x1b[39m", 5);
        }
        bufferAppend(ab, "\x1b[K", 3);
        bufferAppend(ab, "\r\n", 2);
    }
}
// Create our status bar that displays file name, type, and num of lines
void editorDrawStatusBar(struct abuf *ab){
    bufferAppend(ab, "\x1b[7m", 4);
    char status[80], renderStatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.fileName ? E.fileName : "[No Name]", E.numRows, E.changed ? "(modified)" : "");
    int renderLen = snprintf(renderStatus, sizeof(renderStatus), "%d/%d", E.cursorY + 1, E.numRows); // Current line number
    // If the status string is too long, cut it short
    if (len > E.termCols) {
        len = E.termCols;
    }
    bufferAppend(ab, status, len);
    // Keep adding spaces until the second status message (the one displaying the current line number is at the very right-edge of the screen)
    while (len < E.termCols) {
        if (E.termCols - len == renderLen) {
            bufferAppend(ab, renderStatus, renderLen);
            break;
        }
        else {
            bufferAppend(ab, " ", 1);
        len++;
        }
    }
    bufferAppend(ab, "\x1b[m", 3);
    bufferAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
    bufferAppend(ab, "\x1b[K", 3);
    int msgLen = strlen(E.statusMsg);
    if (msgLen > E.termCols){
        msgLen = E.termCols;
    }
    if (msgLen && time(NULL) - E.statusMsg_time < 5) {
        bufferAppend(ab, E.statusMsg, msgLen);
    }
}

void editorRefreshScreen() {
    editorScroll();
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
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    // Convert the text cursor position to 1-indexed values
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cursorY - E.rowOffset) + 1, (E.renderX - E.colOffset) + 1);
    bufferAppend(&ab, buf, strlen(buf));

    bufferAppend(&ab, "\x1b[?25l", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    bufferFree(&ab);
}

// ... indicates that this is a variadic function (any num of arguments)
void editorSetStatusMessage(const char *formatString, ...) {
    va_list ap;
    va_start(ap, formatString);
    vsnprintf(E.statusMsg, sizeof(E.statusMsg), formatString, ap);
    va_end(ap);
    E.statusMsg_time = time(NULL);
}

/************ INPUT ************/

// Prompts the user to input a filename when saving a new file 
char *editorPrompt(char *prompt, void (*callback)(char *, int)) { 
    size_t bufferSize = 128;
    char *buffer = malloc(bufferSize);

    size_t bufferLength = 0;
    buffer[0] = '\0'; // User's inputted file name is stored in buffer

    // Infinite loop that infinitely sets the status message and waits for a keypress
    while (1){
        editorSetStatusMessage(prompt, buffer);
        editorRefreshScreen();

        int c = editorReadKey();
        
        // User can delete / backspace characters while in the file name prompt
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (bufferLength != 0) buffer[--bufferLength] = '\0';
        }
        // Allow the user to hit esc to cancel the save-file function
        else if (c == '\x1b'){
            editorSetStatusMessage("");
            if (callback) callback(buffer, c);
            free(buffer);
            return NULL;
        }
        else if (c == '\r'){
            if (bufferLength != 0) {
                editorSetStatusMessage("");
                if (callback) callback(buffer, c);
                return buffer;
            }

        // Ensure an inputted key is in the range of a char (a valid letter / number)
        } else if (!iscntrl(c) && c < 128){
            if (bufferLength == bufferSize - 1) {
                bufferSize *= 2;
                buffer = realloc(buffer, bufferSize);
            }
            buffer[bufferLength++] = c;
            buffer[bufferLength] = '\0';
        }
        if (callback) callback(buffer, c);
    }
}

// Grant control of the mouse cursor using WASD (Will change to arrow keys later)
// Establish bounds that prevent the cursor from moving off the screen
void editorMoveCursor(int key) {
    editorRow *row = (E.cursorY >= E.numRows) ? NULL : &E.row[E.cursorY];

    switch (key) {
        case ARROW_LEFT:
            if (E.cursorX != 0) {
                E.cursorX--;
            } else if (E.cursorY > 0) {
                E.cursorY--;
                E.cursorX = E.row[E.cursorY].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cursorX < row->size) {
                E.cursorX++;
            } else if (row && E.cursorX == row -> size){
                E.cursorY++;
                E.cursorX = 0;
            }
            break;
        case ARROW_UP:
            if (E.cursorY != 0) E.cursorY--;
            break;
        case ARROW_DOWN:
            if (E.cursorY < E.numRows) E.cursorY++;
            break;
    }
    // If at the last position in a line, snap to the end of the next line if we change rows
    row = (E.cursorY >= E.numRows) ? NULL : &E.row[E.cursorY];
    int rowLength = row ? row->size : 0;
    if (E.cursorX > rowLength){
        E.cursorX = rowLength;
    }
}

// Waits for a keypress, then handles it
void editorProcessKeypress() {
    static int quit_times = SIMPAD_QUIT_TIMES;

    int c = editorReadKey();

    switch (c) {
        case '\r':
            editorInsertNewline();
            break;
        // command + q to quit
        case CTRL_KEY('q'):
            if (E.changed && quit_times > 0) {
                editorSetStatusMessage("WARNING - File has unsaved changes. Press Ctrl-Q again to quit.");
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        // command + s to save
        case CTRL_KEY('s'):
            editorSave();
            break;
        
        // Use Fn + left arrow 
        case HOME_KEY:
            E.cursorX = 0;
            break;

        // Use Fn + right arrow to bring cursor to end of line
        case END_KEY:
            if (E.cursorY < E.numRows) {
                E.cursorX = E.row[E.cursorY].size;
            }
            break;

        // Search in-text function
        case CTRL_KEY('f'):
            editorFind();
            break;
        
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDeleteCharacter();
            break;

        // Simulate the function of a page_up / page_down function by sending the cursor
        // to the top or bottom of our terminal window
        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP){
                    E.cursorY = E.rowOffset;
                } else if (c == PAGE_DOWN) {
                    E.cursorY = E.rowOffset + E.termRows - 1;
                    if (E.cursorY > E.termRows) {
                        E.cursorY = E.numRows;
                    }
                }
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

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        // The default case will allow any keypress not mapped to some function to be inserted into the text
        default:
            editorInsertCharacter(c);
            break;
    }
    quit_times = SIMPAD_QUIT_TIMES; // Reset back to 2 if the user presses any other key
}

/************ INIT ************/

/*
    Initialize all the fields in the E struct
*/
void initEditor() {

    // Coordinates of the cursor in rows and columns
    E.cursorX = 0;
    E.cursorY = 0;
    E.renderX = 0;
    E.rowOffset = 0; // Scroll to top of file by default
    E.colOffset = 0;
    E.numRows = 0;
    E.row = NULL;
    E.changed = 0; // Flag that tells us if a file has been changed since its last save
    E.fileName = NULL;
    E.statusMsg[0] = '\0';
    E.statusMsg_time = 0;

    if (getWindowSize(&E.termRows, &E.termCols) == -1) {
        die("getWindowSize");
    }
    E.termRows -= 2; // Make space for a status bar + status message
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-Q = quit | Ctrl-S = save | Ctrl-F = find");

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}